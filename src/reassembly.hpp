#pragma once

#include "packet.hpp"
#include "processing_stats.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <list>
#include <ostream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tuntom {

struct FragmentRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

// Shared by active/previous sessions. Counters survive session retirement;
// gauges and high-water marks describe their combined live state.
struct ReassemblyMetrics {
    std::size_t active_entries = 0, active_bytes = 0;
    std::size_t peak_entries = 0, peak_bytes = 0, discarded_ids = 0;
    std::uint64_t completed_packets = 0, capacity_evictions = 0;
    std::uint64_t expired_entries = 0, late_fragment_drops = 0;
    std::uint64_t invalid_fragments = 0, overlap_drops = 0;
    std::uint64_t capacity_drops = 0, discarded_id_evictions = 0;
    std::uint64_t session_discarded_entries = 0;

    void write(std::ostream& out) const {
        out << "reassembly_limit_entries_per_session=" << max_reassembly_entries << "\n"
            << "reassembly_limit_bytes_per_session=" << max_reassembly_bytes << "\n"
            << "reassembly_active_entries=" << active_entries << "\n"
            << "reassembly_active_bytes=" << active_bytes << "\n"
            << "reassembly_peak_entries=" << peak_entries << "\n"
            << "reassembly_peak_bytes=" << peak_bytes << "\n"
            << "reassembly_completed_packets=" << completed_packets << "\n"
            << "reassembly_capacity_evictions=" << capacity_evictions << "\n"
            << "reassembly_expired_entries=" << expired_entries << "\n"
            << "reassembly_late_fragment_drops=" << late_fragment_drops << "\n"
            << "reassembly_invalid_fragments=" << invalid_fragments << "\n"
            << "reassembly_overlap_drops=" << overlap_drops << "\n"
            << "reassembly_capacity_drops=" << capacity_drops << "\n"
            << "reassembly_discarded_ids=" << discarded_ids << "\n"
            << "reassembly_discarded_id_evictions=" << discarded_id_evictions << "\n"
            << "reassembly_session_discarded_entries=" << session_discarded_entries << "\n";
    }
};

struct ReassemblyLimits {
    std::size_t entries = max_reassembly_entries;
    std::size_t bytes = max_reassembly_bytes;
    std::size_t discarded_ids = max_reassembly_discarded;
};

class Reassembler {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr auto timeout = std::chrono::seconds(reassembly_timeout_seconds);

    explicit Reassembler(std::size_t maximum_packet_size,
                         ReassemblyMetrics* metrics = nullptr,
                         ReassemblyLimits limits = {})
        : maximum_packet_size_(maximum_packet_size), limits_(limits),
          metrics_(metrics ? *metrics : own_metrics_) {
        if (limits.entries == 0 or limits.bytes == 0 or limits.discarded_ids == 0)
            throw std::invalid_argument("Reassembly limits must be positive");
    }

    // Entries hold list iterators, and metrics may refer to our own storage.
    Reassembler(const Reassembler&) = delete;
    Reassembler& operator=(const Reassembler&) = delete;
    Reassembler(Reassembler&&) = delete;
    Reassembler& operator=(Reassembler&&) = delete;

    ~Reassembler() {
        metrics_.active_entries -= entries_.size();
        metrics_.active_bytes -= total_bytes_;
        metrics_.discarded_ids -= discarded_.size();
        metrics_.session_discarded_entries += entries_.size();
    }

    const ReassemblyMetrics& metrics() const { return metrics_; }

