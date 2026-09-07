// Delayed, lossy handshake flights through the real codec and retry timers.
#include "../src/session.hpp"
#include <iostream>
#include <map>

using namespace tuntom;
using SP = SessionProtocol;
using Wire = std::vector<std::uint8_t>;
using Ms = std::chrono::milliseconds;
const auto start = SP::Time{} + std::chrono::seconds(100);
constexpr std::int64_t epoch = 100000;
const ascon::key_type key {};

void require(bool ok, const char* why) {
    if (not ok) throw std::runtime_error(why);
}

SP::Received receive(SP& receiver, const Wire& wire, SP::Time now) {
    Packet packet;
    Wire scratch;
    const auto wall = epoch + std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
    return receiver.receive(wire.data(), wire.size(), packet, scratch, now, wall);
}

struct Link {
    struct Flight { bool to_server; Wire wire; };
    SP client, server;
    SP::Time now = start;
    Ms delay;
    PacketType lose;
    bool lost = false;
    std::multimap<SP::Time, Flight> network;

    Link(int suite, int rtt_ms, PacketType drop = PacketType::hello)
        : client(42, key, false, 1500, suite != 0, 300, suite == 2),
          server(42, key, true, 1500, suite != 0, 300, suite == 2),
          delay(rtt_ms / 2), lose(drop) {}

    void send(bool to_server, Wire wire) {
        if (wire.empty()) return;
        if (not lost and static_cast<PacketType>(wire[0] & 0x0f) == lose) {
            lost = true;
            return;
        }
        network.emplace(now + delay, Flight {to_server, std::move(wire)});
    }

    void complete(std::uint64_t expected) {
        const auto deadline = now + std::chrono::seconds(12);
        for (; now < deadline; now += Ms(50)) {
            while (not network.empty() and network.begin()->first <= now) {
                auto flight = std::move(network.begin()->second);
                network.erase(network.begin());
                auto result = receive(flight.to_server ? server : client, flight.wire, now);
                send(not flight.to_server, std::move(result.reply));
            }
            const auto wall = epoch + std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
            send(true, client.tick(now, wall));
            server.tick(now, wall);
            if (client.counters().handshake_completed == expected and
                server.counters().handshake_completed == expected) return;
        }
        throw std::runtime_error("handshake did not complete over delayed/lossy link");
    }

    void data_round_trip() {
        Packet packet;
        packet.tunnel_id = 42;
        packet.original_length = 4;
        packet.payload = {0x45, 1, 2, 3};
        for (bool to_server : {true, false}) {
            auto& sender = to_server ? client : server;
            auto& receiver = to_server ? server : client;
            auto wire = sender.encode(packet);
            Packet decoded;
            Wire scratch;
            auto result = receiver.receive(wire.data(), wire.size(), decoded, scratch, now, epoch);
            require(result.data and decoded.payload == packet.payload, "DATA after delayed handshake");
            require(receive(receiver, wire, now).replay_drop, "DATA replay after delayed handshake");
        }
    }
};

void test_latency_and_loss(int suite) {
    for (int rtt : {400, 1200, 4800}) {
        Link link(suite, rtt);
        link.send(true, link.client.begin(link.now, epoch));
        link.complete(1);
        require(link.client.counters().handshake_started == 1 and
                link.server.counters().handshake_started == 1, "latency restarted live exchange");
        link.data_round_trip();
    }
    for (auto drop : {PacketType::init, PacketType::response,
                      PacketType::confirm, PacketType::confirm_ack}) {
        Link link(suite, 1200, drop);
        link.send(true, link.client.begin(link.now, epoch));
        link.complete(1);
        require(link.lost, "flight loss was not exercised");
        require(link.client.counters().handshake_started == 1 and
                link.server.counters().handshake_started == 1, "single lost flight restarted exchange");
        link.data_round_trip();
    }
}

