#pragma once

#include "packet.hpp"
#include "processing_stats.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tuntom {

struct FragmentRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

struct ReassemblyEntry {
    std::uint32_t original_length = 0;
    std::vector<std::uint8_t> buffer;
    std::vector<FragmentRange> ranges;
    std::size_t received_bytes = 0;
    std::chrono::steady_clock::time_point last_update {};
    std::chrono::steady_clock::time_point first_seen {};
};

class Reassembler {
public:
    explicit Reassembler(std::size_t maximum_packet_size)
        : maximum_packet_size_(maximum_packet_size) {
    }

    bool accept(
        const Packet& fragment,
        std::vector<std::uint8_t>& complete_packet,
        ProcessingStats* span_stats = nullptr) {

        complete_packet.clear();

        if (
            fragment.message_id == 0 or
            fragment.original_length == 0 or
            fragment.original_length > maximum_packet_size_ or
            fragment.payload.empty()) {

            return false;
        }

        const std::uint64_t end64 =
            static_cast<std::uint64_t>(fragment.fragment_offset) +
            fragment.payload.size();

        if (end64 > fragment.original_length) {
            return false;
        }

        const std::uint32_t begin = fragment.fragment_offset;
        const std::uint32_t end = static_cast<std::uint32_t>(end64);

        if (
            begin == 0 and
            end == fragment.original_length) {

            complete_packet = fragment.payload;
            return true;
        }

        cleanup_expired();

        auto iterator = entries_.find(fragment.message_id);

        if (iterator == entries_.end()) {
            if (
                entries_.size() >= max_reassembly_entries or
                total_bytes_ + fragment.original_length >
                    max_reassembly_bytes) {

                log_info("DROP reassembly capacity exceeded");
                return false;
            }

            ReassemblyEntry entry;
            entry.original_length = fragment.original_length;
            entry.buffer.resize(fragment.original_length);
            entry.last_update = std::chrono::steady_clock::now();
            entry.first_seen = entry.last_update;

            total_bytes_ += entry.buffer.size();

            iterator =
                entries_
                    .emplace(
                        fragment.message_id,
                        std::move(entry))
                    .first;
        }

        ReassemblyEntry& entry = iterator->second;

        if (entry.original_length != fragment.original_length) {
            erase(iterator);
            log_info("DROP inconsistent reassembly length");
            return false;
        }

        if (entry.ranges.size() >= max_fragments_per_packet) {
            erase(iterator);
            log_info("DROP too many fragments");
            return false;
        }

        for (const FragmentRange& range : entry.ranges) {
            if (begin < range.end and end > range.begin) {
                log_info("DROP duplicate/overlapping fragment");
                return false;
            }
        }

        std::memcpy(
            entry.buffer.data() + begin,
            fragment.payload.data(),
            fragment.payload.size());

        entry.ranges.push_back({begin, end});
        entry.received_bytes += fragment.payload.size();
        entry.last_update = std::chrono::steady_clock::now();

        if (entry.received_bytes != entry.original_length) {
            return false;
        }

        if (span_stats != nullptr and span_stats->select()) {
            span_stats->record(std::chrono::duration<double, std::micro>(
                entry.last_update - entry.first_seen).count());
        }
        complete_packet = std::move(entry.buffer);
        total_bytes_ -= entry.original_length;
        entries_.erase(iterator);
        return true;
    }

    void cleanup_expired() {
        const auto now = std::chrono::steady_clock::now();

        for (auto iterator = entries_.begin(); iterator != entries_.end();) {
            if (
                now - iterator->second.last_update >=
                std::chrono::seconds(reassembly_timeout_seconds)) {

                total_bytes_ -= iterator->second.buffer.size();
                iterator = entries_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

private:
    using entry_iterator =
        std::unordered_map<std::uint64_t, ReassemblyEntry>::iterator;

    void erase(entry_iterator iterator) {
        total_bytes_ -= iterator->second.buffer.size();
        entries_.erase(iterator);
    }

    std::size_t maximum_packet_size_ = default_tun_mtu;
    std::unordered_map<std::uint64_t, ReassemblyEntry> entries_;
    std::size_t total_bytes_ = 0;
};

} // namespace tuntom
