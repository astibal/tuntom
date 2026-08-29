#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <netdb.h>
#include <net/if.h>
#include <poll.h>
#include <pwd.h>
#include <grp.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t protocol_magic = 0x5554554e; // "UTUN"
constexpr std::uint8_t protocol_version_v1 = 1;
constexpr std::uint8_t protocol_version_v2 = 2;
constexpr std::uint8_t protocol_version_v3 = 3;

constexpr std::size_t protocol_header_v1_size = 8;
constexpr std::size_t protocol_header_v2_size = 32;
constexpr std::size_t protocol_header_v3_size = 48;

constexpr std::size_t udp_header_size = 8;
constexpr std::size_t ipv4_header_min_size = 20;
constexpr std::size_t ipv6_header_size = 40;
constexpr std::size_t buffer_size = 65536;

constexpr std::size_t default_tun_mtu = 1500;
constexpr std::size_t default_transport_mtu = 1400;
constexpr std::size_t min_transport_mtu = 1280;
constexpr std::size_t max_ip_packet_size = 65535;

constexpr int keepalive_seconds = 5;
constexpr int rtt_probe_interval_seconds = 15;
constexpr int rtt_probe_timeout_seconds = 5;
constexpr int reassembly_timeout_seconds = 3;
constexpr std::size_t max_reassembly_entries = 64;
constexpr std::size_t max_reassembly_bytes = 4 * 1024 * 1024;
constexpr std::size_t max_fragments_per_packet = 64;

constexpr const char* runtime_user = "tuntom";
constexpr const char* runtime_group = "tuntom";

enum class LogLevel {
    quiet = 0,
    info = 1,
    debug = 2,
};

LogLevel log_level = LogLevel::info;

bool log_enabled(LogLevel level) {
    return static_cast<int>(log_level) >= static_cast<int>(level);
}

void log_info(const std::string& message) {
    if (log_enabled(LogLevel::info)) {
        std::cerr << message << "\n";
    }
}

void harden_process_before_privilege_drop() {
    ::umask(0077);

    rlimit core_limit {};
    core_limit.rlim_cur = 0;
    core_limit.rlim_max = 0;

    if (::setrlimit(RLIMIT_CORE, &core_limit) != 0) {
        throw std::runtime_error(
            "setrlimit(RLIMIT_CORE) failed: " +
            std::string(std::strerror(errno)));
    }
}

void verify_supplementary_groups(gid_t expected_gid) {
    const int group_count = ::getgroups(0, nullptr);

    if (group_count < 0) {
        throw std::runtime_error(
            "getgroups() failed: " +
            std::string(std::strerror(errno)));
    }

    std::vector<gid_t> groups(
        static_cast<std::size_t>(group_count));

    if (
        group_count > 0 and
        ::getgroups(group_count, groups.data()) < 0) {

        throw std::runtime_error(
            "getgroups() failed: " +
            std::string(std::strerror(errno)));
    }

    for (const gid_t group_id : groups) {
        if (group_id == 0) {
            throw std::runtime_error(
                "Privilege drop left root supplementary group");
        }
    }

    if (
        not groups.empty() and
        std::find(
            groups.begin(),
            groups.end(),
            expected_gid) == groups.end()) {

        throw std::runtime_error(
            "Runtime group missing from supplementary groups");
    }
}

void drop_privileges() {
    if (::geteuid() != 0) {
        throw std::runtime_error(
            "tuntom must start as root in order to initialize networking "
            "and drop privileges");
    }

    harden_process_before_privilege_drop();

    passwd* user = ::getpwnam(runtime_user);
    if (user == nullptr) {
        throw std::runtime_error(
            std::string("Runtime user does not exist: ") + runtime_user);
    }

    group* runtime_group_entry = ::getgrnam(runtime_group);
    if (runtime_group_entry == nullptr) {
        throw std::runtime_error(
            std::string("Runtime group does not exist: ") + runtime_group);
    }

    const uid_t uid = user->pw_uid;
    const gid_t gid = runtime_group_entry->gr_gid;

    if (::initgroups(runtime_user, gid) != 0) {
        throw std::runtime_error(
            "initgroups() failed: " +
            std::string(std::strerror(errno)));
    }

    if (::setresgid(gid, gid, gid) != 0) {
        throw std::runtime_error(
            "setresgid() failed: " +
            std::string(std::strerror(errno)));
    }

    if (::setresuid(uid, uid, uid) != 0) {
        throw std::runtime_error(
            "setresuid() failed: " +
            std::string(std::strerror(errno)));
    }

    if (
        ::prctl(
            PR_SET_NO_NEW_PRIVS,
            1,
            0,
            0,
            0) != 0) {

        throw std::runtime_error(
            "prctl(PR_SET_NO_NEW_PRIVS) failed: " +
            std::string(std::strerror(errno)));
    }

    if (
        ::prctl(
            PR_SET_DUMPABLE,
            0,
            0,
            0,
            0) != 0) {

        throw std::runtime_error(
            "prctl(PR_SET_DUMPABLE) failed: " +
            std::string(std::strerror(errno)));
    }

    if (
        ::getuid() != uid or
        ::geteuid() != uid or
        ::getgid() != gid or
        ::getegid() != gid) {

        throw std::runtime_error(
            "Privilege drop verification failed");
    }

    verify_supplementary_groups(gid);

    log_info(
        std::string("Privileges dropped and hardened as ") +
        runtime_user + ":" + runtime_group);
}


enum class PacketType : std::uint8_t {
    hello = 1,
    keepalive = 2,
    data = 3,
    ping = 4,
    pong = 5,
};

enum class Direction {
    tun_to_udp,
    udp_to_tun,
};

struct Packet {
    PacketType type = PacketType::data;
    std::uint16_t tunnel_id = 0;
    std::uint8_t protocol_version = protocol_version_v3;
    std::uint64_t sequence = 0;

    // V3 DATA uses fragment metadata. PING/PONG use message_id as probe_id.
    std::uint64_t message_id = 0;
    std::uint32_t fragment_offset = 0;
    std::uint32_t original_length = 0;

    std::vector<std::uint8_t> payload;
};

enum class StatsFormat {
    txt,
};

struct Options {
    bool allow_v1 = false;
    bool allow_v2 = false;
    bool ttl_compensate = true;
    std::size_t tun_mtu = default_tun_mtu;
    std::size_t transport_mtu = default_transport_mtu;
    std::string stats_file;
    StatsFormat stats_format = StatsFormat::txt;
};

struct Stats {
    std::uint64_t tun_rx_packets = 0;
    std::uint64_t tun_rx_bytes = 0;
    std::uint64_t tun_tx_packets = 0;
    std::uint64_t tun_tx_bytes = 0;
    std::uint64_t udp_rx_packets = 0;
    std::uint64_t udp_rx_bytes = 0;
    std::uint64_t udp_tx_packets = 0;
    std::uint64_t udp_tx_bytes = 0;
    std::uint64_t data_tx_packets = 0;
    std::uint64_t data_rx_packets = 0;
    std::uint64_t fragments_tx = 0;
    std::uint64_t fragments_rx = 0;
    std::uint64_t drops_protocol = 0;
    std::uint64_t drops_tunnel_id = 0;
    std::uint64_t drops_replay = 0;
    std::uint64_t drops_mtu = 0;
    std::uint64_t drops_process = 0;
    std::uint64_t udp_send_errors = 0;
    std::uint64_t tun_write_errors = 0;

    double rtt_last_ms = 0.0;
    double rtt_min_ms = 0.0;
    double rtt_max_ms = 0.0;
    double rtt_avg_ms = 0.0;
    double rtt_jitter_ms = 0.0;
    std::uint64_t rtt_samples = 0;
    std::uint64_t rtt_lost = 0;
};

