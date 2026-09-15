#pragma once

#include "codec.hpp"
#include "../ip_flow.hpp"
#include <chrono>
#include <list>
#include <ostream>
#include <unordered_map>
#include <unordered_set>

namespace tuntom::divert {
using Clock = std::chrono::steady_clock;
struct PacketInfo { FlowKey flow; unsigned flags = 0; };
inline bool packet_info(const std::uint8_t* data, std::size_t size, PacketInfo& result) {
    ParsedIpFlow parsed;
    if (!parse_ip_flow(data, size, parsed) || !parsed.has_l4 || parsed.fragmented) return false;
    std::size_t offset, end;
    if (parsed.l3.version == 4) {
        offset = std::size_t(data[0] & 15) * 4;
        end = load_u16(data + 2);
    } else {
        offset = 40; end = 40U + load_u16(data + 4);
        auto protocol = data[6];
        for (unsigned i = 0; ip6_extension(protocol) && i < 8; ++i) {
            if (offset + 2 > end || protocol == 44) return false;
            const auto next = data[offset];
            const auto length = protocol == 51 ? (std::size_t(data[offset + 1]) + 2) * 4 :
                (std::size_t(data[offset + 1]) + 1) * 8;
            if (length > end - offset) return false;
            offset += length; protocol = next;
        }
        if (protocol != parsed.protocol) return false;
    }
    result = {};
    if (parsed.protocol == 6) {
        if (offset + 20 > end) return false;
        const auto header = std::size_t(data[offset + 12] >> 4) * 4;
        if (header < 20 || header > end - offset) return false;
        result.flags = data[offset + 13];
    } else if (parsed.protocol == 17) {
        if (offset + 8 > end) return false;
        const auto length = load_u16(data + offset + 4);
        if (length < 8 || length > end - offset) return false;
    } else return false;
    result.flow = parsed.l4;
    return true;
}
inline FlowKey canonical(FlowKey key) {
    const auto reversed = reverse_key(key);
    return reversed.source < key.source ||
        (reversed.source == key.source && reversed.source_port < key.source_port) ? reversed : key;
}
enum class AdmissionResult { proxy, bypass, full };
class Admission {
    using Set = std::unordered_set<FlowKey, FlowHash>;
    Set existing_tcp_, diverted_tcp_, existing_udp_;
    std::size_t capacity_;
    bool tcp_positive_released_ = false, tcp_negative_released_ = false, udp_released_ = false;
    static void release(Set& set, bool& released) {
        if (!released) { Set{}.swap(set); released = true; }
    }
    bool remember(Set& set, const FlowKey& key) {
        if (set.count(key)) return true;
        if (set.size() >= capacity_) return false;
        set.insert(key); return true;
    }
public:
    explicit Admission(std::size_t capacity) : capacity_(capacity) {}
    void maintain(double seconds) {
        if (seconds >= 3600) {
            release(diverted_tcp_, tcp_positive_released_);
            release(existing_udp_, udp_released_);
        }
        if (seconds >= 86400) release(existing_tcp_, tcp_negative_released_);
    }
    AdmissionResult classify(const PacketInfo& packet, double seconds) {
        maintain(seconds);
        // Once the learning phases end there is no admission hash lookup.
        if (packet.flow.protocol == 6 && seconds >= 86400) return AdmissionResult::proxy;
        if (packet.flow.protocol == 17 && seconds >= 3600) return AdmissionResult::proxy;
        const auto key = canonical(packet.flow);
        if (packet.flow.protocol == 6) {
            if (existing_tcp_.count(key)) return AdmissionResult::bypass;
            if (seconds >= 3600 || diverted_tcp_.count(key)) return AdmissionResult::proxy;
            if ((packet.flags & 0x12) == 0x02)
                return remember(diverted_tcp_, key) ? AdmissionResult::proxy : AdmissionResult::full;
            return remember(existing_tcp_, key) ? AdmissionResult::bypass : AdmissionResult::full;
        }
        if (seconds < 60)
            return remember(existing_udp_, key) ? AdmissionResult::bypass : AdmissionResult::full;
        return existing_udp_.count(key) ? AdmissionResult::bypass : AdmissionResult::proxy;
    }
    void stats(std::ostream& out) const {
        out << "existing_tcp=" << existing_tcp_.size() << "\ndiverted_tcp=" << diverted_tcp_.size()
            << "\nexisting_udp=" << existing_udp_.size() << '\n';
    }
};

enum class Learn { ok, full, conflict, missing };
class Routes {
    struct Entry {
        FlowKey forward;
        Envelope client, server;
        Clock::time_point touched;
        std::list<FlowKey>::iterator position;
    };
    std::unordered_map<FlowKey, Entry, FlowHash> entries_;
    std::list<FlowKey> recent_;
    std::size_t capacity_;
    Clock::duration idle_;
    std::uint64_t expired_ = 0;
    void touch(Entry& entry, Clock::time_point now) {
        entry.touched = now;
        recent_.splice(recent_.end(), recent_, entry.position);
    }
    static bool same_context(const Envelope& a, const Envelope& b) {
        return a.origin() == b.origin() && a.original() == b.original();
    }
public:
    Routes(std::size_t capacity, Clock::duration idle) : capacity_(capacity), idle_(idle) {}
    void maintain(Clock::time_point now) {
        // Bounded maintenance; capacity pressure never evicts a live entry.
        for (unsigned i = 0; i < 64 && !recent_.empty(); ++i) {
            const auto found = entries_.find(recent_.front());
            if (now - found->second.touched < idle_) break;
            entries_.erase(found); recent_.pop_front(); ++expired_;
        }
    }
    Learn learn(const FlowKey& flow, const Envelope& env, bool from_client, Clock::time_point now) {
        const auto key = canonical(flow);
        auto found = entries_.find(key);
        if (found != entries_.end() && now - found->second.touched >= idle_) {
            recent_.erase(found->second.position); entries_.erase(found); ++expired_;
            found = entries_.end();
        }
        if (found == entries_.end()) {
            if (!from_client) return Learn::missing;
            if (entries_.size() >= capacity_) return Learn::full;
            recent_.push_back(key);
            try {
                entries_.emplace(key, Entry{flow, env, env, now, std::prev(recent_.end())});
            } catch (...) { recent_.pop_back(); throw; }
            return Learn::ok;
        }
        auto& entry = found->second;
        if (!same_context(entry.client, env) ||
            !(flow == (from_client ? entry.forward : reverse_key(entry.forward)))) return Learn::conflict;
        if (from_client) entry.client = env; else entry.server = env;
        touch(entry, now);
        return Learn::ok;
    }
    bool lookup(const FlowKey& flow, bool to_client_side, Clock::time_point now, Envelope& env) {
        const auto found = entries_.find(canonical(flow));
        if (found == entries_.end()) return false;
        auto& entry = found->second;
        if (now - entry.touched >= idle_) return false;
        if (!(flow == (to_client_side ? reverse_key(entry.forward) : entry.forward))) return false;
        env = to_client_side ? entry.server : entry.client;
        touch(entry, now);
        return true;
    }
    void stats(std::ostream& out) const {
        out << "flow_entries=" << entries_.size() << "\nflow_expirations=" << expired_ << '\n';
    }
};
} // namespace tuntom::divert
