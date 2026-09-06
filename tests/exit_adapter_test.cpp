#include "../src/adapter/exit_adapter.hpp"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace tuntom;

void require(bool value, const char* why) {
    if (not value) throw std::runtime_error(why);
}

std::vector<std::uint8_t> ipv4(
    std::uint8_t protocol, std::uint16_t sport, std::uint16_t dport,
    std::uint8_t source, std::uint8_t destination, bool later_fragment = false) {
    std::vector<std::uint8_t> packet(24);
    packet[0] = 0x45;
    packet[2] = 0;
    packet[3] = 24;
    if (later_fragment) packet[7] = 1;
    packet[9] = protocol;
    packet[12] = 10; packet[15] = source;
    packet[16] = 10; packet[19] = destination;
    packet[20] = static_cast<std::uint8_t>(sport >> 8);
    packet[21] = static_cast<std::uint8_t>(sport);
    packet[22] = static_cast<std::uint8_t>(dport >> 8);
    packet[23] = static_cast<std::uint8_t>(dport);
    return packet;
}

std::vector<std::uint8_t> ipv6(
    std::uint16_t sport, std::uint16_t dport,
    std::uint8_t source, std::uint8_t destination) {
    std::vector<std::uint8_t> packet(52);
    packet[0] = 0x60;
    packet[5] = 12;
    packet[6] = 0; // hop-by-hop extension
    packet[23] = source;
    packet[39] = destination;
    packet[40] = 17; // UDP after the extension
    packet[48] = static_cast<std::uint8_t>(sport >> 8);
    packet[49] = static_cast<std::uint8_t>(sport);
    packet[50] = static_cast<std::uint8_t>(dport >> 8);
    packet[51] = static_cast<std::uint8_t>(dport);
    return packet;
}

int main() {
    using Clock = ExitAdapterRoutes::Clock;
    const auto start = Clock::now();
    ExitAdapterRoutes routes(2, 2, std::chrono::seconds(10), std::chrono::seconds(20));
    auto request = ipv4(6, 12345, 443, 1, 2);
    auto response = ipv4(6, 443, 12345, 2, 1);
    std::vector<std::uint64_t> labels {17, 99};
    std::vector<std::uint64_t> found;

    require(routes.learn(request.data(), request.size(), labels, start), "valid request rejected");
    require(routes.l3_size() == 1 and routes.l4_size() == 1, "both caches not learned");
    require(routes.lookup(response.data(), response.size(), found, start) and found == labels,
            "reverse L4 lookup failed");

    auto fragment = ipv4(6, 0, 0, 2, 1, true);
    found.clear();
    require(routes.lookup(fragment.data(), fragment.size(), found, start) and found == labels,
            "fragment did not use L3 fallback");

    auto unknown = ipv4(17, 53, 40000, 9, 8);
    require(not routes.lookup(unknown.data(), unknown.size(), found, start),
            "unknown tuple was not dropped");
    require(not routes.lookup(response.data(), response.size(), found,
                              start + std::chrono::seconds(21)),
            "expired entries remained usable");

    auto request6 = ipv6(5353, 53, 1, 2);
    auto response6 = ipv6(53, 5353, 2, 1);
    require(routes.learn(request6.data(), request6.size(), {6}, start),
            "valid IPv6 request rejected");
    require(routes.lookup(response6.data(), response6.size(), found, start) and
            found == std::vector<std::uint64_t> {6},
            "IPv6 extension-header L4 lookup failed");

    ExitAdapterRoutes lru(1, 1, std::chrono::hours(1), std::chrono::hours(1));
    auto second = ipv4(17, 1000, 2000, 3, 4);
    lru.learn(request.data(), request.size(), {1}, start);
    lru.learn(second.data(), second.size(), {2}, start);
    require(not lru.lookup(response.data(), response.size(), found, start),
            "LRU capacity did not evict oldest flow");

    std::cout << "PASS: exit adapter IPv4/IPv6 learning, fallback, expiry and LRU\n";
}
