#include "../src/session.hpp"
#include "../src/cli.hpp"
#include <iostream>
#include <stdexcept>
using namespace tuntom;
using Wire = std::vector<std::uint8_t>;
using SP = SessionProtocol;
const auto start = SP::Time{} + std::chrono::seconds(100);
constexpr std::int64_t wall = 100000;
ascon::key_type key {};
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
SP::Received recv(SP& s, const Wire& w, SP::Time now = start) {
    Packet p; Wire scratch;
    return s.receive(w.data(), w.size(), p, scratch, now, wall);
}
Wire data(SP& s) {
    Packet p; p.type = PacketType::data; p.tunnel_id = 42;
    p.message_id = 7; p.original_length = 4; p.payload = {0x45, 1, 2, 3};
    return s.encode(p);
}
struct Pair {
    SP c {42, key, false, 1500, true, 300, true};
    SP s {42, key, true, 1500, true, 300, true};
    Wire init, response, confirm, ack;
    void handshake(SP::Time now = start) {
        init = c.begin(now, wall); response = recv(s, init, now).reply;
        require(init.size() == 110 && response.size() == 134, "PFS wire sizes");
        require(load_be16(init.data()+74) == 2 && load_be16(init.data()+76) == 32, "INIT suite/DH");
        require(load_be16(response.data()+98) == 2 && load_be16(response.data()+100) == 32, "RESPONSE suite/DH");
        confirm = recv(c, response, now).reply;
        require(confirm.size() == 33 && confirm[0] == 0x8a, "encrypted CONFIRM");
        require(!recv(s, data(c), now).data, "pre-confirm DATA");
        ack = recv(s, confirm, now).reply;
        require(ack.size() == 33 && ack[0] == 0x8b, "encrypted ACK");
        require(recv(c, ack, now).activated, "client activation");
    }
};
Wire retag(Wire w, bool from_server) {
    // Test attacker knows PSK: authenticate deliberately malformed exchanges.
    const auto h = ascon::derive_direction_key(key, 42, !from_server);
    Wire input(w.begin(), w.begin() + 18);
    input.insert(input.end(), w.begin() + 34, w.end());
    ascon::tag_type tag {};
    ascon::mac(h, 42, input.data(), input.size(), tag);
    std::copy(tag.begin(), tag.end(), w.begin() + 18);
    return w;
}
int main() {
    log_level = LogLevel::quiet;
    Pair p; p.handshake();
    auto wire = data(p.c);
    auto r = recv(p.s, wire);
    require(r.data && r.update_peer, "PFS DATA");
    require(!recv(p.s, wire).data && !recv(p.c, wire).data, "replay/reflection");
    for (std::size_t i = 0; i < wire.size(); ++i) {
        auto bad = wire; bad[i] ^= 1;
        require(!recv(p.s, bad).data, "DATA tampering");
    }
    require(recv(p.s, p.confirm).reply == p.ack, "duplicate confirm");
    require(!recv(p.c, p.ack).activated, "duplicate ACK reset");
    // A recorded transcript and PSK no longer suffice for the old derivation.
    auto transcript = p.init; transcript.insert(transcript.end(), p.response.begin(), p.response.end());
    const std::string label("V4-SESSION-C2S\0", 15);
    Wire legacy_input(label.begin(), label.end()); legacy_input.insert(legacy_input.end(), transcript.begin(), transcript.end());
    ascon::key_type old_key {}; ascon::mac(key, 42, legacy_input.data(), legacy_input.size(), old_key);
    ProtocolV5 legacy(42, old_key, old_key, true); Packet packet;
    require(!legacy.decode(wire.data(), wire.size(), packet), "legacy derivation decrypts PFS");
    // Invalid authenticated DH or lengths must not occupy pending state.
    for (int variant = 0; variant < 6; ++variant) {
        Pair q;
        auto init = q.c.begin(start, wall), bad = init;
        if (variant == 0) std::fill(bad.begin()+78, bad.end(), 0);
        if (variant == 1) { std::fill(bad.begin()+78, bad.end(), 0); bad[78] = 1; }
        if (variant == 2) bad[77] = 31;
        if (variant == 3) bad.pop_back();
        if (variant == 4) bad.push_back(0);
        if (variant == 5) bad[75] = 3;
        require(recv(q.s, retag(bad, false)).reply.empty() && !q.s.ready(), "invalid DH/length accepted");
        // Use a fresh nonce: even rejected low-order input consumes its nonce.
        auto fresh = q.c.begin(start, wall);
        auto response = recv(q.s, fresh).reply;
        require(response.size() == 134, "invalid share occupied pending");
        auto bad_response = response;
        for (std::size_t i = 102; i < 134; ++i) bad_response.at(i) = 0;
        require(recv(q.c, retag(bad_response, true)).reply.empty() && !q.c.ready(), "zero server DH");
        require(!recv(q.c, response).reply.empty(), "good response after bad share");
    }
    for (int suite = 0; suite < 2; ++suite) {
        SP old_client(42, key, false, 1500, suite == 1), old_server(42, key, true, 1500, suite == 1);
        Pair q;
        require(recv(q.s, old_client.begin(start, wall)).reply.empty(), "old suite fallback");
        require(recv(old_server, q.c.begin(start, wall)).reply.empty(), "PFS downgraded");
    }
    // Every wire byte, including both public shares, is authenticated.
    {
        Pair q;
        auto init = q.c.begin(start, wall);
        for (std::size_t i = 0; i < init.size(); ++i) {
            auto bad = init; bad[i] ^= 1;
            require(recv(q.s, bad).reply.empty(), "INIT tampering");
        }
        auto response = recv(q.s, init).reply;
        for (std::size_t i = 0; i < response.size(); ++i) {
            auto bad = response; bad[i] ^= 1;
            require(recv(q.c, bad).reply.empty(), "RESPONSE tampering");
        }
        require(!recv(q.c, response).reply.empty(), "valid after tampering");
    }
    // Lost RESPONSE: retain the client's DH share and the server's candidate.
    {
        Pair q;
        auto init = q.c.begin(start, wall);
        auto lost = recv(q.s, init).reply;
        auto retry = q.c.tick(start + std::chrono::seconds(1), wall);
        require(retry == init, "retry changed DH/transcript");
        const auto retried = start + std::chrono::seconds(1);
        auto response = recv(q.s, retry, retried).reply;
        require(response == lost, "retry changed server RESPONSE/DH");
        auto confirm = recv(q.c, response, retried).reply;
        auto ack = recv(q.s, confirm, retried).reply;
        require(!ack.empty(), "lost response recovery");
        const auto ack_retry = retried + std::chrono::seconds(1);
        require(q.c.tick(ack_retry, wall) == confirm, "CONFIRM retry bytes");
        require(recv(q.s, confirm, ack_retry).reply == ack, "ACK retry bytes");
        require(recv(q.c, ack, ack_retry).activated, "ACK recovery");
    }
    // Rekey despite continuous traffic, preserving the old receive window.
    const auto rekey = start + SP::rekey_interval;
    require(recv(p.c, data(p.s), rekey - std::chrono::seconds(1)).data, "keep client live");
    require(p.c.tick(rekey - std::chrono::seconds(1), wall).empty(), "early rekey");
    auto delayed1 = data(p.c), delayed2 = data(p.c);
    const auto init = p.c.tick(rekey, wall);
    require(!init.empty() && init != p.init, "periodic DH rekey");
    require(recv(p.s, data(p.c), rekey).data, "old TX during rekey");
    auto response = recv(p.s, init, rekey).reply;
    auto confirm = recv(p.c, response, rekey).reply;
    auto ack = recv(p.s, confirm, rekey).reply;
    require(recv(p.c, ack, rekey).activated, "rekey activation");
    require(recv(p.s, delayed1, rekey).data, "old receive overlap");
    require(!recv(p.s, delayed2, rekey + SP::old_lifetime).data, "old session expiry");
    require(recv(p.s, data(p.c), rekey + SP::old_lifetime).data, "new session data");
    // Restart accepts a new exchange, never the captured confirmation.
    SP restarted(42, key, true, 1500, true, 300, true);
    require(!recv(restarted, p.init).reply.empty(), "restart INIT");
    require(!recv(restarted, p.confirm).activated && !restarted.ready(), "restart stale confirm");
    auto current = recv(p.s, data(p.c), rekey + SP::old_lifetime).session;
    require(current != nullptr, "counter test session");
    current->next_counter = 0xffffffffULL;
    require(!data(p.s).empty() && data(p.s).empty(), "PFS counter cap");
    for (const char* legacy_flag : {"--allow-v1", "--allow-v2"}) {
        Options options; const char* args[] = {"tuntom", legacy_flag};
        bool rejected = false;
        try { parse_options(2, const_cast<char**>(args), 1, options); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "PFS legacy CLI");
    }
    Options options;
    require(options.pfs && options.encrypt_ascon, "PFS+AEAD is not default");
    const char* auth_args[] = {"tuntom", "--crypto-auth-only"};
    parse_options(2, const_cast<char**>(auth_args), 1, options);
    require(!options.pfs && !options.encrypt_ascon, "auth-only CLI mode");
    for (const char* removed : {"--pfs", "--encrypt-ascon"}) {
        Options removed_options; const char* args[] = {"tuntom", removed};
        bool rejected = false;
        try { parse_options(2, const_cast<char**>(args), 1, removed_options); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "removed crypto CLI option accepted");
    }
    std::cout << "PASS: PFS handshake, DH validation, suite isolation, retransmission, periodic rekey, expiry, counter cap and CLI\n";
}
