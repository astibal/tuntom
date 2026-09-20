#pragma once

#include <chrono>
#include <cstdint>
#include <ostream>

namespace tuntom {

// Keeps the low-latency one-round event loop until repeated immediate polls
// and a post-service readiness check prove that input is queued.  Hysteresis
// prevents short bursts from repeatedly entering and leaving overload mode.
class AdaptivePolling {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    using Time = Clock::time_point;

    static constexpr auto immediate_poll = std::chrono::microseconds(5);
    static constexpr auto idle_poll = std::chrono::microseconds(50);
    static constexpr auto backlog_grace = std::chrono::milliseconds(32);
    static constexpr auto processing_slice = std::chrono::microseconds(150);
    static constexpr std::uint32_t entry_streak = 8;

    void observe_poll(Duration waited) {
        if (waited >= idle_poll) {
            leave_overload();
            return;
        }

        if (waited <= immediate_poll) {
            if (busy_streak_ != UINT32_MAX) ++busy_streak_;
        } else {
            busy_streak_ = 0;
        }
    }

    bool should_check_backlog() const {
        return overloaded_ or busy_streak_ >= entry_streak;
    }

    void observe_backlog(bool ready, Time now) {
        if (ready) {
            ++backlog_confirmations_;
            last_backlog_ = now;
            backlog_seen_ = true;
            if (not overloaded_ and busy_streak_ >= entry_streak) {
                overloaded_ = true;
                ++overload_entries_;
            }
            return;
        }

        if (
            overloaded_ and
            backlog_seen_ and
            now - last_backlog_ >= backlog_grace) {

            leave_overload();
        }
    }

    unsigned batch_size() const {
        if (not overloaded_) return 1;
        if (busy_streak_ >= 128) return 16;
        if (busy_streak_ >= 32) return 8;
        return 4;
    }

    bool overloaded() const { return overloaded_; }
    std::uint32_t busy_streak() const { return busy_streak_; }
    std::uint64_t overload_entries() const { return overload_entries_; }
    std::uint64_t backlog_confirmations() const { return backlog_confirmations_; }
    std::uint64_t slice_limit_hits() const { return slice_limit_hits_; }
    void note_slice_limit() { ++slice_limit_hits_; }

    void write_stats(std::ostream& out) const {
        out << "event_poll_overload=" << (overloaded_ ? 1 : 0) << "\n"
            << "event_poll_busy_streak=" << busy_streak_ << "\n"
            << "event_poll_batch=" << batch_size() << "\n"
            << "event_poll_overload_entries=" << overload_entries_ << "\n"
            << "event_poll_backlog_confirmations=" << backlog_confirmations_ << "\n"
            << "event_poll_slice_limit_hits=" << slice_limit_hits_ << "\n";
    }

private:
    void leave_overload() {
        overloaded_ = false;
        busy_streak_ = 0;
        backlog_seen_ = false;
    }

    bool overloaded_ = false;
    bool backlog_seen_ = false;
    std::uint32_t busy_streak_ = 0;
    std::uint64_t overload_entries_ = 0;
    std::uint64_t backlog_confirmations_ = 0;
    std::uint64_t slice_limit_hits_ = 0;
    Time last_backlog_ {};
};

} // namespace tuntom
