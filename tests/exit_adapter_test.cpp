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
    ParsedIpFlow response_flow;
    require(parse_ip_flow(response.data(), response.size(), response_flow), "response parsing failed");
    require(routes.lookup(response_flow, found, start) and found == labels,
            "parsed reverse L4 lookup failed");

    auto fragment = ipv4(6, 0, 0, 2, 1, true);
    found.clear();
    require(routes.lookup(fragment.data(), fragment.size(), found, start) and found == labels,
            "fragment did not use L3 fallback");
    ParsedIpFlow fragment_flow;
    require(parse_ip_flow(fragment.data(), fragment.size(), fragment_flow), "fragment parsing failed");
    require(routes.lookup(fragment_flow, found, start) and found == labels,
            "parsed fragment did not use L3 fallback");

    auto unknown = ipv4(17, 53, 40000, 9, 8);
    require(not routes.lookup(unknown.data(), unknown.size(), found, start),
            "unknown tuple was not dropped");
    require(not routes.lookup(response_flow, found, start + std::chrono::seconds(21)),
            "expired entries remained usable");
    require(not routes.lookup(nullptr, 0, found, start), "invalid raw packet accepted");
    routes.record_parse_error();
    require(routes.parse_errors() == 2, "raw and shared parsing errors must count once each");

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

    ExitAdapterRoutes strict(2, 1, std::chrono::hours(1), std::chrono::hours(1), true);
    auto same_pair = ipv4(6, 12346, 443, 1, 2);
    require(strict.learn(request.data(), request.size(), {1}, start), "strict L4 learning");
    require(strict.learn(same_pair.data(), same_pair.size(), {2}, start), "strict second flow learning");
    require(strict.l3_size() == 0, "strict mode must not learn IP-pair labels");
    require(!strict.lookup(response.data(), response.size(), found, start), "evicted flow must not inherit another flow's labels");
    require(!strict.lookup(fragment.data(), fragment.size(), found, start), "strict mode rejects fragments");
    require(!strict.learn(fragment.data(), fragment.size(), {3}, start), "strict mode rejects incomplete identities");
    // Every prefix length, both IP families; no L3 fallback can hide a bad key.
    for (unsigned bits = 0; bits <= 16; ++bits) {
        for (bool v6 : {false, true}) {
            ExitAdapterRoutes pooled(1, 8, std::chrono::seconds(10),
                                     std::chrono::seconds(20), true, bits);
            const auto packet = [&](std::uint16_t client_port, bool reverse,
                                    std::uint16_t server_port = 443) {
                const auto sport = reverse ? server_port : client_port;
                const auto dport = reverse ? client_port : server_port;
                return v6 ? ipv6(sport, dport, reverse ? 2 : 1, reverse ? 1 : 2)
                          : ipv4(6, sport, dport, reverse ? 2 : 1, reverse ? 1 : 2);
            };
            const auto learn = [&](std::uint16_t port, std::uint64_t label) {
                auto p = packet(port, false);
                require(pooled.learn(p.data(), p.size(), {label}, start), "pool learn");
            };
            const auto lookup = [&](std::uint16_t port, Clock::time_point at,
                                    std::uint16_t server = 443) {
                auto p = packet(port, true, server);
                return pooled.lookup(p.data(), p.size(), found, at);
            };
            const auto last = static_cast<std::uint16_t>((1U << (16 - bits)) - 1);
            learn(0, 1);
            learn(last, 2);
            require(pooled.l4_size() == 1, "same prefix must share one entry");
            require(lookup(0, start) && found == std::vector<std::uint64_t>{2},
                    "latest stack must apply to entire pool");
            require(lookup(last, start), "pool upper boundary missing");
            require(!lookup(last, start, 444), "server port must remain exact");
            if (bits) {
                const auto next = static_cast<std::uint16_t>(last + 1);
                require(!lookup(next, start), "adjacent pool must miss");
                learn(next, 3);
                require(pooled.l4_size() == 2, "adjacent pool must remain separate");
            }
            pooled.flush();
            require(pooled.l3_size() == 0 && pooled.l4_size() == 0, "flush counts");
            require(!lookup(last, start), "masked cache entry survived flush");
            learn(0, 4);
            require(lookup(last, start) && found == std::vector<std::uint64_t>{4}, "relearn after flush");
            require(lookup(last, start + std::chrono::seconds(15)), "pool refresh");
            require(lookup(0, start + std::chrono::seconds(30)), "pool shares timeout");
            require(!lookup(last, start + std::chrono::seconds(51)), "pool must expire");
        }
    }
    {
        tuntom::LruCache<int, int, std::hash<int>> cache(256, std::chrono::seconds(20));
        const auto now = Clock::now();
        for (int i = 0; i < 256; ++i) cache.put(i, i, now);
        cache.flush();
        int value = -1;
        require(cache.size() == 0 && !cache.get(255, value, now), "logical flush");
        cache.put(255, 999, now); // Revive an entry beyond the reclamation batch.
        require(cache.size() == 1 && cache.get(255, value, now) && value == 999, "revive stale entry");
        for (int i = 300; i < 600; ++i) cache.put(i, i, now);
        require(cache.size() == 256 && !cache.get(255, value, now), "post-flush capacity");
        require(cache.evictions() == 45 && cache.expirations() == 0, "flush preserves accounting");
        cache.flush(); cache.flush(); cache.reclaim();
        require(cache.size() == 0 && !cache.get(599, value, now), "repeated flush");
    }
    bool invalid_bits = false;
    try {
        ExitAdapterRoutes invalid(1, 1, std::chrono::seconds(1), std::chrono::seconds(1), false, 17);
    } catch (const std::invalid_argument&) { invalid_bits = true; }
    require(invalid_bits, "invalid prefix length accepted");
    std::cout << "PASS: exit adapter IPv4/IPv6 learning, fallback, expiry, LRU and strict L4\n";
}
