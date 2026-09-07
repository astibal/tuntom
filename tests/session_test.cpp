// Socket-free state-machine tests with an explicit monotonic clock.
#include "../src/session.hpp"

using namespace tuntom;

void require(bool ok, const char* message) {
    if (not ok) throw std::runtime_error(message);
}
using Wire = std::vector<std::uint8_t>;
using Time = SessionProtocol::Time;
const Time start = Time{} + std::chrono::seconds(100);
ascon::key_type key {};

SessionProtocol::Received receive(SessionProtocol& p, const Wire& wire,
                                  Time now = start) {
    Packet packet;
    Wire scratch;
    return p.receive(wire.data(), wire.size(), packet, scratch, now);
}
Wire data(SessionProtocol& sender, std::uint32_t offset = 0) {
    Packet packet;
    packet.type = PacketType::data;
    packet.tunnel_id = 42;
    packet.message_id = 99;
    packet.original_length = 40;
    packet.fragment_offset = offset;
    packet.payload.resize(20, offset == 0 ? 0x45 : 0x66);
    return sender.encode(packet);
}
struct Flight { Wire init, response, confirm, ack; };
Flight handshake(SessionProtocol& client, SessionProtocol& server, Time now = start) {
    Flight f;
    f.init = client.begin(now);
    auto r = receive(server, f.init, now);
    require(not r.update_peer and not r.activated, "INIT changed peer/session");
    f.response = r.reply;
    require(f.init.size() == 78 and f.response.size() == 102, "handshake layout");
    f.confirm = receive(client, f.response, now).reply;
    require(f.confirm.size() == 33, "confirmation layout");
    r = receive(server, f.confirm, now);
    require(r.activated and r.update_peer, "CONFIRM did not activate server");
    f.ack = r.reply;
    require(receive(client, f.ack, now).activated, "ACK did not confirm client");
    return f;
}

void test_handshake_reordering_retransmission() {
    SessionProtocol client(42, key, false), server(42, key, true);
    require(data(client).empty(), "DATA before handshake");
    auto init = client.begin(start);
    require(client.tick(start + std::chrono::milliseconds(999)).empty(), "early retry");
    auto retry_init = client.tick(start + std::chrono::seconds(1));
    require(retry_init == init, "INIT retry changed live exchange");
    init = retry_init;
    auto response = receive(server, init).reply;
    require(receive(server, init).reply == response, "INIT duplicate changed RESPONSE");
    auto confirm = receive(client, response).reply;
    require(receive(client, response).reply == confirm, "RESPONSE duplicate changed CONFIRM");
    auto early = data(client);
    require(not receive(server, early).data, "DATA accepted before CONFIRM");
    auto activation = receive(server, confirm);
    require(activation.activated, "activation failed");
    require(client.tick(start + std::chrono::seconds(2)) == confirm, "lost ACK retry failed");
    auto duplicate = receive(server, confirm);
    require(not duplicate.update_peer and not duplicate.activated, "duplicate CONFIRM reactivated");
    require(duplicate.reply == activation.reply, "duplicate CONFIRM did not resend ACK");
    require(receive(client, duplicate.reply).activated, "ACK rejected");
    require(not receive(client, duplicate.reply).activated, "duplicate ACK reactivated");
    auto first = data(client), second = data(client);
    require(receive(server, second).data, "higher counter rejected");
    require(receive(server, first).data, "reordering rejected");
    require(not receive(server, first).data, "duplicate DATA accepted");
    require(receive(server, early).data, "early DATA consumed replay state before CONFIRM");
    receive(server, confirm);
    require(not receive(server, first).data, "CONFIRM reset replay window");
    require(not receive(client, data(client)).data, "client reflection accepted");
    require(not receive(server, data(server)).data, "server reflection accepted");
    require(receive(client, data(server)).data, "server-to-client DATA failed");
    Packet ordinary;
    ordinary.tunnel_id = 42;
    ordinary.type = PacketType::keepalive;
    ProtocolV5 long_term(42, key, false);
    ordinary.sequence = 123;
    require(not receive(server, long_term.encode(ordinary)).data,
            "long-term key authenticated session traffic");
    for (auto type : {PacketType::hello, PacketType::keepalive, PacketType::ping,
                      PacketType::pong, PacketType::mtu_probe, PacketType::mtu_reply}) {
        ordinary.type = type;
        ordinary.message_id = (type == PacketType::hello or type == PacketType::keepalive) ? 0 : 456;
        ordinary.original_length = (type == PacketType::mtu_probe or type == PacketType::mtu_reply) ? 1400 : 0;
        ordinary.payload.clear();
        if (type == PacketType::mtu_probe) ordinary.payload.resize(1324);
        auto wire = client.encode(ordinary);
        require(receive(server, wire).data, "session control message rejected");
        require(not receive(client, wire).data, "session control reflection accepted");
        require(not receive(server, wire).data, "session control replay accepted");
    }
}