void dump_bytes(
    const std::string& prefix,
    const std::uint8_t* data,
    std::size_t size,
    std::size_t max_bytes = 28) {

    if (not log_enabled(LogLevel::debug)) {
        return;
    }

    std::cerr << prefix << " " << size << " bytes:";

    const std::size_t count = std::min(size, max_bytes);
    for (std::size_t i = 0; i < count; ++i) {
        std::cerr << " " << std::hex << static_cast<unsigned>(data[i]);
    }

    std::cerr << std::dec << "\n";
}

namespace ascon {

constexpr std::size_t key_size = 16;
constexpr std::size_t tag_size = 16;

using key_type = std::array<std::uint8_t, key_size>;
using tag_type = std::array<std::uint8_t, tag_size>;

std::uint64_t rotate_right(std::uint64_t value, unsigned shift) {
    return (value >> shift) | (value << (64U - shift));
}

std::uint64_t load_be64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8U) | data[i];
    }
    return value;
}

void store_be64(std::uint8_t* data, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        data[i] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8U;
    }
}

void permute(std::array<std::uint64_t, 5>& state, int rounds) {
    static constexpr std::array<std::uint8_t, 12> round_constants {
        0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5,
        0x96, 0x87, 0x78, 0x69, 0x5a, 0x4b,
    };

    const int first_round = 12 - rounds;
    for (int round = first_round; round < 12; ++round) {
        state[2] ^= round_constants[round];

        state[0] ^= state[4];
        state[4] ^= state[3];
        state[2] ^= state[1];

        const std::uint64_t t0 = not state[0] and state[1];
        const std::uint64_t t1 = not state[1] and state[2];
        const std::uint64_t t2 = not state[2] and state[3];
        const std::uint64_t t3 = not state[3] and state[4];
        const std::uint64_t t4 = not state[4] and state[0];

        state[0] ^= t1;
        state[1] ^= t2;
        state[2] ^= t3;
        state[3] ^= t4;
        state[4] ^= t0;

        state[1] ^= state[0];
        state[0] ^= state[4];
        state[3] ^= state[2];
        state[2] = not state[2];

        state[0] ^= rotate_right(state[0], 19) ^ rotate_right(state[0], 28);
        state[1] ^= rotate_right(state[1], 61) ^ rotate_right(state[1], 39);
        state[2] ^= rotate_right(state[2], 1) ^ rotate_right(state[2], 6);
        state[3] ^= rotate_right(state[3], 10) ^ rotate_right(state[3], 17);
        state[4] ^= rotate_right(state[4], 7) ^ rotate_right(state[4], 41);
    }
}

void mac(
    const key_type& key,
    std::uint16_t tunnel_id,
    const std::uint8_t* data,
    std::size_t size,
    tag_type& tag) {

    const std::uint64_t k0 = load_be64(key.data());
    const std::uint64_t k1 = load_be64(key.data() + 8);

    // Compact Ascon-p[12]/p[8]-based keyed sponge MAC.
    // Kept self-contained for the single-file deployment model.
    std::array<std::uint64_t, 5> state {
        0x54554e544f4d4d41ULL, // "TUNTOMMA" domain separator
        k0,
        k1,
        static_cast<std::uint64_t>(tunnel_id),
        not static_cast<std::uint64_t>(tunnel_id),
    };

    permute(state, 12);
    state[3] ^= k0;
    state[4] ^= k1;

    while (size >= 8) {
        state[0] ^= load_be64(data);
        permute(state, 8);
        data += 8;
        size -= 8;
    }

    std::array<std::uint8_t, 8> final_block {};
    if (size > 0) {
        std::memcpy(final_block.data(), data, size);
    }
    final_block[size] = 0x80;

    state[0] ^= load_be64(final_block.data());
    state[4] ^= 1;

    state[1] ^= k0;
    state[2] ^= k1;
    permute(state, 12);
    state[3] ^= k0;
    state[4] ^= k1;

    store_be64(tag.data(), state[3]);
    store_be64(tag.data() + 8, state[4]);
}

key_type derive_node_key(const key_type& master_key, std::uint16_t tunnel_id) {
    static constexpr std::array<std::uint8_t, 15> label {
        'T', 'U', 'N', 'T', 'O', 'M', '-', 'N', 'O', 'D', 'E', '-', 'K', 'E', 'Y'
    };

    tag_type derived {};
    mac(master_key, tunnel_id, label.data(), label.size(), derived);

    key_type node_key {};
    std::copy(derived.begin(), derived.end(), node_key.begin());
    return node_key;
}

bool constant_time_equal(const std::uint8_t* a, const std::uint8_t* b, std::size_t size) {
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < size; ++i) {
        difference |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    }
    return difference == 0;
}

} // namespace ascon

std::uint64_t load_be64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8U) | data[i];
    }
    return value;
}

void store_be64(std::uint8_t* data, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        data[i] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8U;
    }
}

void store_be16(std::uint8_t* data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    data[1] = static_cast<std::uint8_t>(value & 0xffU);
}

std::uint16_t load_be16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8U) |
        data[1]);
}

std::uint32_t load_be32(const std::uint8_t* data) {
    return
        (static_cast<std::uint32_t>(data[0]) << 24U) |
        (static_cast<std::uint32_t>(data[1]) << 16U) |
        (static_cast<std::uint32_t>(data[2]) << 8U) |
        static_cast<std::uint32_t>(data[3]);
}

void store_be32(std::uint8_t* data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    data[1] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    data[2] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    data[3] = static_cast<std::uint8_t>(value & 0xffU);
}

ascon::key_type parse_master_key() {
    const char* secret = std::getenv("TUNTOM_SECRET");
    if (secret == nullptr) {
        throw std::runtime_error("TUNTOM_SECRET is not set");
    }

    const std::string value(secret);
    if (value.size() != ascon::key_size * 2) {
        throw std::runtime_error("TUNTOM_SECRET must contain exactly 32 hex characters");
    }

    ascon::key_type key {};
    for (std::size_t i = 0; i < key.size(); ++i) {
        const std::string byte_string = value.substr(i * 2, 2);
        std::size_t parsed = 0;
        const unsigned long byte = std::stoul(byte_string, &parsed, 16);
        if (parsed != 2 or byte > 0xffUL) {
            throw std::runtime_error("TUNTOM_SECRET contains invalid hex");
        }
        key[i] = static_cast<std::uint8_t>(byte);
    }

    return key;
}

class SequenceGenerator {
public:
    std::uint64_t next() {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto nanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

        std::uint64_t candidate = static_cast<std::uint64_t>(nanoseconds);

        if (candidate <= last_sequence_) {
            candidate = last_sequence_ + 1;
        }

        last_sequence_ = candidate;
        return candidate;
    }

private:
    std::uint64_t last_sequence_ = 0;
};

class ReplayWindow {
public:
    bool accept(std::uint64_t sequence) {
        if (sequence == 0) {
            return false;
        }

        if (highest_sequence_ == 0) {
            highest_sequence_ = sequence;
            bitmap_ = 1;
            return true;
        }

        if (sequence > highest_sequence_) {
            const std::uint64_t shift = sequence - highest_sequence_;

            if (shift >= 64) {
                bitmap_ = 1;
            } else {
                bitmap_ = (bitmap_ << shift) | 1ULL;
            }

            highest_sequence_ = sequence;
            return true;
        }

        const std::uint64_t distance = highest_sequence_ - sequence;
        if (distance >= 64) {
            return false;
        }

        const std::uint64_t mask = 1ULL << distance;
        if ((bitmap_ & mask) != 0) {
            return false;
        }

        bitmap_ |= mask;
        return true;
    }

private:
    std::uint64_t highest_sequence_ = 0;
    std::uint64_t bitmap_ = 0;
};

class Protocol {
public:
    virtual ~Protocol() = default;

    virtual std::uint8_t version() const = 0;
    virtual std::vector<std::uint8_t> encode(const Packet& packet) const = 0;
    virtual bool decode(
        const std::uint8_t* data,
        std::size_t size,
        Packet& packet) const = 0;
};

