#include "../src/switch_admission.hpp"
#include <iostream>

static void require(bool condition, const char* message) {
    if (not condition) throw std::runtime_error(message);
}

int main() {
    using namespace std::chrono_literals;
    using Gate = tuntom::SwitchAdmission;
    const Gate::Time start {};
    Gate gate(start);
    unsigned attempts = 0;
    // Continuous demand at 1us resolution cannot exceed burst + earned tokens.
    for (int tick = 0; tick <= 1000000; ++tick) {
        const auto now = start + std::chrono::microseconds(tick);
        if (gate.ready(now)) { gate.attempted(now); ++attempts; }
        require(attempts <= 16U + static_cast<unsigned>(tick / 31250), "accept flood exceeded rate bound");
    }
    require(attempts == 48, "limiter did not refill");
    require(gate.poll_timeout_ms(start + 1s, 1000) == 32, "fractional timeout must round up");
    require(gate.poll_timeout_ms(start + 1s, 5) == 5, "limiter hid an earlier timer");
    gate.failed(EMFILE, start + 1s);
    require(not gate.ready(start + 1999ms), "accept retried during FD backoff");
    require(gate.ready(start + 2s), "accept did not recover at deadline");
    // A long idle period grants at most one burst, not an unbounded backlog.
    unsigned burst = 0;
    while (gate.ready(start + 1h)) { gate.attempted(start + 1h); ++burst; }
    require(burst == 16, "idle time accumulated too much credit");

    tuntom::AcceptBackoff control;
    for (const int error : {EMFILE, ENFILE, ENOMEM, ENOBUFS, EBADF}) {
        control.failed(error, start + 1s);
        require(not control.ready(start + 1999ms), "control error did not back off");
        require(control.poll_timeout_ms(start + 1999ms, 1000) == 1, "wrong control deadline");
        require(control.ready(start + 2s), "control did not recover");
    }
    for (const int error : {EAGAIN, EINTR}) {
        control.failed(error, start + 3s);
        require(control.ready(start + 3s), "normal accept retry added an outage");
    }
    const tuntom::SwitchCapacity configured;
    const auto normal = configured.limited_to(1024);
    require(normal.ports == 256 and normal.pending == 16, "normal limits changed");
    const auto small = configured.limited_to(59); // 64 FD limit minus five existing FDs
    require(small.ports == 27 and small.pending == 16, "FD headroom was not preserved");
    const auto minimum = configured.limited_to(18);
    require(minimum.ports == 1 and minimum.pending == 1, "no room for port replacement");
    bool rejected = false;
    try { (void)configured.limited_to(17); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "unsafe startup capacity accepted");
    std::cout << "PASS: admission flood bound, backoff deadlines and FD budget\n";
}
