#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ostream>

namespace tuntom {

// Fixed five-second buckets. Counter deltas belong to the observation time.
class ThroughputStats {
public:
    using Clock = std::chrono::steady_clock;
    using Counters = std::array<std::uint64_t, 4>;
    static constexpr std::size_t bucket_count = 12;

    void update(Clock::time_point now, const Counters& counters) {
        if (!initialized_) {
            initialized_ = true;
            bucket_start_ = now;
            previous_ = counters;
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
        for (std::size_t i = 0; i < counters.size(); ++i) {
            current_[i] += counters[i] >= previous_[i] ? counters[i] - previous_[i] : counters[i];
        }
        previous_ = counters;
    }

    void write(std::ostream& out) const {
        static constexpr std::array<const char*, 4> names {
            "tun_rx", "tun_tx", "udp_rx", "udp_tx"
        };
        const auto flags = out.flags();
        const auto precision = out.precision();
        out << "throughput_bucket_seconds=5\n"
            << "throughput_window_buckets=" << completed_ << "\n"
            << std::fixed << std::setprecision(3);
        for (std::size_t i = 0; i < names.size(); ++i) {
            double total = 0;
            for (const auto& bucket : buckets_) total += static_cast<double>(bucket[i]);
            const double latest = completed_ == 0 ? 0.0 :
                static_cast<double>(buckets_[(next_ + bucket_count - 1) % bucket_count][i]);
            out << names[i] << "_bps_5s=" << latest * 8.0 / 5.0 << "\n"
                << names[i] << "_bps_1m=" << (completed_ == 0 ? 0.0 :
                    total * 8.0 / (5.0 * static_cast<double>(completed_))) << "\n";
        }
        out.flags(flags);
        out.precision(precision);
    }

private:
    bool initialized_ = false;
    Clock::time_point bucket_start_ {};
    Counters previous_ {};
    Counters current_ {};
    std::array<Counters, bucket_count> buckets_ {};
    std::size_t next_ = 0;
    std::size_t completed_ = 0;
};

} // namespace tuntom
