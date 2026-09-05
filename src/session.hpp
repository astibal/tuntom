#pragma once

#include "protocol.hpp"
#include "replay.hpp"
#include "reassembly.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include <sys/random.h>

namespace tuntom {

// V4 session state is independent of sockets/TUN; callers supply a monotonic
// clock so timeout and reordering behavior can be tested without sleeping.
class SessionProtocol {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr std::uint64_t counter_mask = 0x0000ffffffffffffULL;
    static constexpr auto retry_interval = std::chrono::seconds(1);
    static constexpr auto pending_lifetime = std::chrono::seconds(10);
    static constexpr auto old_lifetime = std::chrono::seconds(3);
    static constexpr auto idle_restart = std::chrono::seconds(20);

    struct Session {
        ProtocolV4 codec;
        ReplayWindow replay;
        Reassembler reassembly;
        std::uint16_t hint;
        std::uint64_t next_counter = 1;
        std::uint64_t exchange;
        std::vector<std::uint8_t> init, response;
        Session(std::uint16_t id, const ascon::key_type& tx,
                const ascon::key_type& rx, std::uint16_t h,
                std::uint64_t ex, std::size_t mtu)
            : codec(id, tx, rx), reassembly(mtu), hint(h), exchange(ex) {}
    };
    struct Received {
        bool data = false;
        bool control = false;
        bool replay_drop = false;
        bool activated = false;
        bool update_peer = false;
        Session* session = nullptr;
        std::vector<std::uint8_t> reply;
    };

    SessionProtocol(std::uint16_t id, const ascon::key_type& master,
                    bool server, std::size_t mtu = default_tun_mtu)
        : id_(id), master_(master), server_(server), mtu_(mtu),
          handshake_(id, master, server) {}

    bool ready() const {
        return active_ and active_->next_counter <= counter_mask;
    }

    // Expansion is the existing AMAC with explicit, NUL-delimited domains.
    // init_hash is a 32-byte keyed commitment (two independent AMAC domains),
    // not an unkeyed hash or a standardized Ascon hash profile.
    static std::vector<std::uint8_t> commitment(
        const ascon::key_type& master, std::uint16_t id,
        const std::vector<std::uint8_t>& init) {
        auto a = expand(master, id, "V4-INIT-BIND-0", init);
        auto b = expand(master, id, "V4-INIT-BIND-1", init);
        std::vector<std::uint8_t> result(a.size() + b.size());
        std::copy(a.begin(), a.end(), result.begin());
        std::copy(b.begin(), b.end(), result.begin() + a.size());
        return result;
    }

    std::vector<std::uint8_t> begin(Time now) {
        if (server_) return {};
        previous_.reset();
        pending_.reset();
        waiting_ack_ = false;
        Packet init = control(PacketType::init, random_id());
        init.payload.resize(36);
        random_bytes(init.payload.data(), 32);
        client_init_ = handshake_.encode(init);
        client_exchange_ = init.message_id;
        flight_ = client_init_;
        flight_started_ = last_retry_ = now;
        return flight_;
    }

    std::vector<std::uint8_t> tick(Time now) {
        if (previous_ and now >= previous_until_) previous_.reset();
        if (pending_ and now >= pending_until_) pending_.reset();
        if (server_) return {};
        if (not flight_.empty()) {
            if (now - flight_started_ >= pending_lifetime) {
                // An unconfirmed candidate must not remain usable forever.
                if (waiting_ack_) active_ = std::move(previous_);
                return begin(now);
            }
            if (now - last_retry_ >= retry_interval) {
                last_retry_ = now;
                return flight_;
            }
            return {};
        }
        if (not ready() or now - last_received_ >= idle_restart) return begin(now);
        return {};
    }

    bool encode_into(Packet& packet, std::vector<std::uint8_t>& output,
                     std::vector<std::uint8_t>& scratch) {
        output.clear();
        if (not ready()) return false;
        packet.sequence = (std::uint64_t(active_->hint) << 48) |
                          active_->next_counter++;
        active_->codec.encode_into(packet, output, scratch);
        return true;
    }

