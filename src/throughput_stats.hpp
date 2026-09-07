#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <initializer_list>
#include <ostream>

namespace tuntom {

// Fixed five-second buckets. Counter deltas belong to the observation time.
class ThroughputStats {
public:
    using Clock = std::chrono::steady_clock;
    struct Counter {
        std::uint64_t packets = 0;
        std::uint64_t bytes = 0;
    };
    using Counters = std::array<Counter, 6>;
    static constexpr std::size_t bucket_count = 12;

    ThroughputStats() : ThroughputStats({
        "tun_rx", "tun_tx", "udp_rx", "udp_tx", "switch_rx", "switch_tx"}) {}

    ThroughputStats(std::initializer_list<const char*> names) {
        std::size_t index = 0;
        for (const char* name : names) {
            if (index == names_.size()) break;
            names_[index++] = name;
        }
        used_ = index;
    }

    void update(Clock::time_point now, std::initializer_list<Counter> counters) {
        if (!initialized_) {
            initialized_ = true;
            bucket_start_ = now;
            std::size_t index = 0;
            for (const Counter counter : counters) {
                if (index == used_) break;
                previous_[index++] = counter;
            }
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - bucket_start_).count() / 5;
        if (elapsed > 0) {
            // A long pause must age out traffic, without iterating over the pause.
            if (elapsed > static_cast<std::int64_t>(bucket_count)) {
                buckets_ = {};
                completed_ = bucket_count;
                next_ = 0;
            } else {
                for (std::int64_t i = 0; i < elapsed; ++i) {
                    buckets_[next_] = i == 0 ? current_ : Counters {};
                    next_ = (next_ + 1) % bucket_count;
                    if (completed_ < bucket_count) ++completed_;
                }
            }
            current_ = {};
            bucket_start_ += std::chrono::seconds(elapsed * 5);
        }
        std::size_t index = 0;
        for (const Counter counter : counters) {
            if (index == used_) break;
            current_[index].packets += counter.packets >= previous_[index].packets ?
                counter.packets - previous_[index].packets : counter.packets;
            current_[index].bytes += counter.bytes >= previous_[index].bytes ?
                counter.bytes - previous_[index].bytes : counter.bytes;
            previous_[index++] = counter;
        }
    }

    void write(std::ostream& out) const {
        const auto flags = out.flags();
        const auto precision = out.precision();
        out << "throughput_bucket_seconds=5\n"
            << "throughput_window_buckets=" << completed_ << "\n"
            << std::fixed << std::setprecision(3);
        for (std::size_t i = 0; i < used_; ++i) {
            double packets_total = 0;
            double bytes_total = 0;
            for (const auto& bucket : buckets_) {
                packets_total += static_cast<double>(bucket[i].packets);
                bytes_total += static_cast<double>(bucket[i].bytes);
            }
            const Counter latest = completed_ == 0 ? Counter {} :
                buckets_[(next_ + bucket_count - 1) % bucket_count][i];
            const double seconds = completed_ == 0 ? 1.0 :
                5.0 * static_cast<double>(completed_);
            out << names_[i] << "_bps_5s="
                << static_cast<double>(latest.bytes) * 8.0 / 5.0 << "\n"
                << names_[i] << "_bps_1m="
                << (completed_ == 0 ? 0.0 : bytes_total * 8.0 / seconds) << "\n"
                << names_[i] << "_pps_5s="
                << static_cast<double>(latest.packets) / 5.0 << "\n"
                << names_[i] << "_pps_1m="
                << (completed_ == 0 ? 0.0 : packets_total / seconds) << "\n";
        }
        out.flags(flags);
        out.precision(precision);
    }

private:
    std::array<const char*, 6> names_ {};
    std::size_t used_ = 0;
    bool initialized_ = false;
    Clock::time_point bucket_start_ {};
    Counters previous_ {};
    Counters current_ {};
    std::array<Counters, bucket_count> buckets_ {};
    std::size_t next_ = 0;
    std::size_t completed_ = 0;
};

} // namespace tuntom
