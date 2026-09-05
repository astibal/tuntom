#include "../src/session.hpp"
#include "../src/cli.hpp"
#include <iostream>
using namespace tuntom;
using Wire = std::vector<std::uint8_t>;
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
const auto now = SessionProtocol::Time{} + std::chrono::seconds(100);
ascon::key_type key {};
SessionProtocol::Received recv(SessionProtocol& s, const Wire& w, Packet& p) {
    Wire scratch;
    return s.receive(w.data(), w.size(), p, scratch, now);
}
void run(bool encrypted, bool pfs = false) {
    SessionProtocol c(42, key, false, 1500, encrypted, 300, pfs), s(42, key, true, 1500, encrypted, 300, pfs);
    Packet decoded;
    auto init = c.begin(now);
    require(load_be16(init.data() + 88) == (pfs ? 2 : (encrypted ? 1 : 0)), "INIT suite");
    auto response = recv(s, init, decoded).reply;
    auto confirm = recv(c, response, decoded).reply;
    require(recv(c, response, decoded).reply == confirm, "confirm retransmission");
    auto ack = recv(s, confirm, decoded).reply;
    require(recv(s, confirm, decoded).reply == ack, "ack retransmission");
    require(recv(c, ack, decoded).activated, "activation");
    SessionProtocol::Session* server_session = nullptr;
    for (auto type : {PacketType::data, PacketType::hello, PacketType::keepalive,
                      PacketType::ping, PacketType::pong, PacketType::mtu_probe,
                      PacketType::mtu_reply}) {
        Packet p;
        p.tunnel_id = 42; p.type = type;
        p.message_id = (type == PacketType::hello || type == PacketType::keepalive) ? 0 : 99;
        p.original_length = type == PacketType::data ? 1500 :
            (type == PacketType::mtu_probe || type == PacketType::mtu_reply) ? 1400 : 0;
        if (type == PacketType::data || type == PacketType::mtu_probe) p.payload.resize(1324, 0x45);
        auto wire = c.encode(p);
        require(wire.size() == p.payload.size() + 48, "wire overhead");
        require(bool(wire[7] & 128) == encrypted, "mode flag");
        if (encrypted && !p.payload.empty())
            require(!std::equal(p.payload.begin(), p.payload.end(), wire.begin() + 48), "plaintext exposed");
        for (std::size_t i = 0; i < wire.size(); ++i) {
            auto bad = wire; bad[i] ^= 1;
            require(!recv(s, bad, decoded).data, "tampering accepted");
        }
        auto downgrade = wire; downgrade[7] ^= 128;
        require(!recv(s, downgrade, decoded).data, "mode flag downgrade");
        require(!recv(c, wire, decoded).data, "reflection");
        auto r = recv(s, wire, decoded);
        require(r.data && decoded.payload == p.payload, "round trip");
        server_session = r.session;
        require(!recv(s, wire, decoded).data, "replay");
        auto reverse = s.encode(p);
        require(recv(c, reverse, decoded).data && decoded.payload == p.payload, "reverse direction");
    }
    Packet p; p.tunnel_id = 42; p.message_id = 123; p.original_length = 1500;
    p.payload.resize(1000, 42);
    auto head = c.encode(p);
    p.fragment_offset = 1000; p.payload.assign(500, 7);
    auto tail = c.encode(p);
    Wire complete;
    auto r = recv(s, tail, decoded);
    require(r.data && !r.session->reassembly.accept(decoded, complete), "tail");
    r = recv(s, head, decoded);
    require(r.data && r.session->reassembly.accept(decoded, complete), "reassembly");
    require(complete.size() == 1500 && complete[999] == 42 && complete[1000] == 7, "reassembled bytes");
    SessionProtocol restarted(42, key, true, 1500, encrypted, 300, pfs);
    require(!recv(restarted, head, decoded).data, "restart replay");
    require(!recv(restarted, init, decoded).reply.empty(), "restart handshake");
    require(!recv(restarted, confirm, decoded).activated, "old confirmation");
    server_session->next_counter = encrypted ? 0xffffffffULL : SessionProtocol::counter_mask;
    require(!s.encode(p).empty() && s.encode(p).empty() && !s.ready(), "nonce counter limit");
}
int main() {
    log_level = LogLevel::quiet;
    run(false); run(true); run(true, true);
    for (bool encrypted : {false, true}) {
        SessionProtocol c(42, key, false, 1500, encrypted), s(42, key, true, 1500, !encrypted);
        Packet p;
        auto init = c.begin(now);
        require(recv(s, init, p).reply.empty(), "mismatched INIT suite");
        SessionProtocol matching(42, key, true, 1500, encrypted);
        auto response = recv(matching, init, p).reply;
        ProtocolV4 decoder(42, key, false), signer(42, key, true);
        require(decoder.decode(response.data(), response.size(), p), "response decoding");
        store_be16(p.payload.data() + 64, encrypted ? 0 : 1);
        auto mismatched = signer.encode(p);
        require(recv(c, mismatched, p).reply.empty(), "mismatched RESPONSE suite");
    }
    for (const char* legacy : {"--allow-v1", "--allow-v2"}) {
        Options options;
        const char* args[] = {"tuntom", "--encrypt-ascon", legacy};
        bool threw = false;
        try { parse_options(3, const_cast<char**>(args), 1, options); }
        catch (const std::runtime_error&) { threw = true; }
        require(threw, "legacy encryption flags accepted");
    }
    for (const char* value : {"0", "1", "301", "86402", "-2", "abc", "300s"}) {
        Options options;
        const char* args[] = {"tuntom", "--init-window", value};
        bool threw = false;
        try { parse_options(3, const_cast<char**>(args), 1, options); }
        catch (const std::exception&) { threw = true; }
        require(threw, "invalid INIT window accepted");
    }
    {
        Options options;
        require(options.init_window == 300, "default INIT window");
        const char* args[] = {"tuntom", "--init-window", "600"};
        parse_options(3, const_cast<char**>(args), 1, options);
        require(options.init_window == 600, "custom INIT window");
        bool threw = false;
        try { parse_options(2, const_cast<char**>(args), 1, options); }
        catch (const std::exception&) { threw = true; }
        require(threw, "missing INIT window accepted");
    }
    Options options;
    const char* args[] = {"tuntom", "--encrypt-ascon"};
    parse_options(2, const_cast<char**>(args), 1, options);
    require(options.encrypt_ascon, "CLI option");
    std::cout << "PASS: encrypted/plain sessions, controls, fragments, mode mismatch, tampering, replay, restart, limits and CLI\n";
}