    std::vector<std::uint8_t> encode(Packet& packet) {
        std::vector<std::uint8_t> output, scratch;
        encode_into(packet, output, scratch);
        return output;
    }

    Received receive(const std::uint8_t* wire, std::size_t size, Packet& packet,
                     std::vector<std::uint8_t>& scratch, Time now) {
        Received result;
        if (previous_ and now >= previous_until_) previous_.reset();
        if (pending_ and now >= pending_until_) pending_.reset();
        if (size < protocol_header_v4_size) return result;
        const auto type = static_cast<PacketType>(wire[7]);
        if (not server_ and not flight_.empty() and
            now - flight_started_ >= pending_lifetime and
            (type == PacketType::response or type == PacketType::confirm_ack)) return result;
        if (type == PacketType::init or type == PacketType::response) {
            if ((server_ and type != PacketType::init) or
                (not server_ and type != PacketType::response)) return result;
            if (not handshake_.decode_with_scratch(wire, size, packet, scratch)) return result;
            std::vector<std::uint8_t> encoded(wire, wire + size);
            if (server_) {
                result.control = true;
                if (active_ and active_->init == encoded) {
                    result.reply = active_->response;
                    return result;
                }
                if (pending_) {
                    // A duplicate doesn't extend the fixed deadline; another
                    // INIT cannot evict the one pending authenticated exchange.
                    if (pending_->init == encoded) result.reply = pending_->response;
                    return result;
                }
                // Don't let a captured INIT repeatedly monopolize the pending
                // slot after its deadline. Bounded recent history is per boot.
                if (std::find(recent_inits_.begin(), recent_inits_.end(), encoded) !=
                    recent_inits_.end()) return result;
                if (recent_inits_.size() == 64) recent_inits_.erase(recent_inits_.begin());
                recent_inits_.push_back(encoded);
                Packet response = control(PacketType::response, packet.message_id);
                response.payload = commitment(master_, id_, encoded);
                response.payload.resize(68);
                random_bytes(response.payload.data() + 32, 32);
                auto response_wire = handshake_.encode(response);
                previous_.reset(); // At most two candidate session keys.
                pending_ = derive(encoded, response_wire, packet.message_id);
                pending_until_ = now + pending_lifetime;
                result.reply = std::move(response_wire);
                return result;
            }
            if (client_init_.empty() or packet.message_id != client_exchange_) return result;
            const auto binding = commitment(master_, id_, client_init_);
            if (not ascon::constant_time_equal(binding.data(), packet.payload.data(), 32)) return result;
            result.control = true;
            if (waiting_ack_) {
                // Never reset counters/replay state on a repeated response.
                if (active_ and active_->response == encoded) result.reply = flight_;
                return result;
            }
            auto candidate = derive(client_init_, encoded, client_exchange_);
            previous_ = std::move(active_);
            previous_until_ = now + old_lifetime;
            active_ = std::move(candidate);
            waiting_ack_ = true;
            flight_ = confirmation(*active_, PacketType::confirm);
            flight_started_ = last_retry_ = now;
            result.reply = flight_;
            return result;
        }

        const auto hint = static_cast<std::uint16_t>(load_be64(wire + 8) >> 48);
        Session* matched = nullptr;
        for (Session* candidate : {active_.get(), pending_.get(), previous_.get()}) {
            if (candidate and candidate->hint == hint and
                candidate->codec.decode_with_scratch(wire, size, packet, scratch)) {
                matched = candidate;
                break;
            }
        }
        if (not matched) return result;
        if (type == PacketType::confirm) {
            if (not server_ or packet.message_id != matched->exchange) return result;
            result.control = true;
            if (matched == pending_.get()) {
                previous_ = std::move(active_);
                previous_until_ = now + old_lifetime;
                active_ = std::move(pending_);
                result.activated = result.update_peer = true;
            }
            if (matched == active_.get()) {
                result.reply = confirmation(*matched, PacketType::confirm_ack);
            }
            return result;
        }
        if (type == PacketType::confirm_ack) {
            if (server_ or matched != active_.get() or
                packet.message_id != matched->exchange) return result;
            result.control = true;
            if (not waiting_ack_) return result;
            waiting_ack_ = false;
            flight_.clear();
            client_init_.clear();
            last_received_ = now;
            result.activated = true;
            return result;
        }
        // No DATA before server-side CONFIRM. Counter zero belongs exclusively
        // to CONFIRM/ACK and is never inserted into the DATA replay window.
        if (matched == pending_.get()) return result;
        if (not matched->replay.accept(packet.sequence & counter_mask)) {
            result.replay_drop = true;
            return result;
        }
        if (matched == active_.get()) {
            last_received_ = now;
            result.update_peer = server_;
        }
        result.data = true;
        result.session = matched;
        return result;
    }

