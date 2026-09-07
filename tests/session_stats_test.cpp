#include "../src/session.hpp"
#include <sstream>
#include <iostream>
#include <map>
using namespace tuntom;
using SP = SessionProtocol;
using Wire = std::vector<std::uint8_t>;
const auto start = SP::Time{} + std::chrono::seconds(100);
ascon::key_type key {};
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
SP::Received recv(SP& s, const Wire& wire, SP::Time now = start) {
    Wire scratch; Packet p;
    return s.receive(wire.data(), wire.size(), p, scratch, now, 100000);
}
auto snapshot(const SP& s, SP::Time now = start) {
    std::ostringstream output; s.write_stats(output, now);
    std::map<std::string, std::string> fields;
    std::istringstream input(output.str()); std::string line;
    while (std::getline(input, line)) {
        auto n = line.find('=');
        check(n != std::string::npos, "stats format");
        check(fields.emplace(line.substr(0,n), line.substr(n+1)).second, "duplicate stats field");
    }
    return fields;
}
void round_trip(int suite) {
    SP c(42, key, false, 1500, suite != 0, 300, suite == 2);
    SP s(42, key, true, 1500, suite != 0, 300, suite == 2);
    auto f = snapshot(c);
    check(f["suite"] == std::to_string(suite) && f["suite_active"] == "-1", "configured vs active suite");
    check(f["session_ready"] == "0" && f["session_age_seconds"] == "-1", "no session gauges");
    check(f["handshake_last_age_seconds"] == "-1" && f["handshake_state"] == "idle", "no handshake gauges");
    check(f["pfs"] == (suite == 2 ? "1" : "0"), "PFS gauge");
    check(f["rekey_interval_seconds"] == (suite == 2 ? "120" : "0"), "interval gauge");
    for (int exchange = 0; exchange < 2; ++exchange) {
        auto now = start + std::chrono::seconds(exchange * 120);
        auto init = c.begin(now, 100000);
        check(snapshot(c, now)["handshake_state"] == "wait_response", "INIT state");
        auto response = recv(s, init, now).reply;
        check(snapshot(s, now)["handshake_state"] == "wait_confirm", "pending state");
        check(c.counters().handshake_started == std::uint64_t(exchange+1) &&
              s.counters().handshake_started == std::uint64_t(exchange+1), "attempts");
        check(c.counters().rekey_started == std::uint64_t(exchange) &&
              s.counters().rekey_started == std::uint64_t(exchange), "rekey attempts");
        auto confirm = recv(c, response, now).reply;
        f = snapshot(c, now);
        check(f["handshake_state"] == "wait_ack" && f["session_ready"] == "1" &&
              f["session_confirmed"] == "0", "candidate gauges");
        check(recv(c, response, now).reply == confirm, "duplicate RESPONSE");
        auto ack = recv(s, confirm, now).reply;
        check(s.counters().handshake_completed == std::uint64_t(exchange+1) &&
              c.counters().handshake_completed == std::uint64_t(exchange), "completion boundaries");
        check(recv(s, confirm, now).reply == ack, "duplicate CONFIRM");
        check(recv(c, ack, now).activated, "ACK");
        check(!recv(c, ack, now).activated, "duplicate ACK");
        check(c.counters().rekey_completed == std::uint64_t(exchange) &&
              s.counters().rekey_completed == std::uint64_t(exchange), "completion not doubled");
        check(c.counters().handshake_retries == std::uint64_t(exchange+1) &&
              s.counters().handshake_retries == std::uint64_t(exchange+1), "duplicate reply retries");
        f = snapshot(c, now + std::chrono::seconds(3));
        check(f["session_age_seconds"] == "3" && f["handshake_last_age_seconds"] == "3", "ages");
        check(f["suite_active"] == std::to_string(suite) && f["session_confirmed"] == "1", "active gauges");
        Packet p; p.type = PacketType::keepalive; p.tunnel_id = 42;
        c.encode(p);
        check(snapshot(c, now)["session_tx_counter"] == "1", "packet counter resets on rekey");
    }
}
void reassembly_across_rekey() {
    SP c(42,key,false), s(42,key,true);
    auto handshake = [&](SP::Time now) {
        auto response = recv(s, c.begin(now, 100000), now).reply;
        auto confirm = recv(c, response, now).reply;
        auto ack = recv(s, confirm, now).reply;
        check(recv(c, ack, now).activated, "reassembly stats handshake");
    };
    auto partial = [&](std::uint64_t id, SP::Time now) {
        Packet packet; packet.message_id = id; packet.original_length = 8;
        packet.payload = {1, 2};
        const auto wire = c.encode(packet);
        Wire scratch, complete;
        auto r = s.receive(wire.data(), wire.size(), packet, scratch, now, 100000);
        check(r.data && !r.session->reassembly.accept(packet, complete, nullptr, now),
              "partial for stats");
    };
    handshake(start);
    partial(1, start);
    auto rekey = start + std::chrono::seconds(1);
    handshake(rekey);
    partial(1, rekey); // same ID in two different sessions
    check(snapshot(s, rekey)["reassembly_active_entries"] == "2" &&
          snapshot(s, rekey)["reassembly_active_bytes"] == "16", "aggregate session gauges");
    // Retirement is separately accounted for; it must not look like expiry or
    // lose cumulative counters. tick destroys the previous-session owner.
    auto retired = rekey + SP::old_lifetime;
    s.tick(retired, 100000);
    auto f = snapshot(s, retired);
    check(f["reassembly_active_entries"] == "1" &&
          f["reassembly_session_discarded_entries"] == "1" &&
          f["reassembly_peak_entries"] == "2", "retirement lost metrics");
    s.cleanup(retired);
    f = snapshot(s, retired);
    check(f["reassembly_active_entries"] == "0" &&
          f["reassembly_active_bytes"] == "0" &&
          f["reassembly_expired_entries"] == "1", "periodic cleanup metrics");
    handshake(retired);
    check(snapshot(s, retired)["reassembly_expired_entries"] == "1",
          "rekey reset loss counter");
}
int main() {
    log_level = LogLevel::quiet;
    for (int suite = 0; suite <= 2; ++suite) round_trip(suite);
    reassembly_across_rekey();
    {
        SP c(42,key,false), s(42,key,true);
        auto init = c.begin(start,100000);
        auto response = recv(s,init).reply;
        auto retry = c.tick(start + std::chrono::seconds(1),100000);
        check(c.counters().handshake_started == 1 && c.counters().handshake_retries == 1, "INIT retry is not new attempt");
        check(recv(s,retry,start + std::chrono::seconds(1)).reply == response &&
              s.counters().handshake_started == 1 && s.counters().handshake_retries == 1,
              "cached RESPONSE retry is not new attempt");
        s.tick(start + SP::pending_lifetime,100000);
        s.tick(start + SP::pending_lifetime,100000);
        check(s.counters().handshake_timeouts == 1, "pending timeout counted once");
        check(recv(c,response,start + SP::pending_lifetime).reply.empty(), "expired response");
        c.tick(start + std::chrono::seconds(6),100000);
        check(c.counters().handshake_timeouts == 1, "client flight timeout");
    }
    {
        SP c(42,key,false), s(42,key,true);
        auto response = recv(s,c.begin(start,100000)).reply;
        auto confirm = recv(c,response).reply;
        check(c.tick(start + std::chrono::seconds(1),100000) == confirm, "timer CONFIRM retry");
        check(c.counters().handshake_retries == 1 && c.counters().handshake_started == 1, "CONFIRM retry is not new attempt");
        c.tick(start + SP::pending_lifetime,100000);
        check(c.counters().handshake_timeouts == 1 && c.counters().handshake_completed == 0, "unconfirmed timeout");
    }
    {
        SP c(42,key,false), s(42,key,true,1500,true,300,true);
        auto init = c.begin(start,100000);
        auto bad = init; bad.back() ^= 1;
        recv(s,bad);
        check(s.counters().handshake_suite_mismatch == 0, "unauthenticated mismatch");
        recv(s,init);
        check(s.counters().handshake_suite_mismatch == 1 && s.counters().handshake_started == 0, "authenticated suite mismatch");
    }
    {
        SP c(42,key,false,1500,true,300,true), s(42,key,true,1500,true,300,true);
        auto init = c.begin(start,100000);
        ProtocolV5 decode(42,key,true), encode(42,key,false); Packet p;
        check(decode.decode(init.data(),init.size(),p), "decode INIT");
        std::fill(p.payload.begin()+44,p.payload.end(),0);
        recv(s,encode.encode(p));
        check(s.counters().handshake_dh_rejected == 1 && s.counters().handshake_started == 0, "zero DH rejected");
        auto response = recv(s,c.begin(start,100000)).reply;
        // Expiration through receive must count once, just like tick.
        recv(s,{},start + SP::pending_lifetime);
        s.tick(start + SP::pending_lifetime,100000);
        check(s.counters().handshake_timeouts == 1, "receive expiration counted once");
        check(!response.empty(), "accepted fresh INIT");
    }
    std::cout << "PASS: session stats, suites, completion boundaries, retries, timeouts, rekeys and authenticated rejection counters\n";
}