void test_restart_and_timeouts() {
    SessionProtocol client(42, key, false), server(42, key, true);
    const auto flight = handshake(client, server);
    const auto old = data(client);
    SessionProtocol restarted(42, key, true);
    require(not receive(restarted, old).data, "old DATA accepted after restart");
    auto fresh_response = receive(restarted, flight.init).reply;
    require(not fresh_response.empty() and fresh_response != flight.response, "server nonce reused");
    require(not receive(restarted, flight.confirm).activated, "old CONFIRM accepted after restart");
    require(not receive(client, fresh_response).activated, "unsolicited RESPONSE activated");
    const auto later = start + std::chrono::seconds(21);
    auto new_init = client.tick(later);
    require(not new_init.empty() and new_init != flight.init, "idle recovery didn't restart");
    require(receive(client, flight.response, later).reply.empty(), "old response bound to new INIT");
    auto response = receive(restarted, new_init, later).reply;
    auto confirm = receive(client, response, later).reply;
    auto ack = receive(restarted, confirm, later).reply;
    require(receive(client, ack, later).activated, "restart recovery failed");
    require(not receive(restarted, old, later).data, "old session packet survived rekey");
    require(receive(restarted, data(client), later).data, "new DATA rejected");

    SessionProtocol c(42, key, false), s(42, key, true);
    auto init = c.begin(start);
    auto r1 = receive(s, init).reply;
    receive(s, init, start + std::chrono::seconds(4));
    auto r2 = receive(s, init, start + std::chrono::seconds(5)).reply;
    require(r2.empty(), "expired INIT reopened pending state");
    auto stale_confirm = receive(c, r1).reply;
    require(not receive(s, stale_confirm, start + std::chrono::seconds(5)).activated,
            "expired pending key accepted");
    auto retry = c.tick(start + std::chrono::seconds(6));
    require(not retry.empty() and retry != stale_confirm, "CONFIRM timeout not restarted");

    SessionProtocol ack_client(42, key, false), ack_server(42, key, true);
    const auto ack_init = ack_client.begin(start);
    const auto ack_response = receive(ack_server, ack_init).reply;
    const auto lost_confirm = receive(ack_client, ack_response).reply;
    require(ack_client.tick(start + std::chrono::seconds(4)) == lost_confirm,
            "CONFIRM retry stopped before five seconds");
    const auto new_attempt = ack_client.tick(start + std::chrono::seconds(5));
    require(not new_attempt.empty() and new_attempt != lost_confirm and new_attempt != ack_init,
            "CONFIRM deadline did not restart at five seconds");
}