    void cleanup() {
        if (active_) active_->reassembly.cleanup_expired();
        if (previous_) previous_->reassembly.cleanup_expired();
    }

private:
    static ascon::key_type expand(const ascon::key_type& master, std::uint16_t id,
                                  const char* label,
                                  const std::vector<std::uint8_t>& transcript) {
        std::vector<std::uint8_t> input(label, label + std::strlen(label) + 1);
        input.insert(input.end(), transcript.begin(), transcript.end());
        ascon::key_type output {};
        ascon::mac(master, id, input.data(), input.size(), output);
        return output;
    }

    std::unique_ptr<Session> derive(const std::vector<std::uint8_t>& init,
                                    const std::vector<std::uint8_t>& response,
                                    std::uint64_t exchange) const {
        auto transcript = init;
        transcript.insert(transcript.end(), response.begin(), response.end());
        const auto c2s = expand(master_, id_, "V4-SESSION-C2S", transcript);
        const auto s2c = expand(master_, id_, "V4-SESSION-S2C", transcript);
        const auto hint = expand(master_, id_, "V4-SESSION-HINT", transcript);
        auto result = std::make_unique<Session>(id_, server_ ? s2c : c2s,
            server_ ? c2s : s2c, load_be16(hint.data()), exchange, mtu_);
        result->init = init;
        result->response = response;
        return result;
    }

    Packet control(PacketType type, std::uint64_t exchange) const {
        Packet packet;
        packet.type = type;
        packet.tunnel_id = id_;
        packet.message_id = exchange;
        return packet;
    }
    std::vector<std::uint8_t> confirmation(Session& session, PacketType type) const {
        auto packet = control(type, session.exchange);
        packet.sequence = std::uint64_t(session.hint) << 48;
        return session.codec.encode(packet);
    }
    static void random_bytes(std::uint8_t* output, std::size_t size) {
        while (size != 0) {
            const auto n = ::getrandom(output, size, 0);
            if (n < 0 and errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("getrandom failed");
            output += n;
            size -= static_cast<std::size_t>(n);
        }
    }
    static std::uint64_t random_id() {
        std::array<std::uint8_t, 8> bytes {};
        std::uint64_t value = 0;
        while (value == 0) {
            random_bytes(bytes.data(), bytes.size());
            value = load_be64(bytes.data());
        }
        return value;
    }

    std::uint16_t id_;
    ascon::key_type master_;
    bool server_;
    std::size_t mtu_;
    ProtocolV4 handshake_;
    std::unique_ptr<Session> active_, previous_, pending_;
    Time previous_until_ {}, pending_until_ {}, last_received_ {};
    Time flight_started_ {}, last_retry_ {};
    bool waiting_ack_ = false;
    std::uint64_t client_exchange_ = 0;
    std::vector<std::uint8_t> client_init_, flight_;
    std::vector<std::vector<std::uint8_t>> recent_inits_;
};

} // namespace tuntom
