#pragma once

#include "protocol_constants.hpp"
#include <cstdint>
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
    ipc = 12,
    info = 13,
    control = 14,
    control_challenge = 15,
    // V5 extension namespace (wire bit 4). Older V5 implementations reject
    // these packets because bit 4 used to be reserved.
    auth_challenge = 16,
    auth_response = 17,
    auth_ok = 18,
    auth_failed = 19,
    relocate = 20,
    config = 21,
    relocate_bind = 22,
    relocate_ok = 23,
};

enum class Direction {
    tun_to_udp,
    udp_to_tun,
};

struct Packet {
    PacketType type = PacketType::data;
    std::uint16_t tunnel_id = 0;
    std::uint8_t protocol_version = protocol_version_v5;
    std::uint64_t sequence = 0;

    // V5 fragmented DATA uses optional metadata. PING/PONG use message_id as probe_id.
    std::uint64_t message_id = 0;
    std::uint32_t fragment_offset = 0;
    std::uint32_t original_length = 0;

    std::vector<std::uint8_t> payload;
};

} // namespace tuntom