    bool accept(const Packet& fragment, std::vector<std::uint8_t>& complete_packet,
                ProcessingStats* span_stats = nullptr, Time now = Clock::now()) {
        complete_packet.clear();
        if (fragment.message_id == 0 or fragment.original_length == 0 or
            fragment.original_length > maximum_packet_size_ or
            fragment.payload.empty()) {
            ++metrics_.invalid_fragments;
            return false;
        }
        const std::uint64_t end64 =
            std::uint64_t(fragment.fragment_offset) + fragment.payload.size();
        if (end64 > fragment.original_length) {
            ++metrics_.invalid_fragments;
            return false;
        }

        const auto begin = fragment.fragment_offset;
        const auto end = static_cast<std::uint32_t>(end64);
        // Whole DATA uses its wire sequence as an implicit ID, independently
        // of explicit fragmented-message IDs. It never consumes pool capacity.
        if (begin == 0 and end == fragment.original_length) {
            complete_packet = fragment.payload;
            return true;
        }

        // Only expired queue heads are visited; never scan the live pool, even
        // when every new message requires capacity eviction.
        cleanup_expired(now);
        if (discarded_.find(fragment.message_id) != discarded_.end()) {
            ++metrics_.late_fragment_drops;
            return false;
        }

        auto iterator = entries_.find(fragment.message_id);
        if (iterator == entries_.end()) {
            // Check before evicting: an inadmissible packet must not empty the
            // pool. Production's byte limit accommodates every supported MTU.
            if (fragment.original_length > limits_.bytes) {
                ++metrics_.capacity_drops;
                remember_discarded(fragment.message_id, now);
                return false;
            }
            while (entries_.size() >= limits_.entries or
                   fragment.original_length > limits_.bytes - total_bytes_) {
                auto oldest = entries_.find(arrival_order_.front());
                remember_discarded(oldest->first, now);
                erase(oldest);
                ++metrics_.capacity_evictions;
            }

            // Any offset can open a new message: UDP reordering is supported.
            Entry entry;
            entry.original_length = fragment.original_length;
            entry.buffer.resize(fragment.original_length);
            entry.first_seen = entry.last_update = now;
            arrival_order_.push_back(fragment.message_id);
            entry.arrival = std::prev(arrival_order_.end());
            try {
                activity_order_.push_back(fragment.message_id);
                entry.activity = std::prev(activity_order_.end());
                try {
                    iterator = entries_.emplace(fragment.message_id, std::move(entry)).first;
                } catch (...) {
                    activity_order_.pop_back();
                    throw;
                }
            } catch (...) {
                arrival_order_.pop_back();
                throw;
            }
            total_bytes_ += fragment.original_length;
            ++metrics_.active_entries;
            metrics_.active_bytes += fragment.original_length;
            metrics_.peak_entries = std::max(metrics_.peak_entries, metrics_.active_entries);
            metrics_.peak_bytes = std::max(metrics_.peak_bytes, metrics_.active_bytes);
        }

        Entry& entry = iterator->second;
        if (entry.original_length != fragment.original_length or
            entry.ranges.size() >= max_fragments_per_packet) {
            ++metrics_.invalid_fragments;
            remember_discarded(fragment.message_id, now);
            erase(iterator);
            return false;
        }
        for (const auto& range : entry.ranges) {
            if (begin < range.end and end > range.begin) {
                ++metrics_.overlap_drops;
                return false;
            }
        }
        entry.ranges.push_back({begin, end});
        std::memcpy(entry.buffer.data() + begin,
                    fragment.payload.data(), fragment.payload.size());
        entry.received_bytes += fragment.payload.size();
        entry.last_update = now;
        activity_order_.splice(activity_order_.end(), activity_order_, entry.activity);
        if (entry.received_bytes != entry.original_length) return false;

        if (span_stats != nullptr and span_stats->select()) {
            span_stats->record(std::chrono::duration<double, std::micro>(
                now - entry.first_seen).count());
        }
        complete_packet = std::move(entry.buffer);
        erase(iterator);
        ++metrics_.completed_packets;
        return true;
    }

    void cleanup_expired(Time now = Clock::now()) {
        while (not discarded_order_.empty() and
               now >= discarded_order_.front().expires) {
            forget_oldest_discarded();
        }
        while (not activity_order_.empty()) {
            auto oldest = entries_.find(activity_order_.front());
            if (now - oldest->second.last_update < timeout) break;
            remember_discarded(oldest->first, now);
            erase(oldest);
            ++metrics_.expired_entries;
        }
    }

private:
    struct Entry {
        std::uint32_t original_length = 0;
        std::vector<std::uint8_t> buffer;
        std::vector<FragmentRange> ranges;
        std::size_t received_bytes = 0;
        Time first_seen {}, last_update {};
        std::list<std::uint64_t>::iterator arrival, activity;
    };
    struct Discarded { std::uint64_t id; Time expires; };
    using EntryIterator = std::unordered_map<std::uint64_t, Entry>::iterator;

    void erase(EntryIterator iterator) {
        const auto bytes = iterator->second.original_length;
        arrival_order_.erase(iterator->second.arrival);
        activity_order_.erase(iterator->second.activity);
        total_bytes_ -= bytes;
        --metrics_.active_entries;
        metrics_.active_bytes -= bytes;
        entries_.erase(iterator);
    }

    void remember_discarded(std::uint64_t id, Time now) {
        // Late arrivals never refresh this record or evict another live packet.
        if (discarded_.find(id) != discarded_.end()) return;
        if (discarded_.size() >= limits_.discarded_ids) {
            forget_oldest_discarded();
            ++metrics_.discarded_id_evictions;
        }
        discarded_order_.push_back({id, now + timeout});
        try {
            discarded_.emplace(id, std::prev(discarded_order_.end()));
        } catch (...) {
            discarded_order_.pop_back();
            throw;
        }
        ++metrics_.discarded_ids;
    }

    void forget_oldest_discarded() {
        discarded_.erase(discarded_order_.front().id);
        discarded_order_.pop_front();
        --metrics_.discarded_ids;
    }

    std::size_t maximum_packet_size_;
    ReassemblyLimits limits_;
    ReassemblyMetrics own_metrics_;
    ReassemblyMetrics& metrics_;
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::list<std::uint64_t> arrival_order_, activity_order_;
    std::size_t total_bytes_ = 0;
    std::list<Discarded> discarded_order_;
    std::unordered_map<std::uint64_t, std::list<Discarded>::iterator> discarded_;
};

} // namespace tuntom
