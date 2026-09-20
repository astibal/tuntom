#include "../src/info_message.hpp"
#include "../src/info_worker.hpp"
#include "../src/session.hpp"
#include "../src/cli.hpp"
#include <iostream>
#include <sstream>
#include <atomic>
#include <future>
#include <thread>
#include <poll.h>

using namespace tuntom;
using Wire = std::vector<std::uint8_t>;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

int main() {
    // Selection uses only interface loopback flag, IPv4 family and !127/8.
    sockaddr_in ip[7] {};
    ifaddrs interfaces[8] {};
    const std::uint32_t values[] {0x7f000001, 0x7ffffffe, 0x0a000002, 0xc0000201,
                                  0x0a000002, 0x0a000003, 0x00000000};
    for (std::size_t i = 0; i < 8; ++i) {
        interfaces[i].ifa_flags = IFF_LOOPBACK;
        if (i < 7) {
            ip[i].sin_family = AF_INET;
            ip[i].sin_addr.s_addr = htonl(values[i]);
            interfaces[i].ifa_addr = reinterpret_cast<sockaddr*>(&ip[i]);
            interfaces[i].ifa_next = &interfaces[i + 1];
        }
    }
    interfaces[3].ifa_flags = IFF_UP; // Non-loopback, even with a non-127 address.
    ip[5].sin_family = AF_INET6;
    const info::Addresses expected {0, 0x0a000002};
    require(info::loopback_addresses(interfaces) == expected, "loopback selection/deduplication");
    require(info::loopback_addresses(nullptr).empty(), "no interfaces");
    auto payload = info::encode_access(expected);
    const auto bytes = [](const std::string& value) { return Wire(value.begin(), value.end()); };
    info::Fields decoded;
    require(info::decode(payload, decoded) && decoded == info::Fields{{"access", "0.0.0.0,10.0.0.2"}}, "access text");
    require(info::decode(info::encode_access({}), decoded) && decoded == info::Fields{{"access", ""}}, "empty access");
    require(info::decode(bytes("access=10.0.0.1,10.0.0.2\ncustom= \tHello, world=x \t\n"), decoded) &&
            decoded == info::Fields{{"access", "10.0.0.1,10.0.0.2"}, {"custom", "Hello, world=x"}}, "opaque values/trim/first equal");
    require(info::decode(bytes("access=not an IP\nempty=\nspaces= \t "), decoded) &&
            decoded.at("access") == "not an IP" && decoded.at("empty").empty() && decoded.at("spaces").empty(), "no value heuristics");
    const auto preserved = decoded;
    for (const auto* bad : {"", "\n", "access=x\n\n", "access=x\nmissing", "=x", "access=x\naccess=y",
                           "1key=x", "Key=x", "a-b=x", "a.b=x", " a=x", "a =x", "a\t=x", "a=x\r\n"}) {
        require(!info::decode(bytes(bad), decoded) && decoded == preserved, "malformed snapshot changed state");
    }
    // Check every byte independently, including NUL, DEL, BOM/UTF-8 constituents.
    for (unsigned value = 0; value < 256; ++value) {
        auto candidate = bytes("custom=left");
        candidate.push_back(static_cast<std::uint8_t>(value));
        candidate.insert(candidate.end(), {'r', 'i', 'g', 'h', 't'});
        const bool allowed = (value >= 0x20 && value <= 0x7e) || value == 9;
        require(info::decode(candidate, decoded) == allowed, "ASCII whitelist");
    }
    require(!info::decode(bytes("\xef\xbb\xbfkey=x"), decoded), "BOM accepted");
    require(info::decode(bytes("a=" + std::string(info::max_payload - 2, 'x')), decoded), "maximum payload rejected");
    require(!info::decode(bytes("a=" + std::string(info::max_payload - 1, 'x')), decoded), "oversized payload accepted");
    const auto sanitized = info::encode({{"custom", " \xc4\x8d\xff \t"}, {"empty", ""}});
    require(sanitized == bytes("custom=???\nempty=\n"), "sender byte sanitization/trim");
    for (const auto& fields : {info::Fields{}, info::Fields{{"bad-key", "x"}},
                              info::Fields{{"\xc4\x8d", "x"}}, info::Fields{{"a", "x\nb=y"}},
                              info::Fields{{"a", std::string(1, '\0')}}, info::Fields{{"a", "x\r"}},
                              info::Fields{{"a", std::string(1, '\x7f')}},
                              info::Fields{{"a", std::string(info::max_payload, 'x')}}}) {
        bool rejected = false;
        try { (void)info::encode(fields); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "sender accepted invalid field");
    }
    info::PeerSnapshot snapshot;
    auto stats = [&] { std::ostringstream out; snapshot.write_stats(out); return out.str(); };
    snapshot.activated(100);
    require(stats() == "info_msg_peer_received=0\n", "initial peer stats");
    require(snapshot.accept(100, 2, bytes("access=10.0.0.1\ncustom= hi, there \nreceived=x\n")), "snapshot accepted");
    const auto before = stats();
    require(before == "info_msg_peer_received=1\npeer_info_access=10.0.0.1\npeer_info_custom=hi, there\npeer_info_received=x\n", "stats prefix/custom keys");
    require(!snapshot.accept(100, 3, bytes("access=x\nbad")) && stats() == before, "invalid update not atomic");
    require(!snapshot.accept(100, 1, bytes("access=old")) && stats() == before, "older snapshot replaced latest");
    require(snapshot.accept(100, 3, bytes("access=")) && stats() == "info_msg_peer_received=1\npeer_info_access=\n", "snapshot replacement removed absent keys");
    require(snapshot.accept(200, 1, bytes("other=x")), "INFO before ACK");
    snapshot.activated(200);
    require(stats() == "info_msg_peer_received=1\npeer_info_other=x\n", "ACK erased current INFO");
    snapshot.activated(300);
    require(stats() == "info_msg_peer_received=0\n", "rekey retained previous-session metadata");
    require(!Options{}.info_msg_enable, "INFO must default off");
    Options options; char flag[] = "--info-msg-enable"; char* argv[] {flag};
    parse_options(1, argv, 0, options);
    require(options.info_msg_enable, "INFO flag ignored");

    auto parse = [](std::vector<std::string> arguments) {
        std::vector<char*> pointers;
        for (auto& argument : arguments) pointers.push_back(argument.data());
        Options result;
        parse_options(static_cast<int>(pointers.size()), pointers.data(), 0, result);
        return result;
    };
    const auto custom = parse({"--info-field=site= \xc4\x8d ", "--info-field", "note=left=right, text", "--info-field=empty="});
    require(!custom.info_msg_enable, "custom fields unexpectedly enabled sending");
    require(info::decode(info::encode_access({0x0a000001}, custom.info_fields), decoded) &&
            decoded == info::Fields{{"access", "10.0.0.1"}, {"site", "??"}, {"note", "left=right, text"}, {"empty", ""}}, "admin fields/sanitization");
    for (const auto& arguments : std::vector<std::vector<std::string>>{
             {"--info-field"}, {"--info-field="}, {"--info-field=missing"}, {"--info-field==x"},
             {"--info-field=Bad=x"}, {"--info-field=access=10.0.0.1"},
             {"--info-field=x=1", "--info-field", "x=2"}, {"--info-field=x=a\nb=c"},
             {"--info-field=x=" + std::string(info::max_payload - 3, 'a')}}) {
        bool rejected = false;
        try { (void)parse(arguments); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "invalid CLI field accepted");
    }

    // Collection runs off the event loop and hands over one complete text buffer.
    const auto main_thread = std::this_thread::get_id();
    std::atomic<unsigned> collections {0};
    auto take = [&](info::Worker& worker) {
        pollfd descriptor {worker.fd(), POLLIN, 0};
        require(::poll(&descriptor, 1, 3000) == 1, "worker completion timeout");
        info::Worker::Result result;
        require(worker.take(result), "worker signaled without snapshot");
        return result;
    };
    {
        info::Worker worker([&] {
            require(std::this_thread::get_id() != main_thread, "collection ran on event loop");
            const auto number = ++collections;
            if (number == 2) throw std::runtime_error("enumeration failed");
            return info::encode({{"sample", std::to_string(number)}});
        });
        worker.request(10);
        auto result = take(worker);
        require(result.valid && result.generation == 10 &&
                Wire(result.text.begin(), result.text.begin() + result.size) == bytes("sample=1\n"), "worker snapshot");
        worker.request(20);
        result = take(worker);
        require(!result.valid && result.size == 0 && result.generation == 20, "worker reused stale data after failure");
        worker.request(30);
        result = take(worker);
        require(result.valid && result.generation == 30 && collections == 3, "worker did not recover/recollect");
    }
    {
        std::promise<void> entered, release;
        auto started = entered.get_future();
        auto released = release.get_future();
        unsigned calls = 0;
        info::Worker worker([&] {
            if (++calls == 1) {
                entered.set_value();
                released.wait_for(std::chrono::seconds(2));
            }
            return bytes("access=fresh");
        });
        worker.request(1);
        require(started.wait_for(std::chrono::seconds(2)) == std::future_status::ready, "collector did not start");
        worker.request(2);
        release.set_value();
        const auto result = take(worker);
        require(result.valid && result.generation == 2 && calls == 2, "obsolete collection published after rekey");
    }
    {
        info::Worker worker([&] { return bytes("access=x\nmalformed"); });
        worker.request(1);
        require(!take(worker).valid, "worker published malformed text buffer");
    }

    ascon::key_type key {};
    using SP = SessionProtocol;
    auto now = SP::Time{} + std::chrono::seconds(100);
    constexpr std::int64_t wall = 100000;
    for (bool encrypted : {false, true}) {
        SP client(42, key, false, 1500, encrypted, 300, encrypted);
        SP server(42, key, true, 1500, encrypted, 300, encrypted);
        Packet packet; Wire scratch;
        auto recv = [&](SP& target, const Wire& wire) {
            return target.receive(wire.data(), wire.size(), packet, scratch, now, wall);
        };
        auto establish = [&] {
            auto response = recv(server, client.begin(now, wall)).reply;
            auto confirm = recv(client, response).reply;
            auto result = recv(server, confirm);
            require(result.activated, "server activation");
            require(recv(client, result.reply).activated, "client activation");
            require(!recv(server, confirm).activated, "duplicate confirmation activation");
        };
        establish();
        Packet message; message.type = PacketType::info; message.payload = payload;
        auto old = client.encode(message);
        auto wire = client.encode(message);
        require(wire.size() == 25 + payload.size(), "INFO header");
        require(recv(server, wire).data && packet.type == PacketType::info && packet.payload == payload, "session INFO roundtrip");
        require(recv(server, wire).replay_drop, "INFO replay accepted");
        for (std::size_t i = 0; i < wire.size(); ++i) {
            auto bad = wire; bad[i] ^= 1;
            require(!recv(server, bad).data, "tampered INFO accepted");
        }
        message.payload = bytes("access=x\nmalformed");
        require(!recv(server, client.encode(message)).data, "authenticated malformed INFO accepted");
        now += std::chrono::seconds(1);
        establish();
        require(!recv(server, old).data, "old session INFO accepted during grace period");
        message.payload = info::encode_access({0xc000020a});
        require(recv(server, client.encode(message)).data && packet.payload == message.payload, "fresh rekey INFO");
        message.payload = info::encode_access({});
        require(recv(server, client.encode(message)).data, "empty rekey INFO");
    }
    std::cout << "PASS: INFO selection, ASCII fields, atomic snapshots, opt-in, authentication, replay and rekey\n";
}
