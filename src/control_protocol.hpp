#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>

namespace tuntom {
constexpr std::size_t control_chunk_size = 16384;
constexpr std::size_t control_max_body = 1024 * 1024;
constexpr std::size_t control_max_flows = 256 * 1024 * 1024;
inline std::size_t control_length(const std::string &s, std::size_t maximum = control_max_body) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("invalid control length");
    std::size_t value = 0;
    for (char c : s) {
        const auto digit = static_cast<unsigned>(c - '0');
        if (digit > maximum || value > (maximum - digit) / 10)
            throw std::runtime_error("control body exceeds size limit");
        value = value * 10 + digit;
    }
    return value;
}
} // namespace tuntom
