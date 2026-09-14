#include "../src/packet_classifier.hpp"
#include "../src/adapter/exit_adapter.hpp"
#include <iostream>
#include <stdexcept>

using namespace tuntom;
using Bytes = std::vector<std::uint8_t>;
using Labels = std::vector<std::uint64_t>;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
void put16(Bytes& packet, std::size_t at, unsigned value) {
    packet[at] = static_cast<std::uint8_t>(value >> 8);
    packet[at + 1] = static_cast<std::uint8_t>(value);
}
Bytes ip4(std::uint8_t proto = 6, unsigned source = 12345, unsigned dest = 443) {
    Bytes packet(40); packet[0] = 0x45; put16(packet, 2, 40); packet[9] = proto;
    packet[12] = 10; packet[15] = 7; packet[16] = 192; packet[18] = 2; packet[19] = 200;
    put16(packet, 20, source); put16(packet, 22, dest);
    return packet;
}
Bytes ip6(std::uint8_t proto = 17, unsigned source = 65535, unsigned dest = 53) {
    Bytes packet(64); packet[0] = 0x60; put16(packet, 4, 24); packet[6] = 0;
    ::inet_pton(AF_INET6, "2001:db8::1", packet.data() + 8);
    ::inet_pton(AF_INET6, "2001:db8:1::1234", packet.data() + 24);
    packet[40] = proto; put16(packet, 48, source); put16(packet, 50, dest);
    return packet;
}
auto classifier(const std::string& body) { return PacketClassifier::parse("format 1\n" + body); }
bool matches(PacketClassifier& c, const Bytes& packet, const Labels& labels) {
    const auto* found = c.classify(packet.data(), packet.size());
    return found && *found == labels;
}
void rejects(const std::string& body) {
    bool failed = false;
    try { (void)classifier(body); } catch (const std::runtime_error&) { failed = true; }
    require(failed, body.c_str());
}