class ProtocolV1 final : public Protocol {
public:
    std::uint8_t version() const override {
        return protocol_version_v1;
    }

    std::vector<std::uint8_t> encode(const Packet& packet) const override {
        std::vector<std::uint8_t> output(
            protocol_header_v1_size + packet.payload.size());

        store_be32(output.data() + 0, protocol_magic);
        store_be16(output.data() + 4, packet.tunnel_id);
        output[6] = protocol_version_v1;
        output[7] = static_cast<std::uint8_t>(packet.type);

        if (not packet.payload.empty()) {
            std::memcpy(
                output.data() + protocol_header_v1_size,
                packet.payload.data(),
                packet.payload.size());
        }

        return output;
    }

    bool decode(
        const std::uint8_t* data,
        std::size_t size,
        Packet& packet) const override {

        if (size < protocol_header_v1_size) {
            return false;
        }

        if (
            load_be32(data + 0) != protocol_magic or
            data[6] != protocol_version_v1) {

            return false;
        }

        const auto type = static_cast<PacketType>(data[7]);
        if (
            type != PacketType::hello and
            type != PacketType::keepalive and
            type != PacketType::data) {

            return false;
        }

        packet.type = type;
        packet.tunnel_id = load_be16(data + 4);
        packet.protocol_version = protocol_version_v1;
        packet.sequence = 0;
        packet.message_id = 0;
        packet.fragment_offset = 0;
        packet.original_length = static_cast<std::uint32_t>(
            size - protocol_header_v1_size);
        packet.payload.assign(data + protocol_header_v1_size, data + size);
        return true;
    }
};

class ProtocolV2 final : public Protocol {
public:
    ProtocolV2(
        std::uint16_t tunnel_id,
        const ascon::key_type& master_key)
        : tunnel_id_(tunnel_id),
          node_key_(ascon::derive_node_key(master_key, tunnel_id)) {
    }

    std::uint8_t version() const override {
        return protocol_version_v2;
    }

    std::vector<std::uint8_t> encode(const Packet& packet) const override {
        std::vector<std::uint8_t> output(
            protocol_header_v2_size + packet.payload.size());

        store_be32(output.data() + 0, protocol_magic);
        store_be16(output.data() + 4, packet.tunnel_id);
        output[6] = protocol_version_v2;
        output[7] = static_cast<std::uint8_t>(packet.type);
        store_be64(output.data() + 8, packet.sequence);

        if (not packet.payload.empty()) {
            std::memcpy(
                output.data() + protocol_header_v2_size,
                packet.payload.data(),
                packet.payload.size());
        }

        std::vector<std::uint8_t> mac_input(16 + packet.payload.size());
        std::memcpy(mac_input.data(), output.data(), 16);

        if (not packet.payload.empty()) {
            std::memcpy(
                mac_input.data() + 16,
                packet.payload.data(),
                packet.payload.size());
        }

        ascon::tag_type tag {};
        ascon::mac(
            node_key_,
            tunnel_id_,
            mac_input.data(),
            mac_input.size(),
            tag);

        std::memcpy(output.data() + 16, tag.data(), tag.size());

        return output;
    }

    bool decode(
        const std::uint8_t* data,
        std::size_t size,
        Packet& packet) const override {

        if (size < protocol_header_v2_size) {
            return false;
        }

        if (
            load_be32(data + 0) != protocol_magic or
            data[6] != protocol_version_v2) {

            return false;
        }

        const std::uint16_t tunnel_id = load_be16(data + 4);
        if (tunnel_id != tunnel_id_) {
            return false;
        }

        const auto type = static_cast<PacketType>(data[7]);
        if (
            type != PacketType::hello and
            type != PacketType::keepalive and
            type != PacketType::data) {

            return false;
        }

        std::vector<std::uint8_t> mac_input(
            16 + size - protocol_header_v2_size);

        std::memcpy(mac_input.data(), data, 16);

        if (size > protocol_header_v2_size) {
            std::memcpy(
                mac_input.data() + 16,
                data + protocol_header_v2_size,
                size - protocol_header_v2_size);
        }

        ascon::tag_type expected_tag {};
        ascon::mac(
            node_key_,
            tunnel_id_,
            mac_input.data(),
            mac_input.size(),
            expected_tag);

        if (
            not ascon::constant_time_equal(
                data + 16,
                expected_tag.data(),
                expected_tag.size())) {

            return false;
        }

        packet.type = type;
        packet.tunnel_id = tunnel_id;
        packet.protocol_version = protocol_version_v2;
        packet.sequence = load_be64(data + 8);
        packet.message_id = 0;
        packet.fragment_offset = 0;
        packet.original_length = static_cast<std::uint32_t>(
            size - protocol_header_v2_size);
        packet.payload.assign(data + protocol_header_v2_size, data + size);
        return true;
    }

private:
    std::uint16_t tunnel_id_ = 0;
    ascon::key_type node_key_ {};
};

class ProtocolV3 final : public Protocol {
public:
    ProtocolV3(
        std::uint16_t tunnel_id,
        const ascon::key_type& master_key)
        : tunnel_id_(tunnel_id),
          node_key_(ascon::derive_node_key(master_key, tunnel_id)) {
    }

    std::uint8_t version() const override {
        return protocol_version_v3;
    }

    std::vector<std::uint8_t> encode(const Packet& packet) const override {
        std::vector<std::uint8_t> output(
            protocol_header_v3_size + packet.payload.size());

        // Bytes 0..31 are authenticated metadata.
        store_be32(output.data() + 0, protocol_magic);
        store_be16(output.data() + 4, packet.tunnel_id);
        output[6] = protocol_version_v3;
        output[7] = static_cast<std::uint8_t>(packet.type);
        store_be64(output.data() + 8, packet.sequence);
        store_be64(output.data() + 16, packet.message_id);
        store_be32(output.data() + 24, packet.fragment_offset);
        store_be32(output.data() + 28, packet.original_length);

        if (not packet.payload.empty()) {
            std::memcpy(
                output.data() + protocol_header_v3_size,
                packet.payload.data(),
                packet.payload.size());
        }

        // The tag itself occupies bytes 32..47 and is not part of the MAC
        // input. The complete V3 metadata (0..31) plus payload is covered.
        std::vector<std::uint8_t> mac_input(32 + packet.payload.size());
        std::memcpy(mac_input.data(), output.data(), 32);

        if (not packet.payload.empty()) {
            std::memcpy(
                mac_input.data() + 32,
                packet.payload.data(),
                packet.payload.size());
        }

        ascon::tag_type tag {};
        ascon::mac(
            node_key_,
            tunnel_id_,
            mac_input.data(),
            mac_input.size(),
            tag);

        std::memcpy(output.data() + 32, tag.data(), tag.size());

        if (log_enabled(LogLevel::debug)) {
            std::cerr
                << "ENCODE v3 seq=" << packet.sequence
                << " msg=" << packet.message_id
                << " offset=" << packet.fragment_offset
                << " original=" << packet.original_length
                << " payload=" << packet.payload.size()
                << " output=" << output.size()
                << "\n";
        }

        return output;
    }

