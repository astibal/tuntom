#pragma once

#include "accept_backoff.hpp"
#include <cstddef>
#include <cstdlib>
#include <dirent.h>
#include <limits>
#include <stdexcept>
#include <sys/resource.h>

namespace tuntom {

struct SwitchCapacity {
    static constexpr std::size_t fd_reserve = 16;
    std::size_t ports = 256;
    std::size_t pending = 16;

    // Pending slots remain separate so a full port table still permits a
    // replacement registration. At least one slot of each kind is required.
    SwitchCapacity limited_to(std::size_t free_fds) const {
        if (free_fds < fd_reserve + 2)
            throw std::runtime_error("Insufficient FD capacity for switch ports, registration and control reserve");
        const auto budget = free_fds - fd_reserve;
        const auto effective_pending = std::min(pending, budget - 1);
        return {std::min(ports, budget - effective_pending), effective_pending};
    }

    // Startup only, before the logger thread starts. Count inherited FDs too,
    // excluding the temporary directory FD and FDs above the soft limit (which
    // can exist if the parent lowered its limit after opening them).
    SwitchCapacity for_process(std::size_t additional_reserve = 0) const {
        rlimit limit {};
        if (::getrlimit(RLIMIT_NOFILE, &limit) < 0)
            throw std::runtime_error("Cannot read switch RLIMIT_NOFILE");
        if (limit.rlim_cur == RLIM_INFINITY) return *this;
        DIR* directory = ::opendir("/proc/self/fd");
        if (not directory) throw std::runtime_error("Cannot count switch descriptors");
        struct CloseDirectory {
            DIR* value;
            ~CloseDirectory() { ::closedir(value); }
        } guard {directory};
        rlim_t used = 0;
        errno = 0;
        while (const auto* entry = ::readdir(directory)) {
            char* end = nullptr;
            const auto fd = std::strtoull(entry->d_name, &end, 10);
            if (end == entry->d_name or *end != '\0') continue;
            if (fd < limit.rlim_cur and fd != static_cast<unsigned>(::dirfd(directory))) ++used;
        }
        if (errno != 0) throw std::runtime_error("Cannot enumerate switch descriptors");
        const auto free_fds = limit.rlim_cur > used ? limit.rlim_cur - used : 0;
        const auto available = static_cast<std::size_t>(std::min<rlim_t>(
            free_fds, std::numeric_limits<std::size_t>::max()));
        return limited_to(available - std::min(available, additional_reserve));
    }
};

class SwitchAdmission {
public:
    using Clock = AcceptBackoff::Clock;
    using Time = Clock::time_point;
    static constexpr auto registration_timeout = std::chrono::seconds(5);
    static constexpr auto token_interval = std::chrono::microseconds(31250);
    static constexpr unsigned burst = 16;

    explicit SwitchAdmission(Time now) : updated_at_(now) {}

    bool ready(Time now) {
        refill(now);
        return backoff_.ready(now) and credit_ >= token_interval;
    }
    // Charge every syscall attempt, including aborted/failed connections.
    void attempted(Time now) {
        refill(now);
        credit_ -= token_interval;
        if (credit_ < token_interval) ++rate_limit_hits_;
    }
    void failed(int error, Time now) { backoff_.failed(error, now); }
    int poll_timeout_ms(Time now, int maximum) {
        refill(now);
        if (not backoff_.ready(now)) return backoff_.poll_timeout_ms(now, maximum);
        return credit_ >= token_interval ? maximum
            : deadline_timeout_ms(now, now + (token_interval - credit_), maximum);
    }
    void write_stats(std::ostream& out, Time now) const {
        backoff_.write_stats(out, "listener", now);
        out << "listener_accept_rate_limit_hits=" << rate_limit_hits_ << '\n';
    }

private:
    void refill(Time now) {
        if (now > updated_at_) {
            credit_ = std::min<Clock::duration>(token_interval * burst, credit_ + (now - updated_at_));
            updated_at_ = now;
        }
    }
    AcceptBackoff backoff_;
    Time updated_at_;
    Clock::duration credit_ = token_interval * burst;
    std::uint64_t rate_limit_hits_ = 0;
};

} // namespace tuntom
