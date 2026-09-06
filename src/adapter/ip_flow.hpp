#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tuntom {

struct IpPairKey {
    std::uint8_t version = 0;
    std::array<std::uint8_t, 16> source {};
    std::array<std::uint8_t, 16> destination {};

    bool operator==(const IpPairKey& other) const {
        return version == other.version and source == other.source and
               destination == other.destination;
    }
};

struct FlowKey : IpPairKey {
    std::uint8_t protocol = 0;
    std::uint16_t source_port = 0;
    std::uint16_t destination_port = 0;

    bool operator==(const FlowKey& other) const {
        return IpPairKey::operator==(other) and protocol == other.protocol and
               source_port == other.source_port and
               destination_port == other.destination_port;
    }
};

inline std::size_t hash_bytes(const std::uint8_t* data, std::size_t size) {
    std::size_t hash = 1469598103934665603ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct IpPairHash {
    std::size_t operator()(const IpPairKey& key) const {
        return hash_bytes(key.source.data(), key.source.size()) ^
               (hash_bytes(key.destination.data(), key.destination.size()) << 1) ^
               key.version;
    }
};

struct FlowHash {
    std::size_t operator()(const FlowKey& key) const {
        return IpPairHash {}(key) ^
               (static_cast<std::size_t>(key.protocol) << 8) ^
               (static_cast<std::size_t>(key.source_port) << 16) ^
               static_cast<std::size_t>(key.destination_port);
    }
};

struct ParsedIpFlow {
    IpPairKey l3;
    FlowKey l4;
    bool has_l4 = false;
};

inline std::uint16_t load_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

inline bool parse_ip_flow(
    const std::uint8_t* packet, std::size_t size, ParsedIpFlow& parsed) {

    if (size == 0) return false;
    parsed = {};
    const std::uint8_t version = packet[0] >> 4;
    std::size_t offset = 0;
    std::uint8_t protocol = 0;

    if (version == 4) {
        if (size < 20) return false;
        const std::size_t header_size = (packet[0] & 0x0fU) * 4U;
        const std::size_t total_size = load_u16(packet + 2);
        if (header_size < 20 or header_size > size or total_size < header_size or
            total_size > size) return false;
        parsed.l3.version = 4;
        std::memcpy(parsed.l3.source.data(), packet + 12, 4);
        std::memcpy(parsed.l3.destination.data(), packet + 16, 4);
        protocol = packet[9];
        offset = header_size;
        const std::uint16_t fragment = load_u16(packet + 6);
        if ((fragment & 0x1fffU) != 0) return true;
    } else if (version == 6) {
        if (size < 40) return false;
        const std::size_t total_size = 40U + load_u16(packet + 4);
        if (total_size > size) return false;
        parsed.l3.version = 6;
        std::memcpy(parsed.l3.source.data(), packet + 8, 16);
        std::memcpy(parsed.l3.destination.data(), packet + 24, 16);
        protocol = packet[6];
        offset = 40;
        for (std::size_t count = 0; count < 8; ++count) {
            if (protocol == 44) {
                if (offset + 8 > total_size) return false;
                const std::uint16_t fragment = load_u16(packet + offset + 2);
                protocol = packet[offset];
                offset += 8;
                if ((fragment & 0xfff8U) != 0) return true;
            } else if (protocol == 0 or protocol == 43 or protocol == 60) {
                if (offset + 2 > total_size) return false;
                const std::size_t length = (packet[offset + 1] + 1U) * 8U;
                if (offset + length > total_size) return false;
                protocol = packet[offset];
                offset += length;
            } else if (protocol == 51) {
                if (offset + 2 > total_size) return false;
                const std::size_t length = (packet[offset + 1] + 2U) * 4U;
                if (offset + length > total_size) return false;
                protocol = packet[offset];
                offset += length;
            } else {
                break;
            }
        }
    } else {
        return false;
    }

    parsed.l4.version = parsed.l3.version;
    parsed.l4.source = parsed.l3.source;
    parsed.l4.destination = parsed.l3.destination;
    parsed.l4.protocol = protocol;
    if ((protocol == 6 or protocol == 17) and offset + 4 <= size) {
        parsed.l4.source_port = load_u16(packet + offset);
        parsed.l4.destination_port = load_u16(packet + offset + 2);
        parsed.has_l4 = true;
    }
    return true;
}

inline IpPairKey reverse_key(const IpPairKey& key) {
    IpPairKey output = key;
    output.source = key.destination;
    output.destination = key.source;
    return output;
}

inline FlowKey reverse_key(const FlowKey& key) {
    FlowKey output = key;
    output.source = key.destination;
    output.destination = key.source;
    output.source_port = key.destination_port;
    output.destination_port = key.source_port;
    return output;
}

} // namespace tuntom
