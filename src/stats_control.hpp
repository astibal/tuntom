#pragma once
#include <csignal>
#include <stdexcept>

namespace tuntom {
inline volatile std::sig_atomic_t stats_toggle_request = 0;
inline volatile std::sig_atomic_t stats_snapshot_request = 0;
inline void stats_signal_handler(int signal) noexcept {
    if (signal == SIGUSR1) stats_toggle_request = !stats_toggle_request;
    if (signal == SIGUSR2) stats_snapshot_request = 1;
}
struct StatsRequests {
    bool toggle = false;
    bool snapshot = false;
};
inline StatsRequests take_stats_requests() {
    if (!stats_toggle_request && !stats_snapshot_request) return {};
    // Block both handlers while consuming requests, so a new request cannot be
    // lost between reading and clearing the flags. No I/O occurs in a handler.
    sigset_t mask, previous;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    if (sigprocmask(SIG_BLOCK, &mask, &previous) != 0)
        throw std::runtime_error("Cannot block stats signals");
    const StatsRequests requests {stats_toggle_request != 0, stats_snapshot_request != 0};
    stats_toggle_request = stats_snapshot_request = 0;
    if (sigprocmask(SIG_SETMASK, &previous, nullptr) != 0)
        throw std::runtime_error("Cannot restore stats signal mask");
    return requests;
}
inline bool stats_write_requested(bool has_path, bool disabled, bool snapshot = false) {
    return has_path && (!disabled || snapshot);
}

// Installed by the standalone executable, not by an embedded Tunnel instance.
class StatsSignals {
public:
    StatsSignals() {
        stats_toggle_request = stats_snapshot_request = 0;
        struct sigaction action {};
        action.sa_handler = stats_signal_handler;
        action.sa_flags = SA_RESTART;
        sigemptyset(&action.sa_mask);
        sigaddset(&action.sa_mask, SIGUSR1);
        sigaddset(&action.sa_mask, SIGUSR2);
        if (sigaction(SIGUSR1, &action, &previous_toggle_) != 0)
            throw std::runtime_error("Cannot install SIGUSR1 stats handler");
        if (sigaction(SIGUSR2, &action, &previous_snapshot_) != 0) {
            sigaction(SIGUSR1, &previous_toggle_, nullptr);
            throw std::runtime_error("Cannot install SIGUSR2 stats handler");
        }
    }
    ~StatsSignals() {
        sigaction(SIGUSR1, &previous_toggle_, nullptr);
        sigaction(SIGUSR2, &previous_snapshot_, nullptr);
    }
    StatsSignals(const StatsSignals&) = delete;
    StatsSignals& operator=(const StatsSignals&) = delete;
private:
    struct sigaction previous_toggle_ {}, previous_snapshot_ {};
};
} // namespace tuntom
