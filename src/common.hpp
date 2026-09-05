#pragma once

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace tuntom {

inline constexpr std::uint32_t protocol_magic = 0x5554554e; // "UTUN"
inline constexpr std::uint8_t protocol_version_v1 = 1;
inline constexpr std::uint8_t protocol_version_v2 = 2;
inline constexpr std::uint8_t protocol_version_v4 = 4;

inline constexpr std::size_t protocol_header_v1_size = 8;
inline constexpr std::size_t protocol_header_v2_size = 32;
inline constexpr std::size_t protocol_header_v4_size = 48;

inline constexpr std::size_t udp_header_size = 8;
inline constexpr std::size_t ipv4_header_min_size = 20;
inline constexpr std::size_t ipv6_header_size = 40;
inline constexpr std::size_t buffer_size = 65536;

inline constexpr std::size_t default_tun_mtu = 1500;
inline constexpr std::size_t default_transport_mtu = 1400;
inline constexpr std::size_t min_transport_mtu = 500;
inline constexpr std::size_t pmtud_upper_mtu = 1500;
inline constexpr int pmtud_probe_timeout_seconds = 2;
inline constexpr std::size_t max_ip_packet_size = 65535;

inline constexpr int keepalive_seconds = 5;
inline constexpr int rtt_probe_interval_seconds = 15;
inline constexpr int rtt_probe_timeout_seconds = 5;
inline constexpr int reassembly_timeout_seconds = 3;
inline constexpr std::size_t max_reassembly_entries = 64;
inline constexpr std::size_t max_reassembly_bytes = 4 * 1024 * 1024;
inline constexpr std::size_t max_fragments_per_packet = 64;

inline constexpr const char* runtime_user = "tuntom";
inline constexpr const char* runtime_group = "tuntom";

enum class LogLevel {
    quiet = 0,
    info = 1,
    debug = 2,
};

inline LogLevel log_level = LogLevel::info;

[[maybe_unused]] inline bool log_enabled(LogLevel level) {
    return static_cast<int>(log_level) >= static_cast<int>(level);
}

[[maybe_unused]] inline void log_info(const std::string& message) {
    if (log_enabled(LogLevel::info)) {
        std::cerr << message << "\n";
    }
}

[[maybe_unused]] inline void log_debug(const std::string& message) {
    if (log_enabled(LogLevel::debug)) {
        std::cerr << message << "\n";
    }
}

} // namespace tuntom
