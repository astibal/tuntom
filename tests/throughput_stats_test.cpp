#include "../src/throughput_stats.hpp"
#include <sstream>
#include <stdexcept>
#include <string>

using Rates = tuntom::ThroughputStats;
static void expect(const Rates& rates, const std::string& field, double expected) {
    std::ostringstream out;
    rates.write(out);
    const auto text = out.str();
    const auto start = text.find(field + "=");
    if (start == std::string::npos ||
        std::stod(text.substr(start + field.size() + 1)) != expected) {
        throw std::runtime_error("Unexpected " + field + ": " + text);
    }
}

int main() {
    Rates rates;
    const auto start = Rates::Clock::time_point {};
    const auto seconds = [](int n) { return std::chrono::seconds(n); };
    rates.update(start, {100, 100, 100, 100});
    rates.update(start + seconds(4), {5100, 10100, 15100, 20100});
    expect(rates, "tun_rx_bps_5s", 0);
    expect(rates, "throughput_window_buckets", 0);
    rates.update(start + seconds(5), {5100, 10100, 15100, 20100});
    expect(rates, "tun_rx_bps_5s", 8000);
    expect(rates, "tun_tx_bps_5s", 16000);
    expect(rates, "udp_rx_bps_5s", 24000);
    expect(rates, "udp_tx_bps_5s", 32000);
    expect(rates, "tun_rx_bps_1m", 8000);
    // Idle buckets contribute zero and the rolling window evicts old traffic.
    rates.update(start + seconds(10), {5100, 10100, 15100, 20100});
    expect(rates, "tun_rx_bps_5s", 0);
    expect(rates, "tun_rx_bps_1m", 4000);
    rates.update(start + seconds(60), {5100, 10100, 15100, 20100});
    expect(rates, "throughput_window_buckets", 12);
    expect(rates, "tun_rx_bps_1m", 666.667);
    rates.update(start + seconds(65), {5100, 10100, 15100, 20100});
    expect(rates, "tun_rx_bps_1m", 0);
    // A boundary observation belongs to the new bucket; partial buckets stay hidden.
    rates.update(start + seconds(70), {10100, 10100, 15100, 20100});
    expect(rates, "tun_rx_bps_5s", 0);
    rates.update(start + seconds(75), {10100, 10100, 15100, 20100});
    expect(rates, "tun_rx_bps_5s", 8000);
    rates.update(start + seconds(1000), {10100, 10100, 15100, 20100});
    expect(rates, "tun_rx_bps_1m", 0);
    expect(rates, "tun_rx_bps_5s", 0);
    // Counter reset must not produce an unsigned underflow spike.
    rates.update(start + seconds(1001), {5000, 0, 0, 0});
    rates.update(start + seconds(1005), {5000, 0, 0, 0});
    expect(rates, "tun_rx_bps_5s", 8000);
}
