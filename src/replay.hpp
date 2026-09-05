#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace tuntom {

class SequenceGenerator {
public:
    std::uint64_t next() {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto nanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

        std::uint64_t candidate = static_cast<std::uint64_t>(nanoseconds);

        if (candidate <= last_sequence_) {
            candidate = last_sequence_ + 1;
        }

        last_sequence_ = candidate;
        return candidate;
    }

private:
    std::uint64_t last_sequence_ = 0;
};

class ReplayWindow {
public:
    bool accept(std::uint64_t sequence) {
        if (sequence == 0) {
            return false;
        }

        // V2/V4 sequences are timestamps, not consecutive packet counters.
        // Keep the highest accepted values in descending order so the
        // window is bounded by received packets rather than nanoseconds.
        const auto end = sequences_.begin() +
            static_cast<std::ptrdiff_t>(sequence_count_);
        const auto position = std::lower_bound(
            sequences_.begin(), end, sequence,
            [](std::uint64_t accepted, std::uint64_t candidate) {
                return accepted > candidate;
            });

        if (position != end and *position == sequence) {
            return false;
        }

        const std::size_t index =
            static_cast<std::size_t>(position - sequences_.begin());
        if (index == sequences_.size()) {
            // Once full, values below the retained window stay rejected,
            // including duplicates whose entries have been evicted.
            return false;
        }

        for (std::size_t i = std::min(sequence_count_, sequences_.size() - 1);
             i > index; --i) {
            sequences_[i] = sequences_[i - 1];
        }
        sequences_[index] = sequence;
        sequence_count_ = std::min(sequence_count_ + 1, sequences_.size());
        return true;
    }

private:
    std::array<std::uint64_t, 64> sequences_ {};
    std::size_t sequence_count_ = 0;
};

} // namespace tuntom
