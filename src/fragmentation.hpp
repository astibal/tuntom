#pragma once

#include "common.hpp"
#include <chrono>
#include <cstddef>
#include <stdexcept>

namespace tuntom {

struct ProbeState {
    std::chrono::steady_clock::time_point sent_at;
};

struct FragmentPlan {
    std::size_t count = 0;
    std::size_t base_size = 0;
    std::size_t larger_fragments = 0;
};

inline FragmentPlan make_fragment_plan(
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

// Whole DATA has no fragment extension; choose the capacity before splitting.
inline FragmentPlan make_v5_fragment_plan(std::size_t packet_size,
                                          std::size_t fragment_payload) {
    constexpr auto extension = protocol_fragment_v5_size - protocol_header_v5_size;
    if (fragment_payload == 0 or fragment_payload > max_ip_packet_size or
        packet_size > max_ip_packet_size)
        throw std::runtime_error("Invalid V5 fragmentation parameters");
    return make_fragment_plan(packet_size,
        packet_size <= fragment_payload + extension ? fragment_payload + extension : fragment_payload);
}

} // namespace tuntom