void test_sessions_reassembly_collision_and_overflow() {
    SessionProtocol c(42, key, false), s(42, key, true);
    handshake(c, s);
    auto old_first = data(c), old_last = data(c, 20);
    Packet decoded;
    Wire scratch, complete;
    auto old = s.receive(old_first.data(), old_first.size(), decoded, scratch, start);
    require(old.data, "old first fragment rejected");
    require(not old.session->reassembly.accept(decoded, complete), "partial completed");
    auto old_session = old.session;
    const auto rekey = start + std::chrono::seconds(1);
    handshake(c, s, rekey);
    auto new_first = data(c), new_last = data(c, 20);
    auto fresh = s.receive(new_last.data(), new_last.size(), decoded, scratch, rekey);
    require(fresh.data and fresh.session != old_session, "session wasn't replaced");
    require(not fresh.session->reassembly.accept(decoded, complete), "mixed fragments across sessions");
    auto new_session = fresh.session;
    auto old_tail = s.receive(old_last.data(), old_last.size(), decoded, scratch, rekey);
    require(old_tail.data and not old_tail.update_peer, "previous session peer handling");
    require(old_tail.session->reassembly.accept(decoded, complete), "old reassembly failed");
    auto new_head = s.receive(new_first.data(), new_first.size(), decoded, scratch, rekey);
    require(new_head.session->reassembly.accept(decoded, complete), "new reassembly failed");

    // Force equal public hints while keeping cryptographically distinct keys.
    // Same counter in each session must still have independent replay state.
    old_session->hint = new_session->hint;
    Packet collision;
    collision.tunnel_id = 42;
    collision.type = PacketType::keepalive;
    collision.sequence = (std::uint64_t(new_session->hint) << 48) | 77;
    // Build peer-direction tags using the handshake transcript derivation.
    auto make_peer_codec = [](const SessionProtocol::Session& session) {
        Wire transcript = session.init;
        transcript.insert(transcript.end(), session.response.begin(), session.response.end());
        auto derive = [&](const char* label) {
            Wire input(label, label + std::strlen(label) + 1);
            input.insert(input.end(), transcript.begin(), transcript.end());
            ascon::key_type out {};
            ascon::mac(key, 42, input.data(), input.size(), out);
            return out;
        };
        return ProtocolV5(42, derive("V4-SESSION-C2S"), derive("V4-SESSION-S2C"));
    };
    auto old_wire = make_peer_codec(*old_session).encode(collision);
    auto new_wire = make_peer_codec(*new_session).encode(collision);
    require(receive(s, old_wire, rekey).data, "hint collision failed old key");
    require(receive(s, new_wire, rekey).data, "hint collision failed new key");
    require(not receive(s, old_wire, rekey).data, "collision bypassed replay");
    collision.sequence++;
    old_wire = make_peer_codec(*old_session).encode(collision);
    require(not receive(s, old_wire, rekey + std::chrono::seconds(3)).data,
            "expired previous session accepted");
    new_session->next_counter = SessionProtocol::counter_mask;
    require(not data(s).empty(), "last counter rejected");
    require(data(s).empty() and not s.ready(), "counter wrapped");
}

void test_invalid_handshake() {
    SessionProtocol c(42, key, false), s(42, key, true);
    const auto init = c.begin(start);
    // Tamper every byte, including nonce, mode and length fields.
    for (std::size_t i = 0; i < init.size(); ++i) {
        auto bad = init;
        bad[i] ^= 1;
        require(receive(s, bad).reply.empty(), "tampered INIT accepted");
    }
    ProtocolV5 client_codec(42, key, false);
    Packet packet;
    require(client_codec.decode(init.data(), init.size(), packet) == false, "INIT reflected");
    ProtocolV5 server_codec(42, key, true);
    require(server_codec.decode(init.data(), init.size(), packet), "INIT decoding");
    packet.payload[40] = 1;
    require(receive(s, client_codec.encode(packet)).reply.empty(), "unsupported suite accepted");
    packet.payload[40] = 0;
    packet.payload[43] = 1;
    packet.payload.push_back(0);
    require(receive(s, client_codec.encode(packet)).reply.empty(), "nonempty DH accepted");
    packet.payload.resize(44);
    packet.payload[43] = 0;
    packet.sequence = 1;
    require(receive(s, client_codec.encode(packet)).reply.empty(), "nonzero INIT SEQ accepted");
    auto response = receive(s, init).reply;
    for (std::size_t i = 0; i < response.size(); ++i) {
        auto bad = response;
        bad[i] ^= 1;
        require(receive(c, bad).reply.empty(), "tampered RESPONSE accepted");
    }
    require(client_codec.decode(response.data(), response.size(), packet), "response decode");
    packet.payload[0] ^= 1;
    require(receive(c, server_codec.encode(packet)).reply.empty(), "wrong INIT binding accepted");
    ascon::key_type other = key;
    other[0] = 1;
    SessionProtocol wrong(42, other, true), wrong_tunnel(43, key, true);
    require(receive(wrong, init).reply.empty(), "wrong secret accepted");
    require(receive(wrong_tunnel, init).reply.empty(), "wrong tunnel accepted");
}

