#pragma once

#include "common.hpp"
#include "wire.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tuntom {

inline std::uint16_t ipv4_header_checksum(
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

inline bool compensate_ipv4_ttl(
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

inline bool compensate_ipv6_hop_limit(
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

inline void compensate_ip_hop(
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

} // namespace tuntom
