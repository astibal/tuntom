#include "../src/adaptive_polling.hpp"
#include <chrono>
#include <stdexcept>

static void require(bool condition) {
    if (not condition) throw std::runtime_error("adaptive polling check failed");
}

int main() {
    using Polling = tuntom::AdaptivePolling;
    Polling polling;
    auto now = Polling::Time {};

    require(polling.batch_size() == 1);
    for (unsigned i = 0; i < Polling::entry_streak; ++i)
        polling.observe_poll(std::chrono::microseconds(5));
    require(not polling.overloaded());
    require(polling.should_check_backlog());

    // Fast polls alone are insufficient; queued work must be confirmed.
    polling.observe_backlog(false, now);
    require(not polling.overloaded());
    polling.observe_backlog(true, now);
    require(polling.overloaded());
    require(polling.batch_size() == 4);
    require(polling.overload_entries() == 1);
    require(polling.backlog_confirmations() == 1);
    polling.note_slice_limit();
    require(polling.slice_limit_hits() == 1);

    for (unsigned i = Polling::entry_streak; i < 32; ++i)
        polling.observe_poll(std::chrono::microseconds(1));
    require(polling.batch_size() == 8);
    for (unsigned i = 32; i < 128; ++i)
        polling.observe_poll(std::chrono::microseconds(1));
    require(polling.batch_size() == 16);

    // Lack of queued work uses hysteresis rather than ending a batch burst.
    polling.observe_backlog(false, now + std::chrono::milliseconds(31));
    require(polling.overloaded());
    polling.observe_backlog(false, now + std::chrono::milliseconds(32));
    require(not polling.overloaded());
    require(polling.batch_size() == 1);

    // A genuinely blocking poll exits overload immediately.
    for (unsigned i = 0; i < Polling::entry_streak; ++i)
        polling.observe_poll(std::chrono::microseconds(0));
    now += std::chrono::seconds(1);
    polling.observe_backlog(true, now);
    require(polling.overloaded());
    polling.observe_poll(std::chrono::microseconds(50));
    require(not polling.overloaded());
}
