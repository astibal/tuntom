#pragma once

#include "common.hpp"
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace tuntom {

enum class PacketType : std::uint8_t {
    hello = 1,
    keepalive = 2,
    data = 3,
    ping = 4,
    pong = 5,
    mtu_probe = 6,
    mtu_reply = 7,
    init = 8,
    response = 9,
    confirm = 10,
    confirm_ack = 11,
};

enum class Direction {
    tun_to_udp,
    udp_to_tun,
};

struct Packet {
    PacketType type = PacketType::data;
    std::uint16_t tunnel_id = 0;
    std::uint8_t protocol_version = protocol_version_v4;
    std::uint64_t sequence = 0;

    // V4 DATA uses fragment metadata. PING/PONG use message_id as probe_id.
    std::uint64_t message_id = 0;
    std::uint32_t fragment_offset = 0;
    std::uint32_t original_length = 0;

    std::vector<std::uint8_t> payload;
};

enum class StatsFormat {
    txt,
};

struct Options {
    bool encrypt_ascon = false;
    std::size_t init_window = 300;
    bool allow_v1 = false;
    bool allow_v2 = false;
    bool ttl_compensate = true;
    bool pmtud_auto = true;
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
    std::uint64_t init_timestamp_rejected = 0;
    std::uint64_t init_nonce_capacity_rejected = 0;
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

    std::uint64_t pmtud_probes_sent = 0;
    std::uint64_t pmtud_probes_ok = 0;
    std::uint64_t pmtud_probes_lost = 0;
};

inline void dump_bytes(
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

} // namespace tuntom
