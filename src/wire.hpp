#pragma once

#include "ascon.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace tuntom {

inline std::uint64_t load_be64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8U) | data[i];
    }
    return value;
}

inline void store_be64(std::uint8_t* data, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        data[i] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8U;
    }
}

inline void store_be16(std::uint8_t* data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    data[1] = static_cast<std::uint8_t>(value & 0xffU);
}

inline std::uint16_t load_be16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8U) |
        data[1]);
}

inline std::uint32_t load_be32(const std::uint8_t* data) {
    return
        (static_cast<std::uint32_t>(data[0]) << 24U) |
        (static_cast<std::uint32_t>(data[1]) << 16U) |
        (static_cast<std::uint32_t>(data[2]) << 8U) |
        static_cast<std::uint32_t>(data[3]);
}

inline void store_be32(std::uint8_t* data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    data[1] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    data[2] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    data[3] = static_cast<std::uint8_t>(value & 0xffU);
}

inline ascon::key_type parse_master_key() {
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

} // namespace tuntom