    bool decode(
        const std::uint8_t* data,
        std::size_t size,
        Packet& packet) const override {

        if (size < protocol_header_v3_size) {
            return false;
        }

        if (
            load_be32(data + 0) != protocol_magic or
            data[6] != protocol_version_v3) {

            return false;
        }

        const std::uint16_t tunnel_id = load_be16(data + 4);
        if (tunnel_id != tunnel_id_) {
            return false;
        }

        const auto type = static_cast<PacketType>(data[7]);
        if (
            type != PacketType::hello and
            type != PacketType::keepalive and
            type != PacketType::data and
            type != PacketType::ping and
            type != PacketType::pong) {

            return false;
        }

        std::vector<std::uint8_t> mac_input(
            32 + size - protocol_header_v3_size);

        std::memcpy(mac_input.data(), data, 32);

        if (size > protocol_header_v3_size) {
            std::memcpy(
                mac_input.data() + 32,
                data + protocol_header_v3_size,
                size - protocol_header_v3_size);
        }

        ascon::tag_type expected_tag {};
        ascon::mac(
            node_key_,
            tunnel_id_,
            mac_input.data(),
            mac_input.size(),
            expected_tag);

        if (
            not ascon::constant_time_equal(
                data + 32,
                expected_tag.data(),
                expected_tag.size())) {

            return false;
        }

        packet.type = type;
        packet.tunnel_id = tunnel_id;
        packet.protocol_version = protocol_version_v3;
        packet.sequence = load_be64(data + 8);
        packet.message_id = load_be64(data + 16);
        packet.fragment_offset = load_be32(data + 24);
        packet.original_length = load_be32(data + 28);
        packet.payload.assign(data + protocol_header_v3_size, data + size);

        if (packet.type == PacketType::data) {
            if (
                packet.message_id == 0 or
                packet.original_length == 0 or
                packet.payload.empty()) {

                return false;
            }

            const std::uint64_t fragment_end =
                static_cast<std::uint64_t>(packet.fragment_offset) +
                packet.payload.size();

            if (fragment_end > packet.original_length) {
                return false;
            }
        } else if (
            packet.type == PacketType::ping or
            packet.type == PacketType::pong) {

            if (
                packet.message_id == 0 or
                packet.fragment_offset != 0 or
                packet.original_length != 0 or
                not packet.payload.empty()) {

                return false;
            }
        } else {
            if (
                packet.message_id != 0 or
                packet.fragment_offset != 0 or
                packet.original_length != 0 or
                not packet.payload.empty()) {

                return false;
            }
        }

        return true;
    }

private:
    std::uint16_t tunnel_id_ = 0;
    ascon::key_type node_key_ {};
};

class TunDevice {
public:
    TunDevice(
        const std::string& interface_name,
        std::size_t mtu)
        : interface_name_(interface_name) {

        fd_ = ::open("/dev/net/tun", O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            throw std::runtime_error(
                "Cannot open /dev/net/tun: " +
                std::string(std::strerror(errno)));
        }

        ifreq request {};
        request.ifr_flags = IFF_TUN | IFF_NO_PI;
        std::strncpy(
            request.ifr_name,
            interface_name.c_str(),
            IFNAMSIZ - 1);

        if (::ioctl(fd_, TUNSETIFF, &request) < 0) {
            const std::string error = std::strerror(errno);
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("TUNSETIFF failed: " + error);
        }

        interface_name_ = request.ifr_name;
        set_mtu(mtu);
    }

    ~TunDevice() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    int fd() const {
        return fd_;
    }

    ssize_t read_packet(std::uint8_t* buffer, std::size_t size) {
        return ::read(fd_, buffer, size);
    }

    ssize_t write_packet(const std::uint8_t* buffer, std::size_t size) {
        return ::write(fd_, buffer, size);
    }

private:
    void set_mtu(std::size_t mtu) {
        const int socket_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (socket_fd < 0) {
            throw std::runtime_error(
                "Cannot create MTU ioctl socket: " +
                std::string(std::strerror(errno)));
        }

        ifreq request {};
        std::strncpy(
            request.ifr_name,
            interface_name_.c_str(),
            IFNAMSIZ - 1);
        request.ifr_mtu = static_cast<int>(mtu);

        if (::ioctl(socket_fd, SIOCSIFMTU, &request) < 0) {
            const std::string error = std::strerror(errno);
            ::close(socket_fd);
            throw std::runtime_error("SIOCSIFMTU failed: " + error);
        }

        ::close(socket_fd);
    }

    int fd_ = -1;
    std::string interface_name_;
};

class UdpEndpoint {
public:
    ~UdpEndpoint() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    void open_server(std::uint16_t port) {
        fd_ = ::socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            throw std::runtime_error(
                "socket() failed: " +
                std::string(std::strerror(errno)));
        }

        int v6_only = 0;
        ::setsockopt(
            fd_,
            IPPROTO_IPV6,
            IPV6_V6ONLY,
            &v6_only,
            sizeof(v6_only));

        sockaddr_in6 address {};
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_any;
        address.sin6_port = htons(port);

        if (
            ::bind(
                fd_,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) < 0) {

            throw std::runtime_error(
                "bind() failed: " +
                std::string(std::strerror(errno)));
        }

        // A dual-stack server socket is treated conservatively as IPv6 for
        // transport-MTU calculations. IPv4-mapped peers therefore merely
        // get 20 bytes of extra safety margin.
        outer_ip_header_size_ = ipv6_header_size;
    }

    void open_client(const std::string& host, std::uint16_t port) {
        addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;

        addrinfo* result = nullptr;
        const std::string service = std::to_string(port);

        const int rc =
            ::getaddrinfo(
                host.c_str(),
                service.c_str(),
                &hints,
                &result);

        if (rc != 0) {
            throw std::runtime_error(
                "getaddrinfo() failed: " +
                std::string(gai_strerror(rc)));
        }

        for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
            const int candidate =
                ::socket(
                    item->ai_family,
                    SOCK_DGRAM | SOCK_CLOEXEC,
                    0);

            if (candidate < 0) {
                continue;
            }

            fd_ = candidate;
            std::memset(&peer_, 0, sizeof(peer_));
            std::memcpy(&peer_, item->ai_addr, item->ai_addrlen);
            peer_length_ = static_cast<socklen_t>(item->ai_addrlen);
            peer_valid_ = true;

            outer_ip_header_size_ =
                item->ai_family == AF_INET
                    ? ipv4_header_min_size
                    : ipv6_header_size;

            break;
        }

        ::freeaddrinfo(result);

        if (fd_ < 0 or not peer_valid_) {
            throw std::runtime_error("Cannot create UDP client socket");
        }
    }

    int fd() const {
        return fd_;
    }

    ssize_t receive(
        std::uint8_t* buffer,
        std::size_t size,
        sockaddr_storage& source,
        socklen_t& source_length) {

        source = {};
        source_length = sizeof(source);

        return ::recvfrom(
            fd_,
            buffer,
            size,
            0,
            reinterpret_cast<sockaddr*>(&source),
            &source_length);
    }

    void set_peer(
        const sockaddr_storage& peer,
        socklen_t peer_length) {

        peer_ = peer;
        peer_length_ = peer_length;
        peer_valid_ = true;
    }

    ssize_t send(const std::uint8_t* buffer, std::size_t size) {
        if (not peer_valid_) {
            return 0;
        }

        return ::sendto(
            fd_,
            buffer,
            size,
            0,
            reinterpret_cast<const sockaddr*>(&peer_),
            peer_length_);
    }

    std::size_t outer_ip_header_size() const {
        return outer_ip_header_size_;
    }

private:
    int fd_ = -1;
    sockaddr_storage peer_ {};
    socklen_t peer_length_ = 0;
    bool peer_valid_ = false;
    std::size_t outer_ip_header_size_ = ipv6_header_size;
};

struct FragmentRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

struct ReassemblyEntry {
    std::uint32_t original_length = 0;
    std::vector<std::uint8_t> buffer;
    std::vector<FragmentRange> ranges;
    std::size_t received_bytes = 0;
    std::chrono::steady_clock::time_point last_update {};
};

class Reassembler {
public:
    explicit Reassembler(std::size_t maximum_packet_size)
        : maximum_packet_size_(maximum_packet_size) {
    }