int main() {
    auto c = classifier("classify ip4 src 10.0.0.7/32 dst 192.0.2.128/25 proto tcp sport <1024,65535> dport 443 to [\"ABCD\",0xff,b1000]\n"
                        "classify ip6 src 2001:db8::/32 dst 2001:db8:1::1234 proto udp dport 53 to [6,53]\n"
                        "classify proto icmp to 1\nclassify ip6 proto icmp6 to 58\n"
                        "classify to [0]\n");
    require(matches(c, ip4(), {UINT64_C(0x4142434400000000),255,8}), "IPv4 tuple and label literals");
    require(matches(c, ip4(6,1024), {UINT64_C(0x4142434400000000),255,8}), "source range lower boundary");
    require(matches(c, ip4(6,65535), {UINT64_C(0x4142434400000000),255,8}), "source range upper boundary");
    require(matches(c, ip4(6,1023), {0}), "source port outside range");
    require(matches(c, ip4(17), {0}), "protocol mismatch");
    require(matches(c, ip4(6,12345,80), {0}), "destination port mismatch");
    auto outside = ip4(); outside[19] = 127;
    require(matches(c, outside, {0}), "non-byte-aligned IPv4 CIDR");
    auto wrong_source = ip4(); ++wrong_source[15];
    require(matches(c, wrong_source, {0}), "exact source address");
    require(matches(c, ip6(), {6,53}), "IPv6 extension header tuple");
    require(matches(c, ip4(1), {1}) && matches(c, ip6(58), {58}), "ICMP protocols without ports");
    auto options = ip4(); options.insert(options.begin() + 20, 4, 0);
    options[0] = 0x46; put16(options, 2, 44);
    require(matches(c, options, {UINT64_C(0x4142434400000000),255,8}), "IPv4 options shift transport offset");

    auto prefix = classifier("classify src 2001:db8::/33 to 1\nclassify src 0.0.0.0/0 to 4\nclassify to 0");
    require(matches(prefix, ip6(), {1}) && matches(prefix, ip4(), {4}), "CIDR family isolation");
    auto boundary6 = ip6(); boundary6[12] = 0x80;
    require(matches(prefix, boundary6, {0}), "non-byte-aligned IPv6 CIDR");

    auto fragments = classifier("classify proto tcp dport 443 to 443\nclassify proto tcp to 6\n"
                                "classify proto udp dport 53 to 53\nclassify proto udp to 17\nclassify to 0");
    auto first = ip4(); put16(first, 6, 0x2000);
    require(matches(fragments, first, {443}), "first IPv4 fragment contains ports");
    auto later = ip4(); put16(later, 6, 1);
    require(matches(fragments, later, {6}), "later IPv4 fragment must not read payload as ports");
    auto fragment6 = ip6(); fragment6[6] = 44;
    require(matches(fragments, fragment6, {53}), "first IPv6 fragment contains ports");
    put16(fragment6, 42, 8);
    require(matches(fragments, fragment6, {17}), "later IPv6 fragment can identify UDP without ports");
    fragment6[40] = 60;
    require(matches(fragments, fragment6, {0}), "later IPv6 fragment cannot traverse missing extensions");
    auto short_l4 = ip4(); short_l4.resize(23); put16(short_l4, 2, 23);
    require(matches(fragments, short_l4, {6}), "incomplete port tuple uses L3 only");
    auto ah = ip6(); ah[6] = 51;
    require(matches(fragments, ah, {53}), "IPv6 AH header traversal");

    auto all = classifier("classify to [1,2,3,4,5,6,7,0xffffffffffffffff]");
    require(matches(all, ip4(), {1,2,3,4,5,6,7,UINT64_MAX}), "maximum output stack");
    for (const auto size : {0U,1U,19U,39U}) {
        auto truncated = ip4(); truncated.resize(size);
        require(!all.classify(truncated.data(), truncated.size()), "truncated IP packet matched catch-all");
    }
    auto invalid = ip4(); invalid[0] = 0x44;
    require(!all.classify(invalid.data(), invalid.size()), "invalid IPv4 IHL");
    invalid = ip6(); invalid.resize(41); put16(invalid, 4, 1);
    require(!all.classify(invalid.data(), invalid.size()), "truncated IPv6 extension");
    auto chain = ip6(); chain.resize(40 + 9 * 8); put16(chain, 4, 9 * 8);
    for (std::size_t i = 40; i < chain.size(); i += 8) { chain[i] = 0; chain[i + 1] = 0; }
    require(!all.classify(chain.data(), chain.size()), "bounded IPv6 extension chain");
    PacketClassifier disabled;
    require(!disabled.classify(nullptr,0), "disabled classifier");
    auto no_match = classifier("classify proto udp to 17");
    require(!no_match.classify(ip4().data(),40), "missing rule leaves fallback to caller");
    std::ostringstream stats; no_match.write_stats(stats);
    require(stats.str().find("classifier_misses=1\n") != std::string::npos, "classification miss counter");

    auto raw = classifier("classify proto tcp dport 443 to [17,99]\nclassify ip6 proto udp to [6,53]");
    auto parsed = raw;
    for (const auto& input : {ip4(), ip6(), ip4(6,12345,80), later, invalid, Bytes{}}) {
        const auto* from_packet = raw.classify(input.data(),input.size());
        ParsedIpFlow flow;
        const Labels* from_flow = nullptr;
        if (parse_ip_flow(input.data(),input.size(),flow)) from_flow = parsed.classify(flow);
        else parsed.record_parse_error();
        require((from_packet == nullptr) == (from_flow == nullptr), "packet/flow classification decision differs");
        if (from_packet) require(*from_packet == *from_flow, "packet/flow label stacks differ");
    }
    std::ostringstream raw_stats, parsed_stats;
    raw.write_stats(raw_stats); parsed.write_stats(parsed_stats);
    require(raw_stats.str() == parsed_stats.str(), "packet/flow classification counters differ");
    ParsedIpFlow valid_flow; const auto valid_packet = ip4();
    require(parse_ip_flow(valid_packet.data(),valid_packet.size(),valid_flow), "parse disabled-classifier fixture");
    require(!disabled.classify(valid_flow), "disabled classifier accepted parsed flow");
    disabled.record_parse_error();
    std::ostringstream disabled_stats; disabled.write_stats(disabled_stats);
    require(disabled_stats.str().find("classifier_misses=0\n") != std::string::npos &&
            disabled_stats.str().find("classifier_parse_errors=0\n") != std::string::npos,
            "disabled classifier counted shared parsing or classification");

    // Classification never learns a route; existing reverse entries retain
    // their complete stack before the caller considers classification.
    ExitAdapterRoutes routes(4,4,std::chrono::seconds(30),std::chrono::seconds(30));
    auto request = ip4(); const Labels learned{17,99,42}; Labels found;
    require(routes.learn(request.data(),request.size(),learned), "learn reverse route");
    auto reply = request;
    std::swap_ranges(reply.begin()+12,reply.begin()+16,reply.begin()+16);
    std::swap_ranges(reply.begin()+20,reply.begin()+22,reply.begin()+22);
    require(routes.lookup(reply.data(),reply.size(),found) && found == learned, "reverse cache stack");
    require(matches(all, request, {1,2,3,4,5,6,7,UINT64_MAX}) && routes.l4_size() == 1, "classifier is stateless");

    for (const auto* body : {"classify ip4 ip6 to 1", "classify src 10.0.0.0/33 to 1",
        "classify dst ::/129 to 1", "classify src hostname to 1", "classify ip4 dst ::1 to 1",
        "classify src ::1 dst 1.2.3.4 to 1", "classify proto 256 to 1", "classify proto -1 to 1",
        "classify proto icmp dport 8 to 1", "classify sport <2,1> to 1", "classify dport 65536 to 1",
        "classify dport 1 dport 2 to 1", "classify nope x to 1", "classify to []", "classify to [1,...]",
        "classify to [*]", "classify to [&16]", "classify to [<1,2>]", "classify to [1,2,3,4,5,6,7,8,9]",
        "classify to [\"ABCDEFGHI\"]", "classify to 1 trailing", "classify proto tcp", "format 1"}) rejects(body);
    std::cout << "PASS: stateless IPv4/IPv6 classifier, prefixes, ports, fragments, literals and validation\n";
}
