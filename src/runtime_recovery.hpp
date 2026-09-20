#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <new>
#include <ostream>
#include <sys/select.h>

namespace tuntom {

// Resource failures discard the interrupted operation, not the process.
// Back off data/maintenance work while continuing to serve the control socket.
// This path neither allocates nor logs (logging may itself be the failing work).
class RuntimeRecovery {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto retry_interval = std::chrono::milliseconds(100);

    void poll_failed(int error) {
        ++poll_errors_;
        last_poll_error_ = error;
        retry_after_ = Clock::now() + retry_interval;
    }

    void allocation_failed() {
        ++allocation_errors_;
        retry_after_ = Clock::now() + retry_interval;
    }

    bool wait_for_retry(int control_fd) const {
        const auto now = Clock::now();
        if (now >= retry_after_) return false;
        const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(retry_after_ - now);
        const timespec timeout {0, static_cast<long>(left.count())};
        fd_set input;
        FD_ZERO(&input);
        const bool selectable = control_fd >= 0 and control_fd < FD_SETSIZE;
        if (selectable) FD_SET(control_fd, &input);
        // Use a fixed-size, allocation-free fallback instead of retrying a
        // failing poll in a hot loop. Only control can wake the backoff early.
        // Large control FDs are serviced nonblocking after this <=100ms wait.
        if (::pselect(selectable ? control_fd + 1 : 0,
                      selectable ? &input : nullptr, nullptr, nullptr,
                      &timeout, nullptr) < 0 and errno != EINTR) {
            // A bad control FD must not turn the recovery path into a spin.
            ::pselect(0, nullptr, nullptr, nullptr, &timeout, nullptr);
        }
        return true;
    }

    void write_stats(std::ostream& out) const {
        out << "runtime_poll_errors=" << poll_errors_ << "\n"
            << "runtime_last_poll_errno=" << last_poll_error_ << "\n"
            << "runtime_allocation_errors=" << allocation_errors_ << "\n";
    }

private:
    Clock::time_point retry_after_ {};
    std::uint64_t poll_errors_ = 0, allocation_errors_ = 0;
    int last_poll_error_ = 0;
};

} // namespace tuntom
