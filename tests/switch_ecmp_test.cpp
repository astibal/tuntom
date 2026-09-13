#include "../src/switch_ecmp.hpp"
#include "../src/switch_routes.hpp"
#include "siphash_vectors.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace tuntom;

void check(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

void siphash_vectors_test() {
    using namespace linux_siphash;
    const siphash_key_t key{{0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL}};
    std::array<std::uint8_t, 72> input{};
    for (std::size_t alignment = 0; alignment < 8; ++alignment) {
        for (std::size_t i = 0; i < 64; ++i) input[alignment + i] = static_cast<u8>(i);
        for (std::size_t size = 0; size < 64; ++size)
            check(siphash(input.data() + alignment, size, &key) == siphash_vectors[size],
                  "Linux SipHash-2-4 reference vector / unaligned input");
    }
    check(siphash_1u64(0x0706050403020100ULL, &key) == siphash_vectors[8], "Fixed 8-byte hash");
    check(siphash_2u64(0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL, &key) == siphash_vectors[16],
          "Fixed 16-byte hash");
}

void routes_test() {
    SwitchRoutes rules;
    for (const auto *rule : {"*:1=default:1", "edge*:1=short:2", "edge-42*:1=long:3",
                              "edge-42_1:1=exact:4", "*:2=common*:5"}) add_switch_route(rules, rule);
    for (const auto &entry : std::vector<std::pair<std::string, std::string>>{
             {"internet", "default"}, {"edge-x", "short"}, {"edge-42", "long"},
             {"edge-420", "long"}, {"edge-42_1", "exact"}}) {
        const auto resolved = routes_for_port(rules, entry.first);
        check(resolved.at(1).port == entry.second && resolved.at(2).port == "common*", "Ingress precedence per label");
    }
    check(route_port_matches("*", "anything") && route_port_matches("edge-42*", "edge-42"), "Empty wildcard suffix");
    check(!route_port_matches("edge-42*", "edge-4") && !route_port_matches("edge-42", "edge-420"), "Anchored prefix");
    check(route_port_matches("a?[]", "a?[]") && !route_port_matches("a?[]", "abcd"), "Only star is special");
    for (const auto *rule : {"a**:1=b:2", "a*b:1=b:2", "a:1=b*c:2", "a:1=b**:2",
                             "a:-1=b:2", "a:1=b:18446744073709551616", "a:1=b:", "a:1=bad port:2"}) {
        bool rejected = false;
        try { add_switch_route(rules, rule); } catch (const std::exception &) { rejected = true; }
        check(rejected, "Malformed route accepted");
    }
    bool rejected = false;
    try { add_switch_route(rules, "*:0x1=other:7"); } catch (const std::exception &) { rejected = true; }
    check(rejected, "Duplicate numeric label accepted");
}

std::vector<std::uint8_t> packet(bool ipv6, std::uint16_t sport, bool reverse = false) {
    const std::size_t base = ipv6 ? 40 : 20;
    std::vector<std::uint8_t> p(base + 12, 0);
    p[0] = ipv6 ? 0x60 : 0x45;
    if (ipv6) { p[5] = 12; p[6] = 17; p[7] = 64; }
    else { p[3] = static_cast<std::uint8_t>(p.size()); p[8] = 64; p[9] = 17; }
    const auto src = ipv6 ? 8 : 12, dst = ipv6 ? 24 : 16;
    p[src] = reverse ? 11 : 10; p[dst] = reverse ? 10 : 11;
    const auto sp = reverse ? 443 : sport, dp = reverse ? sport : 443;
    p[base] = static_cast<std::uint8_t>(sp >> 8); p[base + 1] = static_cast<std::uint8_t>(sp);
    p[base + 2] = static_cast<std::uint8_t>(dp >> 8); p[base + 3] = static_cast<std::uint8_t>(dp);
    p[base + 5] = 12;
    return p;
}

SwitchFrameView view(const std::vector<std::uint8_t> &p) {
    static const std::uint8_t labels[8] = {0, 0, 0, 0, 0, 0, 0, 17};
    SwitchFrameView f;
    f.labels = labels; f.label_count = 1; f.payload = p.data(); f.payload_size = p.size();
    return f;
}

std::string choose(const SwitchFrameView &f, const std::vector<std::string> &ports) {
    EcmpSelector selector(f);
    std::string winner;
    for (const auto &port : ports)
        if (selector.consider(ecmp_port_identity(port), port)) winner = port;
    check(selector.multipath() == (ports.size() > 1), "Multipath counter");
    return winner;
}

void flows_test() {
    for (bool ipv6 : {false, true}) {
        auto p = packet(ipv6, 12345), reverse = packet(ipv6, 12345, true);
        check(ecmp_flow_hash(view(p)) == ecmp_flow_hash(view(reverse)), "L4 symmetry");
        auto changed = p; changed.back() = 99;
        check(ecmp_flow_hash(view(p)) == ecmp_flow_hash(view(changed)), "Payload affected flow");
        changed[ipv6 ? 6 : 9] = 6;
        check(ecmp_flow_hash(view(p)) != ecmp_flow_hash(view(changed)), "TCP/UDP domain");
        auto other = packet(ipv6, 12346);
        check(ecmp_flow_hash(view(p)) != ecmp_flow_hash(view(other)), "L4 ports ignored");
        p[ipv6 ? 6 : 9] = reverse[ipv6 ? 6 : 9] = 99;
        check(ecmp_flow_hash(view(p)) == ecmp_flow_hash(view(reverse)), "L3 symmetry");
        if (ipv6) {
            p[6] = reverse[6] = 44;
            p.insert(p.begin() + 40, 8, 0); reverse.insert(reverse.begin() + 40, 8, 0);
            p[5] = reverse[5] = 20; p[40] = reverse[40] = 17;
            p[43] = 1; reverse[43] = 8; // First (M) and later fragment.
        } else {
            p[9] = reverse[9] = 17; p[6] = 0x20; reverse[7] = 1;
        }
        check(ecmp_flow_hash(view(p)) == ecmp_flow_hash(view(reverse)), "First/later fragments disagree");
        auto truncated = packet(ipv6, 12345);
        truncated[ipv6 ? 5 : 3] = ipv6 ? 0 : 20; // Trailing bytes are outside IP length.
        ParsedIpFlow parsed;
        check(parse_ip_flow(truncated.data(), truncated.size(), parsed) && !parsed.has_l4,
              "Read transport ports beyond IP length");
    }
    std::vector<std::uint8_t> opaque{0x01, 0x02};
    const auto hash = ecmp_flow_hash(view(opaque)); opaque.back() = 77;
    check(ecmp_flow_hash(view(opaque)) == hash, "Opaque payload must keep label affinity");
    auto p = packet(false, 12345);
    check(choose(view(p), {}).empty() && choose(view(p), {"only"}) == "only", "Zero/one member");
    std::array<unsigned, 3> counts{};
    const std::vector<std::string> ports{"edge-42", "edge-42_1", "edge-42_2"};
    for (unsigned i = 0; i < 4096; ++i) {
        p = packet(i % 2 != 0, static_cast<std::uint16_t>(1024 + i));
        const auto winner = choose(view(p), ports);
        ++counts[static_cast<std::size_t>(std::find(ports.begin(), ports.end(), winner) - ports.begin())];
        check(winner == choose(view(p), {ports[2], ports[0], ports[1]}), "Registration order affected mapping");
        const auto reduced = choose(view(p), {ports[0], ports[2]});
        check(winner == ports[1] || reduced == winner, "Removal moved an unaffected flow");
        const auto expanded = choose(view(p), {ports[0], ports[1], ports[2], "edge-42_3"});
        check(expanded == winner || expanded == "edge-42_3", "Addition moved flow between old ports");
    }
    for (auto count : counts) check(count > 1000 && count < 1700, "Flow distribution is biased");
}

int main() {
    siphash_vectors_test(); routes_test(); flows_test();
    std::cout << "PASS: Linux SipHash vectors, route patterns, symmetric flows, fragments and stable ECMP\n";
}
