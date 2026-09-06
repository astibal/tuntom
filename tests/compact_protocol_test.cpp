#include "../src/protocol.hpp"
#include "../src/fragmentation.hpp"
#include "../src/reassembly.hpp"
#include <iostream>
using namespace tuntom;
using Wire = std::vector<std::uint8_t>;
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }

int main() {
    log_level = LogLevel::quiet;
    ascon::key_type key {};
    for (bool encrypted : {false, true}) {
        auto c2s = ascon::derive_direction_key(key, 42, true);
        auto s2c = ascon::derive_direction_key(key, 42, false);
        ProtocolV5 sender(42, c2s, s2c, encrypted), receiver(42, s2c, c2s, encrypted);
        for (auto type : {PacketType::data, PacketType::hello, PacketType::keepalive,
                          PacketType::ping, PacketType::pong, PacketType::mtu_probe,
                          PacketType::mtu_reply, PacketType::confirm, PacketType::confirm_ack}) {
            Packet p;
            p.type = type; p.sequence = 0xabcd000000000001ULL; p.message_id = 99;
            if (type == PacketType::data) { p.payload.resize(20, 0x45); p.original_length = 20; }
            if (type == PacketType::hello || type == PacketType::keepalive) p.message_id = 0;
            if (type == PacketType::mtu_probe || type == PacketType::mtu_reply) p.original_length = 1400;
            if (type == PacketType::confirm || type == PacketType::confirm_ack) p.sequence &= ~0x0000ffffffffffffULL;
            const auto wire = sender.encode(p);
            const std::size_t expected = (type == PacketType::data || type == PacketType::hello ||
                type == PacketType::keepalive) ? 25 :
                (type == PacketType::mtu_probe || type == PacketType::mtu_reply) ? 35 : 33;
            require(wire.size() == expected + p.payload.size(), "compact control/data size");
            require(load_be64(wire.data() + 1) == p.sequence, "sequence offset");
            Packet out;
            require(receiver.decode(wire.data(), wire.size(), out), "compact roundtrip");
            require(out.tunnel_id == 42 && out.payload == p.payload, "implicit tunnel/payload");
            // No declared payload length for DATA: truncation still fails authentication.
            for (std::size_t n = 0; n < wire.size(); ++n)
                require(!receiver.decode(wire.data(), n, out), "truncated packet accepted");
            for (unsigned mask : {0x10, 0x20, 0x40, 0x80}) {
                auto bad = wire; bad[0] ^= static_cast<std::uint8_t>(mask);
                require(!receiver.decode(bad.data(), bad.size(), out), "changed flags accepted");
            }
            auto changed_id = p; changed_id.tunnel_id = 255;
            require(sender.encode(changed_id) == wire, "Packet tunnel ID leaked onto wire");
        }
        // Largest representable original packet, including a tail at offset 65534.
        Packet p; p.sequence = 1; p.message_id = 100; p.original_length = 65535;
        p.payload.assign(65534, 42);
        auto first = sender.encode(p);
        require(first.size() == 37 + p.payload.size() && (first[0] & 0x40), "fragment header");
        p.sequence = 2; p.fragment_offset = 65534; p.payload.assign(1, 43);
        auto last = sender.encode(p);
        require(load_be16(last.data() + 17) == 65534 && load_be16(last.data() + 19) == 65535,
                "16-bit boundary");
        Packet out; Reassembler reassembly(65535); Wire complete;
        require(receiver.decode(last.data(), last.size(), out) && !reassembly.accept(out, complete), "tail first");
        require(receiver.decode(first.data(), first.size(), out) && reassembly.accept(out, complete), "large reassembly");
        require(complete.size() == 65535 && complete.back() == 43, "large reassembly bytes");
        p.original_length = 65536;
        bool rejected = false;
        try { sender.encode(p); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "length silently narrowed");
        p.original_length = 65535; p.fragment_offset = 65536;
        rejected = false;
        try { sender.encode(p); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "offset silently narrowed");
        // PMTUD's padding must include its own 35-byte header on either transport.
        for (std::size_t ip_header : {20U, 40U}) {
            Packet probe; probe.type = PacketType::mtu_probe; probe.sequence = 3;
            probe.message_id = 11; probe.original_length = 1400;
            probe.payload.resize(1400 - ip_header - udp_header_size - protocol_probe_v5_size);
            require(sender.encode(probe).size() + ip_header + udp_header_size == 1400, "PMTUD wire MTU");
        }
    }
    // The 12-byte saving must avoid fragmentation all the way to the outer MTU.
    for (std::size_t ip_header : {20U, 40U}) {
        const auto capacity = 1400 - ip_header - udp_header_size - protocol_fragment_v5_size;
        for (std::size_t size : {capacity, capacity + 1, capacity + 12, capacity + 13, std::size_t(65535)}) {
            const auto plan = make_v5_fragment_plan(size, capacity);
            require((plan.count == 1) == (size <= capacity + 12), "premature fragmentation");
            std::size_t total = 0;
            for (std::size_t i = 0; i < plan.count; ++i) {
                const auto payload = plan.base_size + (i < plan.larger_fragments ? 1 : 0);
                const auto header = plan.count == 1 ? 25 : 37;
                require(payload + header + ip_header + udp_header_size <= 1400, "fragment exceeds outer MTU");
                total += payload;
            }
            require(total == size, "fragment plan loses bytes");
        }
    }
    // Unsupported handshake versions fail even with a valid authentication tag.
    ProtocolV5 sender(42, key, false), receiver(42, key, true), wrong_id(43, key, true);
    Packet init; init.type = PacketType::init; init.message_id = 42; init.payload.resize(44);
    auto wire = sender.encode(init);
    Packet out;
    require(wire.size() == 78 && wire[9] == 5, "handshake version position");
    require(!wrong_id.decode(wire.data(), wire.size(), out), "configuration ID not bound to key");
    for (unsigned version : {0, 4, 6, 255}) {
        auto bad = wire; bad[9] = static_cast<std::uint8_t>(version);
        Wire input(bad.begin(), bad.begin() + 18);
        input.insert(input.end(), bad.begin() + 34, bad.end());
        ascon::tag_type tag {};
        ascon::mac(ascon::derive_direction_key(key, 42, true), 42, input.data(), input.size(), tag);
        std::copy(tag.begin(), tag.end(), bad.begin() + 18);
        require(!receiver.decode(bad.data(), bad.size(), out), "unsupported authenticated version");
    }
    std::cout << "PASS: compact layouts, implicit tunnel, flags, truncation, 16-bit boundaries, PMTUD and handshake version\n";
}
