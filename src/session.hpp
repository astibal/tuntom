#pragma once

#include "protocol.hpp"
#include "akdf.hpp"
#include "x25519.hpp"
#include "replay.hpp"
#include "reassembly.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <map>
#include <set>
#include <limits>
#include <stdexcept>
#include <vector>
#include <sys/random.h>

namespace tuntom {

// V4 session state is independent of sockets/TUN; callers supply a monotonic
// clock for timeouts and an optional Unix time for INIT freshness tests.
class SessionProtocol {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr std::uint64_t counter_mask = 0x0000ffffffffffffULL;
    static constexpr auto retry_interval = std::chrono::seconds(1);
    static constexpr auto pending_lifetime = std::chrono::seconds(5);
    static constexpr auto old_lifetime = std::chrono::seconds(3);
    static constexpr auto rekey_interval = std::chrono::minutes(2);
    static constexpr auto idle_restart = std::chrono::seconds(20);

    struct Session {
        ProtocolV4 codec;
        ReplayWindow replay;
        Reassembler reassembly;
        std::uint16_t hint;
        Time created {};
        std::uint64_t next_counter = 1;
        std::uint64_t exchange;
        std::vector<std::uint8_t> init, response;
        Session(std::uint16_t id, const ascon::key_type& tx,
                const ascon::key_type& rx, std::uint16_t h,
                std::uint64_t ex, std::size_t mtu, bool encrypt)
            : codec(id, tx, rx, encrypt), reassembly(mtu), hint(h), exchange(ex) {}
    };
    struct Received {
        bool data = false;
        bool control = false;
        bool replay_drop = false;
        bool activated = false;
        bool update_peer = false;
        bool timestamp_rejected = false;
        bool clock_warning = false;
        bool nonce_capacity = false;
        std::int64_t clock_offset = 0;
        Session* session = nullptr;
        std::vector<std::uint8_t> reply;
    };

    SessionProtocol(std::uint16_t id, const ascon::key_type& master,
                    bool server, std::size_t mtu = default_tun_mtu, bool encrypt = false,
                    std::size_t init_window = 300, bool pfs = false)
        : id_(id), master_(master), server_(server), mtu_(mtu), encrypt_(encrypt or pfs), pfs_(pfs),
          handshake_(id, master, server), init_window_(init_window) {
        if (init_window < 2 or init_window > 86400 or init_window % 2 != 0)
            throw std::runtime_error("INIT window must be even and in range 2..86400 seconds");
    }

    ~SessionProtocol() { secure_zero(master_.data(), master_.size()); }

