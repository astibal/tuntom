#pragma once

#include "logging.hpp"
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace tuntom {

inline constexpr std::uint8_t protocol_version_v5 = 5;

inline constexpr std::size_t protocol_header_v5_size = 25;

inline constexpr std::size_t protocol_fragment_v5_size = 37;
inline constexpr std::size_t protocol_probe_v5_size = 35;
inline constexpr std::size_t protocol_handshake_v5_size = 34;

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

inline constexpr int keepalive_seconds = 20;
inline constexpr int rtt_probe_interval_seconds = 15;
inline constexpr int rtt_probe_timeout_seconds = 5;
inline constexpr int reassembly_timeout_seconds = 3;
inline constexpr std::size_t max_reassembly_entries = 512;
inline constexpr std::size_t max_reassembly_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t max_reassembly_discarded = 32768;
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

template<class... Parts>
inline void log_info(const Parts&... parts) noexcept {
    if (log_enabled(LogLevel::info)) (LogLine() << ... << parts);
}

template<class... Parts>
inline void log_debug(const Parts&... parts) noexcept {
    if (log_enabled(LogLevel::debug)) (LogLine() << ... << parts);
}

// CLI/configuration diagnostics remain synchronous before the writer starts.
// Runtime errors must use the same fail-open path even if thread creation failed.
inline void log_fatal(const char* message) {
    if (logger.attempted() or not logger.sink_available()) LogLine() << "ERROR: " << message;
    else std::cerr << "ERROR: " << message << "\n";
}

} // namespace tuntom
