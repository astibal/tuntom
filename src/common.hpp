#pragma once

#include "logging.hpp"
#include "protocol_constants.hpp"
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace tuntom {

inline constexpr const char* runtime_user = "tuntom";
inline constexpr const char* runtime_group = "tuntom";

struct PrivilegeConfig {
    std::string user = runtime_user;
    std::string group = runtime_group;
};

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
