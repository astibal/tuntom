#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>

namespace tuntom {
constexpr std::size_t control_chunk_size = 16384;
constexpr std::size_t control_max_body = 1024 * 1024;
inline std::size_t control_length(const std::string &s) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("invalid control length");
    std::size_t value = 0;
    for (char c : s) {
        value = value * 10 + static_cast<unsigned>(c - '0');
        if (value > control_max_body) throw std::runtime_error("control body exceeds 1 MiB");
    }
    return value;
}
} // namespace tuntom