void test_init_time_and_nonce() {
    constexpr std::int64_t epoch = 10000;
    auto deliver = [](SessionProtocol& server, const Wire& wire, Time now, std::int64_t wall) {
        Packet packet; Wire scratch;
        return server.receive(wire.data(), wire.size(), packet, scratch, now, wall);
    };
    ProtocolV5 encoder(42, key, false), decoder(42, key, true);
    SessionProtocol client(42, key, false);
    const auto init = client.begin(start, epoch);
    Packet parsed;
    require(decoder.decode(init.data(), init.size(), parsed), "timestamp layout");
    require(load_be64(parsed.payload.data() + 32) == epoch, "timestamp encoding");
    for (int offset : {-151, -150, -120, 0, 120, 150, 151}) {
        SessionProtocol server(42, key, true);
        auto r = deliver(server, init, start, epoch - offset);
        require(r.timestamp_rejected == (offset < -150 or offset > 150), "time boundary");
        require(r.clock_warning == (offset <= -120 or offset >= 120), "clock warning boundary");
        require(r.clock_offset == offset, "clock offset sign");
        require(r.reply.empty() == r.timestamp_rejected, "timestamp acceptance");
    }
    SessionProtocol server(42, key, true);
    require(!deliver(server, init, start, epoch).reply.empty(), "initial acceptance");
    auto altered = parsed;
    store_be64(altered.payload.data() + 32, epoch + 1);
    altered.message_id++;
    require(deliver(server, encoder.encode(altered), start, epoch).replay_drop,
            "same nonce with different timestamp/exchange accepted");
    // More than 64 admissions must never evict a still-valid nonce.
    for (int i = 1; i <= 70; ++i) {
        auto fresh = client.begin(start, epoch);
        require(!deliver(server, fresh, start + std::chrono::seconds(10 * i), epoch).reply.empty(),
                "fresh nonce rejected");
    }
    require(deliver(server, init, start + std::chrono::seconds(710), epoch).replay_drop,
            "nonce evicted after 64 entries");
    require(deliver(server, init, start, epoch + 151).timestamp_rejected, "stale replay accepted");
    // Trigger expiry cleanup, then roll back the local wall clock.
    auto later = client.begin(start, epoch + 151);
    deliver(server, later, start, epoch + 151);
    require(deliver(server, init, start, epoch).timestamp_rejected, "rollback resurrected INIT");
    SessionProtocol restarted(42, key, true);
    require(deliver(restarted, init, start, epoch + 151).timestamp_rejected,
            "restart accepted stale INIT");
    auto tampered = init; tampered.at(protocol_handshake_v5_size + 32) ^= 1;
    auto invalid = deliver(restarted, tampered, start, epoch);
    require(!invalid.clock_warning && !invalid.timestamp_rejected, "unauthenticated clock warning");
    // Saturation must fail closed, and expiry must free capacity.
    SessionProtocol full(42, key, true);
    for (std::uint64_t i = 0; i < 65536; ++i) {
        auto entry = parsed;
        store_be64(entry.payload.data(), i);
        auto r = deliver(full, encoder.encode(entry), start, epoch);
        require(!r.nonce_capacity && !r.replay_drop, "early capacity rejection");
    }
    auto extra = parsed;
    store_be64(extra.payload.data(), 65536);
    require(deliver(full, encoder.encode(extra), start, epoch).nonce_capacity,
            "capacity limit ignored");
    store_be64(extra.payload.data() + 32, epoch + 151);
    require(!deliver(full, encoder.encode(extra), start + std::chrono::seconds(151), epoch + 151).reply.empty(),
            "expired nonces did not release capacity");
    SessionProtocol custom(42, key, true, default_tun_mtu, false, 600);
    require(!deliver(custom, init, start, epoch + 250).reply.empty(), "custom window ignored");

    // Lost RESPONSE: the same INIT retrieves the bounded pending RESPONSE.
    SessionProtocol c(42, key, false), s(42, key, true);
    auto first = c.begin(start, epoch);
    auto response = deliver(s, first, start, epoch).reply;
    auto retry = c.tick(start + std::chrono::seconds(1), epoch + 1);
    require(retry == first, "retry changed live INIT");
    require(deliver(s, retry, start + std::chrono::seconds(1), epoch + 1).reply == response,
            "retry did not return cached RESPONSE");
    auto confirm = deliver(c, response, start + std::chrono::seconds(1), epoch + 1).reply;
    auto ack = deliver(s, confirm, start + std::chrono::seconds(1), epoch + 1).reply;
    require(deliver(c, ack, start + std::chrono::seconds(1), epoch + 1).activated,
            "lost RESPONSE recovery failed");
}

int main() {
    log_level = LogLevel::quiet;
    test_handshake_reordering_retransmission();
    test_restart_and_timeouts();
    test_sessions_reassembly_collision_and_overflow();
    test_invalid_handshake();
    test_init_time_and_nonce();
    std::cout << "PASS: V5 handshake, retransmits, restart replay, reordering, hint collisions, reassembly, overflow and validation\n";
}
