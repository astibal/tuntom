#include "../src/remote_control.hpp"
#include "../src/protocol.hpp"
#include "../src/session.hpp"
#include <deque>
#include <iostream>
using namespace tuntom;
namespace rc = tuntom::remote_control;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    ControlDispatcher d;
    int calls = 0;
    d.stats = [] { return std::string(4500, 's'); };
    d.classifier = [&](const std::string&, const std::string& body) { ++calls; require(body == std::string(9000, 'c'), "body preserved"); return std::string("loaded\n"); };
    require(d.execute({"classifier load 0", ""}, {}).rejected, "default deny");
    require(d.execute({"show stats", ""}, {1}).success, "read allowed");
    require(d.execute({"classifier load 0", ""}, {1}).rejected, "read cannot write");
    require(d.execute({"show stats", ""}, {4}).rejected, "write cannot read");
    require(d.execute({"remote 5 2000 show stats", ""}, ControlAccess::all()).rejected, "no remote recursion");
    std::deque<std::vector<std::uint8_t>> ab, ba;
    RemoteControl a, b;
    auto now = RemoteControl::Time{};
    a.configure(ControlAccess::all(), [&](auto bytes) { ab.push_back(bytes); }, [&](auto request) { return d.execute(request, {}); });
    b.configure(ControlAccess::all(), [&](auto bytes) { ba.push_back(bytes); }, [&](auto request) { return d.execute(request, ControlAccess::all()); });
    a.payload_limit(800); b.payload_limit(800);
    const auto load = a.submit({"classifier load 9000", std::string(9000, 'c')}, 5, std::chrono::milliseconds(20), now);
    // Deliver just the first upload block, then issue a large stats response.
    auto first = ab.front(); ab.pop_front(); b.receive(first, now);
    const auto stats = a.submit({"show stats", ""}, 5, std::chrono::milliseconds(20), now);
    const auto busy = a.submit({"classifier disable 0", ""}, 5, std::chrono::milliseconds(20), now);
    bool dropped_result = false, dropped_confirm = false, stats_during_upload = false;
    for (int step = 0; step < 5000; ++step) {
        now += std::chrono::milliseconds(1);
        if (!ab.empty()) { auto bytes = ab.front(); ab.pop_front(); b.receive(bytes, now); }
        if (!ba.empty()) {
            auto bytes = ba.front(); ba.pop_front(); rc::Frame f; require(rc::decode(bytes, f), "valid frame");
            if (f.id == stats && f.state == rc::State::succeeded && calls == 0) stats_during_upload = true;
            if (f.id == load && f.state == rc::State::succeeded && f.kind == rc::Kind::reply && !dropped_result) dropped_result = true;
            else if (f.id == load && f.kind == rc::Kind::confirmed && !dropped_confirm) dropped_confirm = true;
            else a.receive(bytes, now);
        }
        a.tick(now); b.tick(now);
        if (a.result(load) && a.result(stats) && a.result(busy)) break;
    }
    require(a.result(load) && a.result(load)->exit == 0, "load succeeds after lost result/confirmation");
    require(a.result(stats) && a.result(stats)->exit == 0 && a.result(stats)->body.size() == 4500, "multi-block stats");
    require(a.result(busy) && a.result(busy)->exit == 255, "concurrent change rejected");
    require(calls == 1 && dropped_result && dropped_confirm && stats_during_upload, "exactly once, real concurrency and loss");
    b.receive(first, now); require(calls == 1, "duplicate first frame does not execute");
    a.release(load); a.release(stats); a.release(busy); ab.clear(); ba.clear();
    // A missing chunk expires without executing; STATUS polling cannot extend it.
    auto expire = a.submit({"classifier load 9000", std::string(9000, 'c')}, 5, std::chrono::milliseconds(20), now);
    b.receive(ab.front(), now); ab.clear(); ba.clear();
    rc::Frame query; query.kind = rc::Kind::status; query.id = expire;
    for (int i = 0; i < 21; ++i) { now += std::chrono::milliseconds(100); b.receive(rc::encode(query), now); b.tick(now); ba.clear(); }
    b.receive(rc::encode(query), now); rc::Frame expired; rc::decode(ba.back(), expired);
    require(expired.state == rc::State::expired && calls == 1, "idle expiration despite polls");
    // CONTROL has authenticated wire metadata, no DATA fragmentation, and rejects short payloads.
    ascon::key_type key{}; ProtocolV5 tx(1, key, false), rx(1, key, true);
    Packet p; p.type = PacketType::control; p.sequence = 1; p.payload = rc::encode(query);
    auto wire = tx.encode(p); Packet decoded;
    require(rx.decode(wire.data(), wire.size(), decoded) && decoded.type == PacketType::control, "CONTROL roundtrip");
    wire.back() ^= 1; require(!rx.decode(wire.data(), wire.size(), decoded), "CONTROL authentication");
    // Query the receiver from a fresh sender, with no local cached result.
    ab.clear(); ba.clear();
    a.release(load);
    a.query(load, 5, std::chrono::milliseconds(20), now);
    for (int n = 0; n < 100 && !a.result(load); ++n) {
        if (!ab.empty()) { auto bytes = ab.front(); ab.pop_front(); b.receive(bytes, now); }
        if (!ba.empty()) { auto bytes = ba.front(); ba.pop_front(); a.receive(bytes, now); }
    }
    require(a.result(load) && a.result(load)->exit == 0 && calls == 1, "query restores receiver result without executing");
    // A full unconfirmed history rejects new commands; acknowledging one entry
    // makes room. Expensive response buffers are bounded independently of count.
    RemoteControl full; std::vector<std::uint8_t> answer;
    unsigned executions = 0;
    full.configure(ControlAccess::all(), [&](auto bytes) { answer = bytes; }, [&](auto) { ++executions; return ControlResponse{true, "ok"}; });
    rc::Frame put; put.kind = rc::Kind::put; put.command = "show stats";
    for (unsigned i = 1; i <= 129; ++i) {
        put.id[0] = static_cast<std::uint8_t>(i); full.receive(rc::encode(put), now);
        rc::Frame response; require(rc::decode(answer, response), "history response");
        require(response.state == (i <= 128 ? rc::State::succeeded : rc::State::rejected), "bounded unconfirmed history");
    }
    require(executions == 128, "capacity rejection does not execute");
    rc::Frame ack; ack.id[0] = 1; ack.kind = rc::Kind::finish; ack.state = rc::State::succeeded; ack.total = 2;
    full.receive(rc::encode(ack), now); full.receive(rc::encode(put), now);
    require(executions == 129, "confirmed oldest entry evicted");
    rc::Frame malformed = put; malformed.total = 0; malformed.data = "unexpected";
    malformed.id[0] = 130; full.receive(rc::encode(malformed), now);
    require(executions == 129, "bad length never executes");
    // Session replacement interrupts the sender and expires an unfinished upload.
    a.session_changed(now); b.session_changed(now);
    // Authenticate and replay-check CONTROL exactly like tunnel DATA, but reject
    // packets from a previous key epoch even during the DATA rekey grace period.
    for (bool encrypted : {false, true}) {
        SessionProtocol client(42, key, false, 1500, encrypted, 300, encrypted);
        SessionProtocol server(42, key, true, 1500, encrypted, 300, encrypted);
        Packet packet; std::vector<std::uint8_t> scratch;
        constexpr std::int64_t wall = 100000;
        auto receive = [&](SessionProtocol& target, const std::vector<std::uint8_t>& bytes) {
            return target.receive(bytes.data(), bytes.size(), packet, scratch, now, wall);
        };
        auto establish = [&] {
            auto response = receive(server, client.begin(now, wall)).reply;
            auto confirm = receive(client, response).reply;
            auto accepted = receive(server, confirm);
            require(accepted.activated, "server handshake");
            require(receive(client, accepted.reply).activated, "client handshake");
        };
        establish();
        Packet control; control.type = PacketType::control; control.payload = rc::encode(query);
        auto valid = client.encode(control), old = client.encode(control);
        require(receive(server, valid).data, "session accepts CONTROL");
        require(receive(server, valid).replay_drop, "session rejects replay");
        now += std::chrono::seconds(1); establish();
        require(!receive(server, old).data, "old-session CONTROL rejected");
    }
    std::cout << "PASS remote control permissions, multiplexing, loss recovery, duplicate suppression and expiration\n";
}
