#pragma once

#include "../ip_flow.hpp"
#include "lru_cache.hpp"
#include "../flow_dump.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <stdexcept>

namespace tuntom {

class ExitAdapterRoutes {
public:
    using Clock = std::chrono::steady_clock;

    ExitAdapterRoutes(
        std::size_t l3_capacity,
        std::size_t l4_capacity,
        Clock::duration l3_timeout,
        Clock::duration l4_timeout,
        bool l4_only = false,
        unsigned l4_sport_key_bits = 16)
        : l3_(l3_capacity, l3_timeout), l4_(l4_capacity, l4_timeout), l4_only_(l4_only) {
        if (l4_sport_key_bits > 16)
            throw std::invalid_argument("l4_sport_key_bits must be 0..16");
        l4_client_port_mask_ = static_cast<std::uint16_t>(
            0xffffU << (16 - l4_sport_key_bits));
    }

    bool learn(
        const std::uint8_t* packet,
        std::size_t size,
        const std::vector<std::uint64_t>& labels,
        Clock::time_point now = Clock::now()) {

        ParsedIpFlow flow;
        if (not parse_ip_flow(packet, size, flow) or labels.empty()) {
            ++parse_errors_;
            return false;
        }
        if (l4_only_ && (!flow.has_l4 || flow.fragmented)) {
            ++parse_errors_;
            return false;
        }
        ++learned_packets_;
        if (!l4_only_) l3_.put(reverse_key(flow.l3), labels, now);
        if (flow.has_l4) l4_.put(l4_cache_key(reverse_key(flow.l4)), labels, now);
        return true;
    }

    bool lookup(
        const std::uint8_t* packet,
        std::size_t size,
        std::vector<std::uint64_t>& labels,
        Clock::time_point now = Clock::now()) {

        ParsedIpFlow flow;
        if (not parse_ip_flow(packet, size, flow)) {
            record_parse_error();
            return false;
        }
        return lookup(flow, labels, now);
    }

    // The caller must supply a successfully parsed flow.
    bool lookup(
        const ParsedIpFlow& flow,
        std::vector<std::uint64_t>& labels,
        Clock::time_point now = Clock::now()) {

        if (l4_only_ && flow.fragmented) { ++l4_misses_; return false; }
        if (flow.has_l4 and l4_.get(l4_cache_key(flow.l4), labels, now)) {
            ++l4_hits_;
            std::vector<std::uint64_t> ignored;
            if (!l4_only_) l3_.get(flow.l3, ignored, now);
            return true;
        }
        if (flow.has_l4) ++l4_misses_;
        if (l4_only_) return false;
        if (l3_.get(flow.l3, labels, now)) {
            ++l3_hits_;
            return true;
        }
        ++l3_misses_;
        return false;
    }

    std::string dump_flows(Clock::time_point now = Clock::now()) const {
        FlowDump dump;
        const auto visit = [&](const char* table, const auto& cache) {
            cache.visit_live(now, [&](const auto& key, const auto& labels, auto touched) {
                dump.add(table, key, [&](auto& out) {
                    FlowDump::idle(out, now - touched);
                    out << " labels=";
                    FlowDump::labels(out, labels.data(), labels.size());
                });
            });
        };
        visit("l3", l3_); visit("l4", l4_);
        return dump.finish();
    }

    // Used when parsing is shared with another consumer before lookup.
    void record_parse_error() { ++parse_errors_; }

    std::size_t l3_size() const { return l3_.size(); }
    std::size_t l4_size() const { return l4_.size(); }
    std::uint64_t learned_packets() const { return learned_packets_; }
    std::uint64_t parse_errors() const { return parse_errors_; }
    std::uint64_t l3_hits() const { return l3_hits_; }
    std::uint64_t l3_misses() const { return l3_misses_; }
    std::uint64_t l4_hits() const { return l4_hits_; }
    std::uint64_t l4_misses() const { return l4_misses_; }
    std::uint64_t l3_evictions() const { return l3_.evictions(); }
    std::uint64_t l4_evictions() const { return l4_.evictions(); }
    std::uint64_t l3_expirations() const { return l3_.expirations(); }
    std::uint64_t l4_expirations() const { return l4_.expirations(); }

private:
    FlowKey l4_cache_key(FlowKey key) const {
        // Cache keys face TUN -> switch: the original source port is the destination.
        key.destination_port &= l4_client_port_mask_;
        return key;
    }

    std::uint16_t l4_client_port_mask_ = 0xffff;
    LruCache<IpPairKey, std::vector<std::uint64_t>, IpPairHash> l3_;
    LruCache<FlowKey, std::vector<std::uint64_t>, FlowHash> l4_;
    bool l4_only_;
    std::uint64_t learned_packets_ = 0;
    std::uint64_t parse_errors_ = 0;
    std::uint64_t l3_hits_ = 0;
    std::uint64_t l3_misses_ = 0;
    std::uint64_t l4_hits_ = 0;
    std::uint64_t l4_misses_ = 0;
};

} // namespace tuntom
