#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

namespace tuntom {

// Percentiles use a bounded rolling window; average/max cover all samples.
class ProcessingStats {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::uint64_t sample_interval = 1024;
    static constexpr std::size_t window_size = 4096;

    bool select() { return attempts_++ % sample_interval == 0; }

    void record(double microseconds) {
        window_[samples_ % window_size] = microseconds;
        ++samples_;
        average_ += (microseconds - average_) / static_cast<double>(samples_);
        maximum_ = std::max(maximum_, microseconds);
    }

    void finish(Clock::time_point start) {
        record(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    }

    void write(std::ostream& out, const std::string& prefix) const {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(samples_, window_size));
        std::vector<double> sorted(window_.begin(), window_.begin() + count);
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&](std::size_t percent) {
            return count == 0 ? 0.0 : sorted[(count * percent + 99) / 100 - 1];
        };
        const auto flags = out.flags();
        const auto precision = out.precision();
        out << prefix << "_samples=" << samples_ << "\n"
            << prefix << "_window_samples=" << count << "\n"
            << std::fixed << std::setprecision(3)
            << prefix << "_avg_us=" << average_ << "\n"
            << prefix << "_max_us=" << maximum_ << "\n"
            << prefix << "_p95_us=" << percentile(95) << "\n"
            << prefix << "_p99_us=" << percentile(99) << "\n";
        out.flags(flags);
        out.precision(precision);
    }

private:
    std::uint64_t attempts_ = 0;
    std::uint64_t samples_ = 0;
    double average_ = 0;
    double maximum_ = 0;
    std::array<double, window_size> window_ {};
};

} // namespace tuntom