    bool accept(
        const Packet& fragment,
        std::vector<std::uint8_t>& complete_packet) {

        complete_packet.clear();

        if (
            fragment.message_id == 0 or
            fragment.original_length == 0 or
            fragment.original_length > maximum_packet_size_ or
            fragment.payload.empty()) {

            return false;
        }

        const std::uint64_t end64 =
            static_cast<std::uint64_t>(fragment.fragment_offset) +
            fragment.payload.size();

        if (end64 > fragment.original_length) {
            return false;
        }

        const std::uint32_t begin = fragment.fragment_offset;
        const std::uint32_t end = static_cast<std::uint32_t>(end64);

        if (
            begin == 0 and
            end == fragment.original_length) {

            complete_packet = fragment.payload;
            return true;
        }

        cleanup_expired();

        auto iterator = entries_.find(fragment.message_id);

        if (iterator == entries_.end()) {
            if (
                entries_.size() >= max_reassembly_entries or
                total_bytes_ + fragment.original_length >
                    max_reassembly_bytes) {

                log_info("DROP reassembly capacity exceeded");
                return false;
            }

            ReassemblyEntry entry;
            entry.original_length = fragment.original_length;
            entry.buffer.resize(fragment.original_length);
            entry.last_update = std::chrono::steady_clock::now();

            total_bytes_ += entry.buffer.size();

            iterator =
                entries_
                    .emplace(
                        fragment.message_id,
                        std::move(entry))
                    .first;
        }

        ReassemblyEntry& entry = iterator->second;

        if (entry.original_length != fragment.original_length) {
            erase(iterator);
            log_info("DROP inconsistent reassembly length");
            return false;
        }

        if (entry.ranges.size() >= max_fragments_per_packet) {
            erase(iterator);
            log_info("DROP too many fragments");
            return false;
        }

        for (const FragmentRange& range : entry.ranges) {
            if (begin < range.end and end > range.begin) {
                log_info("DROP duplicate/overlapping fragment");
                return false;
            }
        }

        std::memcpy(
            entry.buffer.data() + begin,
            fragment.payload.data(),
            fragment.payload.size());

        entry.ranges.push_back({begin, end});
        entry.received_bytes += fragment.payload.size();
        entry.last_update = std::chrono::steady_clock::now();

        if (entry.received_bytes != entry.original_length) {
            return false;
        }

        complete_packet = std::move(entry.buffer);
        total_bytes_ -= entry.original_length;
        entries_.erase(iterator);
        return true;
    }

    void cleanup_expired() {
        const auto now = std::chrono::steady_clock::now();

        for (auto iterator = entries_.begin(); iterator != entries_.end();) {
            if (
                now - iterator->second.last_update >=
                std::chrono::seconds(reassembly_timeout_seconds)) {

                total_bytes_ -= iterator->second.buffer.size();
                iterator = entries_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

private:
    using entry_iterator =
        std::unordered_map<std::uint64_t, ReassemblyEntry>::iterator;

    void erase(entry_iterator iterator) {
        total_bytes_ -= iterator->second.buffer.size();
        entries_.erase(iterator);
    }

    std::size_t maximum_packet_size_ = default_tun_mtu;
    std::unordered_map<std::uint64_t, ReassemblyEntry> entries_;
    std::size_t total_bytes_ = 0;
};

std::uint16_t ipv4_header_checksum(
    const std::uint8_t* data,
    std::size_t size) {

    std::uint32_t sum = 0;

    for (std::size_t i = 0; i + 1 < size; i += 2) {
        sum +=
            static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(data[i]) << 8U) |
                data[i + 1]);

        while ((sum >> 16U) != 0) {
            sum = (sum & 0xffffU) + (sum >> 16U);
        }
    }

    if ((size & 1U) != 0) {
        sum += static_cast<std::uint16_t>(data[size - 1] << 8U);
    }

    while ((sum >> 16U) != 0) {
        sum = (sum & 0xffffU) + (sum >> 16U);
    }

    return static_cast<std::uint16_t>(~sum);
}

bool compensate_ipv4_ttl(
    std::uint8_t* packet,
    std::size_t size) {

    if (size < ipv4_header_min_size) {
        return false;
    }

    const std::size_t header_size =
        static_cast<std::size_t>(packet[0] & 0x0fU) * 4;

    if (
        header_size < ipv4_header_min_size or
        header_size > size) {

        return false;
    }

    if (packet[8] == 255) {
        return true;
    }

    ++packet[8];

    // Recompute the IPv4 header checksum after the TTL change.
    packet[10] = 0;
    packet[11] = 0;

    const std::uint16_t checksum =
        ipv4_header_checksum(packet, header_size);

    store_be16(packet + 10, checksum);
    return true;
}

bool compensate_ipv6_hop_limit(
    std::uint8_t* packet,
    std::size_t size) {

    if (size < ipv6_header_size) {
        return false;
    }

    if (packet[7] < 255) {
        ++packet[7];
    }

    return true;
}

void compensate_ip_hop(
    std::vector<std::uint8_t>& packet) {

    if (packet.empty()) {
        return;
    }

    const std::uint8_t version =
        static_cast<std::uint8_t>(packet[0] >> 4U);

    if (version == 4) {
        if (not compensate_ipv4_ttl(packet.data(), packet.size())) {
            log_info("TTL compensation skipped: invalid IPv4 header");
        }
    } else if (version == 6) {
        if (
            not compensate_ipv6_hop_limit(
                packet.data(),
                packet.size())) {

            log_info(
                "Hop-limit compensation skipped: invalid IPv6 header");
        }
    }
}

struct ProbeState {
    std::chrono::steady_clock::time_point sent_at;
};

struct FragmentPlan {
    std::size_t count = 0;
    std::size_t base_size = 0;
    std::size_t larger_fragments = 0;
};

FragmentPlan make_fragment_plan(
    std::size_t packet_size,
    std::size_t maximum_fragment_payload) {

    if (packet_size == 0 or maximum_fragment_payload == 0) {
        throw std::runtime_error("Invalid fragmentation parameters");
    }

    const std::size_t count =
        (packet_size + maximum_fragment_payload - 1) /
        maximum_fragment_payload;

    if (count > max_fragments_per_packet) {
        throw std::runtime_error(
            "Packet requires too many transport fragments");
    }

    const std::size_t base_size = packet_size / count;
    const std::size_t remainder = packet_size % count;

    return {
        count,
        base_size,
        remainder,
    };
}

class Tunnel {
public:
    Tunnel(
        std::uint16_t tunnel_id,
        bool server_mode,
        const std::string& interface_name,
        const std::string& remote_host,
        const Options& options)
        : tunnel_id_(tunnel_id),
          server_mode_(server_mode),
          options_(options),
          tun_(interface_name, options.tun_mtu),
          master_key_(parse_master_key()),
          protocol_v2_(tunnel_id, master_key_),
          protocol_v3_(tunnel_id, master_key_),
          reassembler_(options.tun_mtu) {

        const std::uint16_t port =
            static_cast<std::uint16_t>(40000 + tunnel_id_);

        if (server_mode_) {
            udp_.open_server(port);
        } else {
            udp_.open_client(remote_host, port);
        }

        validate_fragment_capacity();

        drop_privileges();

        if (log_enabled(LogLevel::info)) {
            std::cerr
                << "tuntom id=" << tunnel_id_
                << " tun-mtu=" << options_.tun_mtu
                << " transport-mtu=" << options_.transport_mtu
                << " max-fragment-payload="
                << maximum_fragment_payload()
                << " ttl-compensate="
                << (options_.ttl_compensate ? "yes" : "no")
                << " stats="
                << (options_.stats_file.empty() ? "off" : options_.stats_file)
                << "\n";
        }
    }

    virtual ~Tunnel() = default;

