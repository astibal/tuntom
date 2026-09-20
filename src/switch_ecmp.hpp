#pragma once

#include "ip_flow.hpp"
#include "ipc/switch_protocol.hpp"
#include "vendor/siphash.hpp"
#include <string_view>

namespace tuntom {

// Stable public keys define the ECMP mapping, not authentication. Separate
// domains for endpoint hashes, opaque frames, member identities and HRW scores.
inline constexpr linux_siphash::siphash_key_t ecmp_endpoint_key{
    {0x74756e746f6d2d31ULL, 0x65636d702d666c6fULL}};
inline constexpr linux_siphash::siphash_key_t ecmp_opaque_key{
    {0x74756e746f6d2d31ULL, 0x65636d702d6f7061ULL}};
inline constexpr linux_siphash::siphash_key_t ecmp_member_key{
    {0x74756e746f6d2d31ULL, 0x65636d702d706f72ULL}};
inline constexpr linux_siphash::siphash_key_t ecmp_score_key{
    {0x74756e746f6d2d31ULL, 0x65636d702d687277ULL}};

inline std::uint64_t ecmp_port_identity(std::string_view name) {
    return linux_siphash::siphash(reinterpret_cast<const std::uint8_t *>(name.data()),
                                  name.size(), &ecmp_member_key);
}

inline std::uint64_t ecmp_flow_hash(const SwitchFrameView &frame) {
    ParsedIpFlow flow;
    if (!parse_ip_flow(frame.payload, frame.payload_size, flow)) {
        // IPC explicitly permits opaque payloads. Keep those on a stable path
        // per label stack rather than hashing changing payload bytes.
        return linux_siphash::siphash(frame.labels, frame.label_count * switch_label_size,
                                      &ecmp_opaque_key);
    }
    const bool l4 = flow.has_l4 && !flow.fragmented;
    const std::size_t address_size = flow.l3.version == 4 ? 4 : 16;
    std::uint8_t source[20]{}, destination[20]{};
    source[0] = destination[0] = flow.l3.version;
    source[1] = destination[1] = l4 ? flow.l4.protocol : 0;
    std::memcpy(source + 2, flow.l3.source.data(), address_size);
    std::memcpy(destination + 2, flow.l3.destination.data(), address_size);
    auto size = address_size + 2;
    if (l4) {
        source[size] = static_cast<std::uint8_t>(flow.l4.source_port >> 8);
        source[size + 1] = static_cast<std::uint8_t>(flow.l4.source_port);
        destination[size] = static_cast<std::uint8_t>(flow.l4.destination_port >> 8);
        destination[size + 1] = static_cast<std::uint8_t>(flow.l4.destination_port);
        size += 2;
    }
    return linux_siphash::siphash(source, size, &ecmp_endpoint_key) ^
           linux_siphash::siphash(destination, size, &ecmp_endpoint_key);
}

// Streaming rendezvous selection, identical in ST and MP. Caller supplies only
// currently available members. A single member never parses/hashes the packet.
// Ties use the full port name, independent of iteration/registration order.
class EcmpSelector {
    const SwitchFrameView &frame_;
    std::size_t count_ = 0;
    std::uint64_t flow_ = 0, identity_ = 0, score_ = 0;
    std::string_view name_;

    std::uint64_t score(std::uint64_t identity) const {
        return linux_siphash::siphash_2u64(flow_, identity, &ecmp_score_key);
    }

public:
    explicit EcmpSelector(const SwitchFrameView &frame) : frame_(frame) {}
    bool consider(std::uint64_t identity, std::string_view name) {
        if (++count_ == 1) {
            identity_ = identity;
            name_ = name;
            return true;
        }
        if (count_ == 2) {
            flow_ = ecmp_flow_hash(frame_);
            score_ = score(identity_);
        }
        const auto candidate = score(identity);
        if (candidate < score_ || (candidate == score_ && name >= name_)) return false;
        score_ = candidate;
        name_ = name;
        return true;
    }
    bool multipath() const { return count_ > 1; }
};

} // namespace tuntom