    static std::int64_t wall_seconds() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Bound encrypted-suite key use independently of the 48-bit wire counter.
    // At UDP packet sizes this stays below 2^48 bytes per directional key.
    bool ready() const {
        return active_ and active_->next_counter <= (encrypt_ ? 0xffffffffULL : counter_mask);
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

    std::vector<std::uint8_t> begin(Time now, std::int64_t wall = wall_seconds()) {
        if (server_) return {};
        previous_.reset();
        pending_.reset();
        waiting_ack_ = false;
        Packet init = control(PacketType::init, random_id());
        client_secret_.clear();
        init.payload.resize(pfs_ ? 76 : 44);
        random_bytes(init.payload.data(), 32);
        store_be64(init.payload.data() + 32, static_cast<std::uint64_t>(std::max<std::int64_t>(0, wall)));
        store_be16(init.payload.data() + 40, suite());
        if (pfs_) {
            random_bytes(client_secret_.bytes.data(), 32);
            x25519::Bytes pub {};
            x25519::public_key(pub, client_secret_.bytes);
            store_be16(init.payload.data() + 42, 32);
            std::copy(pub.begin(), pub.end(), init.payload.begin() + 44);
        }
        client_init_ = handshake_.encode(init);
        client_exchange_ = init.message_id;
        flight_ = client_init_;
        flight_started_ = last_retry_ = now;
        return flight_;
    }

    std::vector<std::uint8_t> tick(Time now, std::int64_t wall = wall_seconds()) {
        if (previous_ and now >= previous_until_) previous_.reset();
        if (pending_ and now >= pending_until_) pending_.reset();
        if (server_) return {};
        if (not flight_.empty()) {
            if (now - flight_started_ >= pending_lifetime) {
                // An unconfirmed candidate must not remain usable forever.
                if (waiting_ack_) active_ = std::move(previous_);
                return begin(now, wall);
            }
            if (now - last_retry_ >= retry_interval) {
                if (not waiting_ack_) return begin(now, wall);
                last_retry_ = now;
                return flight_;
            }
            return {};
        }
        if (not ready() or now - last_received_ >= idle_restart or
            (pfs_ and now - active_->created >= rekey_interval)) return begin(now, wall);
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
                     std::vector<std::uint8_t>& scratch, Time now,
                     std::int64_t wall = wall_seconds()) {
        Received result;
        if (previous_ and now >= previous_until_) previous_.reset();
        if (pending_ and now >= pending_until_) pending_.reset();
        if (size < protocol_header_v4_size) return result;
        const auto type = static_cast<PacketType>(wire[7] & 0x7f);
        if (not server_ and not flight_.empty() and
            now - flight_started_ >= pending_lifetime and
            (type == PacketType::response or type == PacketType::confirm_ack)) return result;
        if (type == PacketType::init or type == PacketType::response) {
            if ((server_ and type != PacketType::init) or
                (not server_ and type != PacketType::response)) return result;
            if (not handshake_.decode_with_scratch(wire, size, packet, scratch)) return result;
            if (load_be16(packet.payload.data() + (type == PacketType::init ? 40 : 64)) !=
                suite()) return result;
            std::vector<std::uint8_t> encoded(wire, wire + size);
            if (server_) {
                result.control = true;
                // Never move acceptance time backwards: expired nonce entries
                // must not become replayable after a wall-clock rollback.
                wall_high_water_ = std::max(wall_high_water_, std::max<std::int64_t>(0, wall));
                const auto stamp = load_be64(packet.payload.data() + 32);
                const auto current = static_cast<std::uint64_t>(wall_high_water_);
                const auto half = init_window_ / 2;
                const auto distance = stamp > current ? stamp - current : current - stamp;
                const auto magnitude = static_cast<std::int64_t>(std::min<std::uint64_t>(
                    distance, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
                result.clock_offset = stamp >= current ? magnitude : -magnitude;
                result.timestamp_rejected = distance > half;
                result.clock_warning = distance >= (half * 4 + 4) / 5;
                if (result.timestamp_rejected) return result;
                while (not nonce_expiry_.empty() and nonce_expiry_.begin()->first < current) {
                    seen_nonces_.erase(nonce_expiry_.begin()->second);
                    nonce_expiry_.erase(nonce_expiry_.begin());
                }
                Nonce nonce {};
                std::copy_n(packet.payload.begin(), nonce.size(), nonce.begin());
                if (seen_nonces_.count(nonce)) {
                    result.replay_drop = true;
                    return result;
                }
                if (seen_nonces_.size() >= nonce_capacity) {
                    result.nonce_capacity = true;
                    return result;
                }
                seen_nonces_.insert(nonce);
                nonce_expiry_.emplace(stamp + half, nonce);
                // Fresh retries cannot evict an exchange awaiting CONFIRM.
                if (pending_) return result;
                Packet response = control(PacketType::response, packet.message_id);
                response.payload = commitment(master_, id_, encoded);
                response.payload.resize(pfs_ ? 100 : 68);
                random_bytes(response.payload.data() + 32, 32);
                store_be16(response.payload.data() + 64, suite());
                Secret<32> secret, dh;
                if (pfs_) {
                    random_bytes(secret.bytes.data(), 32);
                    x25519::Bytes peer {}, pub {};
                    std::copy_n(packet.payload.begin() + 44, 32, peer.begin());
                    if (not x25519::shared(dh.bytes, secret.bytes, peer)) return result;
                    x25519::public_key(pub, secret.bytes);
                    store_be16(response.payload.data() + 66, 32);
                    std::copy(pub.begin(), pub.end(), response.payload.begin() + 68);
                }
                auto response_wire = handshake_.encode(response);
                previous_.reset(); // At most two candidate session keys.
                pending_ = derive(encoded, response_wire, packet.message_id, dh.bytes, now);
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
            Secret<32> dh;
            if (pfs_) {
                x25519::Bytes peer {};
                std::copy_n(packet.payload.begin() + 68, 32, peer.begin());
                if (not x25519::shared(dh.bytes, client_secret_.bytes, peer)) return result;
            }
            auto candidate = derive(client_init_, encoded, client_exchange_, dh.bytes, now);
            client_secret_.clear();
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
    std::uint16_t suite() const { return pfs_ ? 2 : (encrypt_ ? 1 : 0); }

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
                                    std::uint64_t exchange, const x25519::Bytes& dh, Time now) const {
        auto transcript = init;
        transcript.insert(transcript.end(), response.begin(), response.end());
        Secret<16> c2s, s2c, hint;
        if (pfs_) {
            Secret<16> prk;
            akdf::extract(prk.bytes, master_, id_, dh);
            akdf::expand(c2s.bytes, prk.bytes, id_, "TUNTOM-AKDF-v1-C2S", transcript);
            akdf::expand(s2c.bytes, prk.bytes, id_, "TUNTOM-AKDF-v1-S2C", transcript);
            akdf::expand(hint.bytes, prk.bytes, id_, "TUNTOM-AKDF-v1-HINT", transcript);
        } else {
            c2s.bytes = expand(master_, id_, "V4-SESSION-C2S", transcript);
            s2c.bytes = expand(master_, id_, "V4-SESSION-S2C", transcript);
            hint.bytes = expand(master_, id_, "V4-SESSION-HINT", transcript);
        }
        auto result = std::make_unique<Session>(id_, server_ ? s2c.bytes : c2s.bytes,
            server_ ? c2s.bytes : s2c.bytes, load_be16(hint.bytes.data()), exchange, mtu_, encrypt_);
        result->created = now;
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
    bool encrypt_;
    bool pfs_;
    Secret<32> client_secret_;
    ProtocolV4 handshake_;
    std::unique_ptr<Session> active_, previous_, pending_;
    Time previous_until_ {}, pending_until_ {}, last_received_ {};
    Time flight_started_ {}, last_retry_ {};
    bool waiting_ack_ = false;
    std::uint64_t client_exchange_ = 0;
    std::vector<std::uint8_t> client_init_, flight_;
    using Nonce = std::array<std::uint8_t, 32>;
    static constexpr std::size_t nonce_capacity = 65536;
    std::set<Nonce> seen_nonces_;
    std::multimap<std::uint64_t, Nonce> nonce_expiry_;
    std::size_t init_window_;
    std::int64_t wall_high_water_ = 0;
};

} // namespace tuntom