    void run() {
        const auto started_now =
            std::chrono::steady_clock::now();

        if (not server_mode_) {
            send_control(PacketType::hello);
            send_rtt_probe();
            next_rtt_probe_ =
                started_now +
                std::chrono::seconds(rtt_probe_interval_seconds);
            rtt_probe_schedule_active_ = true;
        }

        auto last_keepalive = started_now;
        auto last_reassembly_cleanup =
            std::chrono::steady_clock::now();
        auto last_stats_write =
            std::chrono::steady_clock::now();

        write_stats();

        std::array<std::uint8_t, buffer_size> buffer {};

        while (true) {
            pollfd descriptors[2] {};
            descriptors[0].fd = tun_.fd();
            descriptors[0].events = POLLIN;
            descriptors[1].fd = udp_.fd();
            descriptors[1].events = POLLIN;

            const int rc = ::poll(descriptors, 2, 1000);

            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }

                throw std::runtime_error(
                    "poll() failed: " +
                    std::string(std::strerror(errno)));
            }

            if ((descriptors[0].revents & POLLIN) != 0) {
                const ssize_t received =
                    tun_.read_packet(
                        buffer.data(),
                        buffer.size());

                if (received > 0) {
                    const std::size_t packet_size =
                        static_cast<std::size_t>(received);

                    ++stats_.tun_rx_packets;
                    stats_.tun_rx_bytes += packet_size;

                    dump_bytes(
                        "TUN read",
                        buffer.data(),
                        packet_size,
                        20);

                    if (packet_size > options_.tun_mtu) {
                        ++stats_.drops_mtu;
                        log_info("DROP TUN packet larger than configured MTU");
                    } else {
                        send_data(
                            buffer.data(),
                            packet_size);
                    }
                }
            }

            if ((descriptors[1].revents & POLLIN) != 0) {
                sockaddr_storage source {};
                socklen_t source_length = 0;

                const ssize_t received =
                    udp_.receive(
                        buffer.data(),
                        buffer.size(),
                        source,
                        source_length);

                if (received > 0) {
                    ++stats_.udp_rx_packets;
                    stats_.udp_rx_bytes +=
                        static_cast<std::uint64_t>(received);

                    handle_udp_packet(
                        buffer.data(),
                        static_cast<std::size_t>(received),
                        source,
                        source_length);
                }
            }

            const auto now = std::chrono::steady_clock::now();

            if (
                not server_mode_ and
                now - last_keepalive >=
                    std::chrono::seconds(keepalive_seconds)) {

                send_control(PacketType::keepalive);
                last_keepalive = now;
            }

            if (
                rtt_probe_schedule_active_ and
                now >= next_rtt_probe_) {

                send_rtt_probe();
                next_rtt_probe_ +=
                    std::chrono::seconds(
                        rtt_probe_interval_seconds);
            }

            cleanup_rtt_probes(now);

            if (
                now - last_reassembly_cleanup >=
                std::chrono::seconds(1)) {

                reassembler_.cleanup_expired();
                last_reassembly_cleanup = now;
            }

            if (
                now - last_stats_write >=
                    std::chrono::seconds(1)) {

                write_stats();
                last_stats_write = now;
            }
        }
    }

protected:
    virtual bool process(Packet&, Direction) {
        // Future packet processing hooks (e.g. smithproxy integration)
        // can override this.
        return true;
    }