void test_retry_deadline_and_replay(int suite) {
    SP client(42, key, false, 1500, suite != 0, 300, suite == 2);
    SP server(42, key, true, 1500, suite != 0, 300, suite == 2);
    const auto init = client.begin(start, epoch);
    const auto response = receive(server, init, start).reply;
    require(not response.empty(), "initial RESPONSE");
    for (int seconds = 1; seconds < 5; ++seconds) {
        const auto now = start + std::chrono::seconds(seconds);
        const auto retry = client.tick(now, epoch + seconds);
        require(retry == init, "INIT retry changed transcript or PFS share");
        auto duplicate = receive(server, retry, now);
        require(duplicate.reply == response and not duplicate.activated and
                not duplicate.update_peer and not server.ready(), "retry changed server state");
    }
    // A valid MAC with the same nonce but changed exchange, time or DH is not
    // an exact retransmission. None may obtain the pending cached response.
    ProtocolV5 decode(42, key, true), encode(42, key, false);
    Packet original;
    require(decode.decode(init.data(), init.size(), original), "decode INIT for replay variants");
    for (int variant = 0; variant < (suite == 2 ? 3 : 2); ++variant) {
        auto altered = original;
        if (variant == 0) altered.message_id = original.message_id == 1 ? 2 : 1;
        if (variant == 1) store_be64(altered.payload.data() + 32, epoch + 1);
        if (variant == 2) altered.payload.back() ^= 1;
        auto rejected = receive(server, encode.encode(altered), start + Ms(4500));
        require(rejected.replay_drop and rejected.reply.empty(), "altered nonce reuse answered");
    }
    SP competing(42, key, false, 1500, suite != 0, 300, suite == 2);
    const auto other_init = competing.begin(start + Ms(4500), epoch + 4);
    require(receive(server, other_init, start + Ms(4500)).reply.empty(),
            "different INIT displaced pending exchange");
    require(receive(server, init, start + Ms(4999)).reply == response and
            server.counters().handshake_started == 1, "pending exchange changed before deadline");
    const auto deadline = start + SP::pending_lifetime;
    auto expired = receive(server, init, deadline);
    require(expired.replay_drop and expired.reply.empty(), "retry extended or reopened pending deadline");
    require(receive(server, other_init, deadline).replay_drop,
            "INIT ignored while pending reopened an exchange");
    require(receive(client, response, deadline).reply.empty(), "expired client flight accepted RESPONSE");
    const auto fresh = client.tick(deadline, epoch + 5);
    require(fresh != init and not fresh.empty(), "deadline did not create fresh exchange");
    if (suite == 2)
        require(not std::equal(init.begin() + 78, init.end(), fresh.begin() + 78), "deadline reused PFS share");
    require(receive(client, response, deadline).reply.empty(), "old RESPONSE accepted by new attempt");
    const auto fresh_response = receive(server, fresh, deadline).reply;
    const auto confirm = receive(client, fresh_response, deadline).reply;
    auto activation = receive(server, confirm, deadline);
    require(activation.activated and receive(client, activation.reply, deadline).activated,
            "fresh handshake after deadline failed");
    require(receive(server, init, deadline).replay_drop, "expired INIT survived new handshake");
    auto completed = receive(server, fresh, deadline);
    require(completed.replay_drop and completed.reply.empty() and not completed.update_peer,
            "completed INIT replay reopened exchange");
    require(client.counters().handshake_timeouts == 1 and
            server.counters().handshake_timeouts == 1, "retry deadline was refreshed");
}

void test_response_resend_rate(int suite) {
    SP client(42, key, false, 1500, suite != 0, 300, suite == 2);
    SP server(42, key, true, 1500, suite != 0, 300, suite == 2);
    const auto init = client.begin(start, epoch);
    const auto response = receive(server, init, start).reply;
    require(not response.empty(), "initial RESPONSE was rate limited");
    require(receive(server, init, start).reply == response, "first resend was delayed");
    for (int ms = 0; ms < 600; ++ms) {
        auto retry = receive(server, init, start + Ms(ms));
        const bool allowed = ms != 0 and ms % 200 == 0;
        require(allowed ? retry.reply == response : retry.reply.empty(), "200ms resend boundary under flood");
        require(retry.control and not retry.activated and not retry.update_peer and
                not retry.replay_drop and not server.ready(), "resend limiter changed handshake state");
    }
    require(server.counters().handshake_started == 1 and
            server.counters().handshake_retries == 3, "suppressed INITs counted as RESPONSE retries");
    require(receive(server, init, start + Ms(4999)).reply == response, "resend before pending deadline");
    const auto deadline = start + SP::pending_lifetime;
    auto expired = receive(server, init, deadline);
    require(expired.replay_drop and expired.reply.empty(), "rate-limited flood extended pending lifetime");

    // A fresh exchange gets its own immediate resend, even inside the previous
    // exchange's last 200ms window. It must still complete and transfer DATA.
    const auto fresh = client.tick(deadline, epoch + 5);
    const auto fresh_response = receive(server, fresh, deadline).reply;
    require(not fresh_response.empty() and
            receive(server, fresh, deadline).reply == fresh_response, "new exchange inherited resend limit");
    const auto confirm = receive(client, fresh_response, deadline).reply;
    auto activation = receive(server, confirm, deadline);
    require(activation.activated and receive(client, activation.reply, deadline).activated,
            "rate-limited exchange failed to complete");
    Packet packet;
    packet.type = PacketType::keepalive;
    packet.tunnel_id = 42;
    const auto wire = client.encode(packet);
    require(receive(server, wire, deadline).data, "session traffic failed after rate limiting");
    require(receive(server, wire, deadline).replay_drop, "session replay protection reset by rate limiting");
}

void test_delayed_pfs_rekey() {
    Link link(2, 1200);
    link.send(true, link.client.begin(link.now, epoch));
    link.complete(1);
    link.data_round_trip();
    link.network.clear(); // Discard duplicate flights left over from the first handshake.
    link.now += SP::rekey_interval;
    link.data_round_trip(); // Authenticated traffic prevents the idle-restart trigger.
    link.lose = PacketType::response;
    link.send(true, link.client.tick(link.now, epoch + 123));
    link.complete(2);
    require(link.lost and link.client.counters().rekey_started == 1 and
            link.client.counters().rekey_completed == 1 and
            link.server.counters().rekey_completed == 1, "delayed PFS rekey with lost RESPONSE");
    link.data_round_trip();
}

int main() {
    for (int suite = 0; suite <= 2; ++suite) {
        test_latency_and_loss(suite);
        test_retry_deadline_and_replay(suite);
        test_response_resend_rate(suite);
    }
    test_delayed_pfs_rekey();
    std::cout << "PASS: delayed/lossy handshakes, bounded exact retries, RESPONSE rate limit, replay rejection and PFS rekey\n";
}
