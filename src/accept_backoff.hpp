#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ostream>

namespace tuntom {

inline int deadline_timeout_ms(std::chrono::steady_clock::time_point now,
                               std::chrono::steady_clock::time_point deadline,
                               int maximum) {
    if (now >= deadline) return 0;
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<int>(std::min<std::int64_t>(maximum, left));
}

// A ready listener with a persistently failing accept must leave the poll set.
// Only that listener is paused; established connections continue to run.
class AcceptBackoff {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr auto retry_interval = std::chrono::seconds(1);

    bool ready(Time now) const { return now >= retry_after_; }
    int poll_timeout_ms(Time now, int maximum) const {
        return ready(now) ? maximum : deadline_timeout_ms(now, retry_after_, maximum);
    }
    void failed(int error, Time now) {
        if (error == EAGAIN or error == EWOULDBLOCK or error == EINTR) return;
        ++errors_;
        last_error_ = error;
        retry_after_ = now + retry_interval;
    }
    void write_stats(std::ostream& out, const char* prefix, Time now) const {
        out << prefix << "_accept_errors=" << errors_ << '\n'
            << prefix << "_accept_last_errno=" << last_error_ << '\n'
            << prefix << "_accept_backoff=" << (ready(now) ? 0 : 1) << '\n';
    }

private:
    Time retry_after_ {};
    std::uint64_t errors_ = 0;
    int last_error_ = 0;
};

} // namespace tuntom