private:
    std::size_t maximum_fragment_payload() const {
        const std::size_t overhead =
            udp_.outer_ip_header_size() +
            udp_header_size +
            protocol_header_v3_size;

        if (options_.transport_mtu <= overhead) {
            throw std::runtime_error(
                "Transport MTU is too small for tuntom V3");
        }

        return options_.transport_mtu - overhead;
    }

    void validate_fragment_capacity() const {
        const std::size_t maximum_payload =
            maximum_fragment_payload();

        const std::size_t fragment_count =
            (options_.tun_mtu + maximum_payload - 1) /
            maximum_payload;

        if (fragment_count > max_fragments_per_packet) {
            throw std::runtime_error(
                "Configured MTU/transport-MTU combination may require "
                "more than 64 fragments");
        }
    }

    void send_data(
        const std::uint8_t* data,
        std::size_t size) {

        Packet logical_packet;
        logical_packet.type = PacketType::data;
        logical_packet.tunnel_id = tunnel_id_;
        logical_packet.protocol_version = protocol_version_v3;
        logical_packet.payload.assign(data, data + size);

        if (not process(logical_packet, Direction::tun_to_udp)) {
            ++stats_.drops_process;
            return;
        }

        if (logical_packet.payload.size() > options_.tun_mtu) {
            ++stats_.drops_mtu;
            log_info("DROP processed packet larger than configured MTU");
            return;
        }

        ++stats_.data_tx_packets;

        const std::size_t maximum_payload =
            maximum_fragment_payload();

        const FragmentPlan plan =
            make_fragment_plan(
                logical_packet.payload.size(),
                maximum_payload);

        const std::uint64_t message_id =
            message_id_generator_.next();

        std::size_t offset = 0;

        for (std::size_t index = 0; index < plan.count; ++index) {
            const std::size_t fragment_size =
                plan.base_size +
                (index < plan.larger_fragments ? 1 : 0);

            Packet fragment;
            fragment.type = PacketType::data;
            fragment.tunnel_id = tunnel_id_;
            fragment.protocol_version = protocol_version_v3;
            fragment.sequence = sequence_generator_.next();
            fragment.message_id = message_id;
            fragment.fragment_offset =
                static_cast<std::uint32_t>(offset);
            fragment.original_length =
                static_cast<std::uint32_t>(
                    logical_packet.payload.size());

            fragment.payload.assign(
                logical_packet.payload.begin() +
                    static_cast<std::ptrdiff_t>(offset),
                logical_packet.payload.begin() +
                    static_cast<std::ptrdiff_t>(
                        offset + fragment_size));

            const auto encoded =
                protocol_v3_.encode(fragment);

            const ssize_t sent =
                udp_.send(
                    encoded.data(),
                    encoded.size());

            if (sent < 0) {
                ++stats_.udp_send_errors;
                if (log_enabled(LogLevel::info)) {
                    std::cerr
                        << "UDP send failed: "
                        << std::strerror(errno)
                        << "\n";
                }
                return;
            }

            ++stats_.udp_tx_packets;
            stats_.udp_tx_bytes +=
                static_cast<std::uint64_t>(sent);
            ++stats_.fragments_tx;

            if (log_enabled(LogLevel::debug)) {
                std::cerr
                    << "FRAGMENT "
                    << (index + 1) << "/" << plan.count
                    << " msg=" << message_id
                    << " offset=" << offset
                    << " size=" << fragment_size
                    << "\n";
            }

            offset += fragment_size;
        }
    }

    void handle_udp_packet(
        const std::uint8_t* data,
        std::size_t size,
        const sockaddr_storage& source,
        socklen_t source_length) {

        dump_bytes("UDP recv", data, size, 40);

        Packet packet;
        const std::uint8_t version =
            size > 6 ? data[6] : 0;

        bool decoded = false;

        if (version == protocol_version_v3) {
            decoded =
                protocol_v3_.decode(
                    data,
                    size,
                    packet);
        } else if (
            version == protocol_version_v2 and
            options_.allow_v2) {

            decoded =
                protocol_v2_.decode(
                    data,
                    size,
                    packet);
        } else if (
            version == protocol_version_v1 and
            options_.allow_v1) {

            decoded =
                protocol_v1_.decode(
                    data,
                    size,
                    packet);
        } else {
            ++stats_.drops_protocol;
            if (log_enabled(LogLevel::info)) {
                std::cerr
                    << "DROP protocol version "
                    << static_cast<unsigned>(version)
                    << " not allowed\n";
            }
            return;
        }

        if (not decoded) {
            ++stats_.drops_protocol;
            log_info("DROP invalid/auth-failed protocol packet");
            return;
        }

        if (packet.tunnel_id != tunnel_id_) {
            ++stats_.drops_tunnel_id;
            log_info("DROP tunnel id mismatch");
            return;
        }

        if (packet.protocol_version >= protocol_version_v2) {
            if (not replay_window_.accept(packet.sequence)) {
                ++stats_.drops_replay;
                if (log_enabled(LogLevel::info)) {
                    std::cerr
                        << "DROP replay/old seq="
                        << packet.sequence
                        << "\n";
                }
                return;
            }
        }

        // Server learns/updates the NAT peer only after successful
        // authentication (or accepted V1 when explicitly enabled).
        if (server_mode_) {
            udp_.set_peer(source, source_length);
        }

        if (log_enabled(LogLevel::debug)) {
            std::cerr
                << "ACCEPT v"
                << static_cast<unsigned>(packet.protocol_version)
                << " seq=" << packet.sequence
                << " type=" << static_cast<unsigned>(packet.type)
                << " msg=" << packet.message_id
                << " offset=" << packet.fragment_offset
                << " original=" << packet.original_length
                << " payload=" << packet.payload.size()
                << "\n";
        }

        if (packet.type == PacketType::ping) {
            if (
                server_mode_ and
                not rtt_probe_schedule_active_) {

                const auto now =
                    std::chrono::steady_clock::now();

                next_rtt_probe_ =
                    now +
                    std::chrono::milliseconds(
                        (rtt_probe_interval_seconds * 1000) / 2);

                rtt_probe_schedule_active_ = true;
            }

            send_probe_reply(packet.message_id);
            return;
        }

        if (packet.type == PacketType::pong) {
            handle_probe_reply(packet.message_id);
            return;
        }

        if (packet.type != PacketType::data) {
            return;
        }

        if (packet.protocol_version == protocol_version_v3) {
            ++stats_.fragments_rx;

            std::vector<std::uint8_t> complete_packet;

            if (
                not reassembler_.accept(
                    packet,
                    complete_packet)) {

                return;
            }

            Packet logical_packet;
            logical_packet.type = PacketType::data;
            logical_packet.tunnel_id = tunnel_id_;
            logical_packet.protocol_version = protocol_version_v3;
            logical_packet.sequence = packet.sequence;
            logical_packet.message_id = packet.message_id;
            logical_packet.original_length =
                static_cast<std::uint32_t>(
                    complete_packet.size());
            logical_packet.payload =
                std::move(complete_packet);

            deliver_to_tun(logical_packet);
            return;
        }

        // Legacy V1/V2 packets are unfragmented.
        deliver_to_tun(packet);
    }

    void deliver_to_tun(Packet& packet) {
        if (not process(packet, Direction::udp_to_tun)) {
            ++stats_.drops_process;
            return;
        }

        if (packet.payload.size() > options_.tun_mtu) {
            ++stats_.drops_mtu;
            log_info("DROP reassembled packet larger than configured MTU");
            return;
        }

        if (options_.ttl_compensate) {
            compensate_ip_hop(packet.payload);
        }

        dump_bytes(
            "TUN write",
            packet.payload.data(),
            packet.payload.size(),
            20);

        const ssize_t written =
            tun_.write_packet(
                packet.payload.data(),
                packet.payload.size());

        if (written < 0) {
            ++stats_.tun_write_errors;

            if (log_enabled(LogLevel::info)) {
                std::cerr
                    << "TUN write failed: "
                    << std::strerror(errno)
                    << "\n";
            }
        } else {
            ++stats_.tun_tx_packets;
            stats_.tun_tx_bytes +=
                static_cast<std::uint64_t>(written);
            ++stats_.data_rx_packets;
        }
    }

    bool send_control_packet(
        PacketType type,
        std::uint64_t message_id = 0) {

        Packet packet;
        packet.type = type;
        packet.tunnel_id = tunnel_id_;
        packet.protocol_version = protocol_version_v3;
        packet.sequence = sequence_generator_.next();
        packet.message_id = message_id;

        const auto encoded =
            protocol_v3_.encode(packet);

        const ssize_t sent =
            udp_.send(
                encoded.data(),
                encoded.size());

        if (sent < 0) {
            ++stats_.udp_send_errors;
            return false;
        }

        ++stats_.udp_tx_packets;
        stats_.udp_tx_bytes +=
            static_cast<std::uint64_t>(sent);

        return true;
    }

    void send_control(PacketType type) {
        send_control_packet(type);
    }

    void send_rtt_probe() {
        const std::uint64_t probe_id =
            message_id_generator_.next();

        const auto sent_at =
            std::chrono::steady_clock::now();

        if (send_control_packet(PacketType::ping, probe_id)) {
            rtt_probes_[probe_id] = ProbeState { sent_at };
        }
    }

    void send_probe_reply(std::uint64_t probe_id) {
        if (probe_id == 0) {
            return;
        }

        send_control_packet(PacketType::pong, probe_id);
    }

    void handle_probe_reply(std::uint64_t probe_id) {
        const auto it =
            rtt_probes_.find(probe_id);

        if (it == rtt_probes_.end()) {
            return;
        }

        const auto now =
            std::chrono::steady_clock::now();

        const double rtt_ms =
            std::chrono::duration<double, std::milli>(
                now - it->second.sent_at).count();

        rtt_probes_.erase(it);

        stats_.rtt_last_ms = rtt_ms;

        if (stats_.rtt_samples == 0) {
            stats_.rtt_min_ms = rtt_ms;
            stats_.rtt_max_ms = rtt_ms;
            stats_.rtt_avg_ms = rtt_ms;
            stats_.rtt_jitter_ms = 0.0;
        } else {
            stats_.rtt_min_ms =
                std::min(stats_.rtt_min_ms, rtt_ms);
            stats_.rtt_max_ms =
                std::max(stats_.rtt_max_ms, rtt_ms);

            const double previous_average =
                stats_.rtt_avg_ms;

            stats_.rtt_avg_ms +=
                (rtt_ms - stats_.rtt_avg_ms) /
                static_cast<double>(stats_.rtt_samples + 1);

            const double deviation =
                std::abs(rtt_ms - previous_average);

            stats_.rtt_jitter_ms +=
                (deviation - stats_.rtt_jitter_ms) / 16.0;
        }

        ++stats_.rtt_samples;
    }

    void cleanup_rtt_probes(
        const std::chrono::steady_clock::time_point& now) {

        for (auto it = rtt_probes_.begin();
             it != rtt_probes_.end();) {

            if (
                now - it->second.sent_at >=
                    std::chrono::seconds(
                        rtt_probe_timeout_seconds)) {

                ++stats_.rtt_lost;
                it = rtt_probes_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void write_stats() {
        if (options_.stats_file.empty()) {
            return;
        }

        const auto now_steady = std::chrono::steady_clock::now();
        const auto uptime =
            std::chrono::duration_cast<std::chrono::seconds>(
                now_steady - started_at_).count();

        const auto now_system =
            std::chrono::system_clock::now();
        const auto updated_unix =
            std::chrono::duration_cast<std::chrono::seconds>(
                now_system.time_since_epoch()).count();

        const std::string temporary_file =
            options_.stats_file +
            ".tmp." +
            std::to_string(static_cast<long long>(::getpid()));

        {
            std::ofstream output(
                temporary_file,
                std::ios::out | std::ios::trunc);

            if (not output) {
                log_info("Unable to open stats file " + temporary_file);
                return;
            }

            output
                << "format=txt\n"
                << "format_version=1\n"
                << "pid=" << ::getpid() << "\n"
                << "tunnel_id=" << tunnel_id_ << "\n"
                << "mode=" << (server_mode_ ? "server" : "client") << "\n"
                << "updated_unix=" << updated_unix << "\n"
                << "uptime_seconds=" << uptime << "\n"
                << "tun_rx_packets=" << stats_.tun_rx_packets << "\n"
                << "tun_rx_bytes=" << stats_.tun_rx_bytes << "\n"
                << "tun_tx_packets=" << stats_.tun_tx_packets << "\n"
                << "tun_tx_bytes=" << stats_.tun_tx_bytes << "\n"
                << "udp_rx_packets=" << stats_.udp_rx_packets << "\n"
                << "udp_rx_bytes=" << stats_.udp_rx_bytes << "\n"
                << "udp_tx_packets=" << stats_.udp_tx_packets << "\n"
                << "udp_tx_bytes=" << stats_.udp_tx_bytes << "\n"
                << "data_tx_packets=" << stats_.data_tx_packets << "\n"
                << "data_rx_packets=" << stats_.data_rx_packets << "\n"
                << "fragments_tx=" << stats_.fragments_tx << "\n"
                << "fragments_rx=" << stats_.fragments_rx << "\n"
                << "drops_protocol=" << stats_.drops_protocol << "\n"
                << "drops_tunnel_id=" << stats_.drops_tunnel_id << "\n"
                << "drops_replay=" << stats_.drops_replay << "\n"
                << "drops_mtu=" << stats_.drops_mtu << "\n"
                << "drops_process=" << stats_.drops_process << "\n"
                << "udp_send_errors=" << stats_.udp_send_errors << "\n"
                << "tun_write_errors=" << stats_.tun_write_errors << "\n"
                << std::fixed << std::setprecision(3)
                << "rtt_last_ms=" << stats_.rtt_last_ms << "\n"
                << "rtt_min_ms=" << stats_.rtt_min_ms << "\n"
                << "rtt_max_ms=" << stats_.rtt_max_ms << "\n"
                << "rtt_avg_ms=" << stats_.rtt_avg_ms << "\n"
                << "rtt_jitter_ms=" << stats_.rtt_jitter_ms << "\n"
                << std::defaultfloat
                << "rtt_samples=" << stats_.rtt_samples << "\n"
                << "rtt_lost=" << stats_.rtt_lost << "\n";

            output.flush();

            if (not output) {
                log_info("Unable to write stats file " + temporary_file);
                return;
            }
        }

        if (
            std::rename(
                temporary_file.c_str(),
                options_.stats_file.c_str()) != 0) {

            log_info(
                "Unable to publish stats file " +
                options_.stats_file +
                ": " +
                std::strerror(errno));
            std::remove(temporary_file.c_str());
        }
    }

    std::uint16_t tunnel_id_ = 0;
    bool server_mode_ = false;
    Options options_;

    TunDevice tun_;
    UdpEndpoint udp_;

    const ascon::key_type master_key_;
    ProtocolV1 protocol_v1_;
    ProtocolV2 protocol_v2_;
    ProtocolV3 protocol_v3_;

    SequenceGenerator sequence_generator_;
    SequenceGenerator message_id_generator_;
    ReplayWindow replay_window_;
    Reassembler reassembler_;

    std::unordered_map<std::uint64_t, ProbeState> rtt_probes_;
    bool rtt_probe_schedule_active_ = false;
    std::chrono::steady_clock::time_point next_rtt_probe_ {};
    Stats stats_;
    const std::chrono::steady_clock::time_point started_at_ =
        std::chrono::steady_clock::now();
};

void usage(const char* program_name) {
    std::cerr
        << "Usage:\n"
        << "  " << program_name
        << " server <id> <ifname> [options]\n"
        << "  " << program_name
        << " client <id> <ifname> <host> [options]\n"
        << "\n"
        << "Transport options:\n"
        << "  --mtu <n>             TUN/inner MTU (default 1500)\n"
        << "  --transport-mtu <n>   Maximum outer IP packet MTU "
           "(default 1400)\n"
        << "  --no-ttl-compensate   Do not compensate the extra "
           "tuntom routing hop\n"
        << "\n"
        << "Statistics:\n"
        << "  --stats-file <path>    Export runtime statistics to file\n"
        << "  --stats-format <fmt>   Statistics format; currently: txt\n"
        << "\n"
        << "Compatibility:\n"
        << "  --allow-v2            Accept legacy authenticated V2 "
           "packets\n"
        << "  --allow-v1            Accept legacy unauthenticated V1 "
           "packets\n"
        << "\n"
        << "Logging:\n"
        << "  default                informational drops/errors only\n"
        << "  --debug                packet/fragment protocol details\n"
        << "  --quiet                suppress non-fatal logging\n"
        << "\n"
        << "Environment:\n"
        << "  TUNTOM_SECRET          32 hex characters "
           "(128-bit master key)\n";
}

std::size_t parse_size_option(
    const std::string& option,
    const char* value,
    std::size_t minimum,
    std::size_t maximum) {

    std::size_t parsed_characters = 0;
    const unsigned long parsed =
        std::stoul(
            value,
            &parsed_characters,
            10);

    if (
        value[parsed_characters] != '\0' or
        parsed < minimum or
        parsed > maximum) {

        throw std::runtime_error(
            option + " must be in range " +
            std::to_string(minimum) +
            ".." +
            std::to_string(maximum));
    }

    return static_cast<std::size_t>(parsed);
}

void parse_options(
    int argc,
    char** argv,
    int first_option,
    Options& options) {

    for (int i = first_option; i < argc; ++i) {
        const std::string option = argv[i];

        if (option == "--allow-v1") {
            options.allow_v1 = true;
        } else if (option == "--allow-v2") {
            options.allow_v2 = true;
        } else if (option == "--debug") {
            log_level = LogLevel::debug;
        } else if (option == "--quiet") {
            log_level = LogLevel::quiet;
        } else if (option == "--no-ttl-compensate") {
            options.ttl_compensate = false;
        } else if (option == "--ttl-compensate") {
            options.ttl_compensate = true;
        } else if (option == "--mtu") {
            if (++i >= argc) {
                throw std::runtime_error("--mtu requires a value");
            }

            options.tun_mtu =
                parse_size_option(
                    "--mtu",
                    argv[i],
                    576,
                    max_ip_packet_size);
        } else if (option == "--transport-mtu") {
            if (++i >= argc) {
                throw std::runtime_error(
                    "--transport-mtu requires a value");
            }

            options.transport_mtu =
                parse_size_option(
                    "--transport-mtu",
                    argv[i],
                    min_transport_mtu,
                    max_ip_packet_size);
        } else if (option == "--stats-file") {
            if (++i >= argc) {
                throw std::runtime_error(
                    "--stats-file requires a value");
            }

            options.stats_file = argv[i];

            if (options.stats_file.empty()) {
                throw std::runtime_error(
                    "--stats-file must not be empty");
            }
        } else if (option == "--stats-format") {
            if (++i >= argc) {
                throw std::runtime_error(
                    "--stats-format requires a value");
            }

            const std::string format = argv[i];

            if (format == "txt") {
                options.stats_format = StatsFormat::txt;
            } else {
                throw std::runtime_error(
                    "Unsupported stats format: " + format +
                    " (currently supported: txt)");
            }
        } else {
            throw std::runtime_error(
                "Unknown option: " + option);
        }
    }
}

std::uint16_t parse_tunnel_id(const char* value) {
    const unsigned long parsed = std::stoul(value);

    if (parsed == 0 or parsed > 255) {
        throw std::runtime_error(
            "Tunnel id must be in range 1..255");
    }

    return static_cast<std::uint16_t>(parsed);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 4) {
            usage(argv[0]);
            return 1;
        }

        const std::string mode = argv[1];
        const std::uint16_t tunnel_id =
            parse_tunnel_id(argv[2]);
        const std::string interface_name = argv[3];

        if (mode == "server") {
            Options options;
            parse_options(
                argc,
                argv,
                4,
                options);

            Tunnel tunnel(
                tunnel_id,
                true,
                interface_name,
                "",
                options);

            tunnel.run();
            return 0;
        }

        if (mode == "client") {
            if (argc < 5) {
                usage(argv[0]);
                return 1;
            }

            Options options;
            parse_options(
                argc,
                argv,
                5,
                options);

            Tunnel tunnel(
                tunnel_id,
                false,
                interface_name,
                argv[4],
                options);

            tunnel.run();
            return 0;
        }

        usage(argv[0]);
        return 1;
    } catch (const std::exception& error) {
        std::cerr
            << "ERROR: "
            << error.what()
            << "\n";

        return 1;
    }
}
