#pragma once

#include "ip_flow.hpp"
#include "lru_cache.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tuntom {

class ExitAdapterRoutes {
public:
    using Clock = std::chrono::steady_clock;

    ExitAdapterRoutes(
        std::size_t l3_capacity,
        std::size_t l4_capacity,
        Clock::duration l3_timeout,
        Clock::duration l4_timeout)
        : l3_(l3_capacity, l3_timeout), l4_(l4_capacity, l4_timeout) {}

    bool learn(
        const std::uint8_t* packet,
        std::size_t size,
        const std::vector<std::uint64_t>& labels,
        Clock::time_point now = Clock::now()) {

        ParsedIpFlow flow;
        if (not parse_ip_flow(packet, size, flow) or labels.empty()) return false;
        l3_.put(reverse_key(flow.l3), labels, now);
        if (flow.has_l4) l4_.put(reverse_key(flow.l4), labels, now);
        return true;
    }

    bool lookup(
        const std::uint8_t* packet,
        std::size_t size,
        std::vector<std::uint64_t>& labels,
        Clock::time_point now = Clock::now()) {

        ParsedIpFlow flow;
        if (not parse_ip_flow(packet, size, flow)) return false;
        if (flow.has_l4 and l4_.get(flow.l4, labels, now)) {
            std::vector<std::uint64_t> ignored;
            l3_.get(flow.l3, ignored, now);
            return true;
        }
        return l3_.get(flow.l3, labels, now);
    }

    std::size_t l3_size() const { return l3_.size(); }
    std::size_t l4_size() const { return l4_.size(); }

private:
    LruCache<IpPairKey, std::vector<std::uint64_t>, IpPairHash> l3_;
    LruCache<FlowKey, std::vector<std::uint64_t>, FlowHash> l4_;
};

} // namespace tuntom
