#include "../src/divert/flows.hpp"
#include "../src/divert/switch.hpp"
#include <iostream>

using namespace tuntom;
using namespace tuntom::divert;
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static Stack stack(std::initializer_list<std::uint64_t> values) {
    Stack result; for (auto v : values) require(result.push(v), "stack capacity"); return result;
}
static PacketInfo flow(unsigned port = 1000, unsigned protocol = 6, unsigned flags = 2) {
    PacketInfo p; p.flow.version = 4; p.flow.source[0] = 10; p.flow.destination[0] = 20;
    p.flow.protocol = static_cast<std::uint8_t>(protocol);
    p.flow.source_port = static_cast<std::uint16_t>(port); p.flow.destination_port = 80;
    p.flags = flags; return p;
}
static void codec_test() {
    Codec c("hTX");
    Stack labels;
    require(c.offer(stack({17,42}), 123, labels) && labels.size == 8, "two original labels fit");
    require(labels.values[2] == text_label("hTX4"), "ASCII body length excludes cookie and DVRT");
    Envelope env;
    require(c.split(labels, env) && env.present && env.origin() == 123 && env.original() == stack({17,42}), "round trip");
    require(!c.offer(stack({1,2,3}), 123, labels), "overflow rejected");
    require(!c.offer(stack({1}), 0, labels), "zero origin rejected");
    require(c.offer(stack({17}), 123, labels), "one label fits");
    // The envelope may be before or between ordinary labels, not only a suffix.
    auto moved = stack({labels.values[1], labels.values[2], 123, 0, 17, 17});
    require(c.split(moved, env) && env.base == stack({17}), "non-fixed marker position");
    auto broken = moved; broken.values[0] = text_label("hTX9");
    require(!c.split(broken, env), "truncated body rejected");
    broken = moved; broken.values[3] = 4;
    require(!c.split(broken, env), "invalid action rejected");
    broken = moved; broken.values[2] = 0;
    require(!c.split(broken, env), "invalid origin rejected");
    auto foreign = stack({17, text_label("abc2"), text_label("DVRT"), 123, 0});
    require(c.split(foreign, env) && !env.present && env.base == foreign, "foreign cookie opaque");
    for (const auto* invalid : {"ab", "abcd", "a1c"}) {
        bool threw = false; try { Codec bad(invalid); } catch (const std::exception&) { threw = true; }
        require(threw, "cookie validation");
    }
}
static void admission_test() {
    Admission a(8);
    auto old = flow(1000, 6, 16), fresh = flow(1001);
    require(a.classify(old, 0) == AdmissionResult::bypass, "old TCP bypass");
    require(a.classify(fresh, 0) == AdmissionResult::proxy, "new SYN diverted");
    fresh.flags = 16;
    require(a.classify(fresh, 1) == AdmissionResult::proxy, "new TCP data stays diverted");
    fresh.flow = reverse_key(fresh.flow); fresh.flags = 18;
    require(a.classify(fresh, 2) == AdmissionResult::proxy, "bidirectional admission key");
    require(a.classify(flow(1002,6,16), 3599.99) == AdmissionResult::bypass, "last learning instant");
    require(a.classify(flow(1003,6,16), 3600) == AdmissionResult::proxy, "no TCP learning after hour");
    require(a.classify(old, 86399.99) == AdmissionResult::bypass, "existing lasts day");
    require(a.classify(old, 86400) == AdmissionResult::proxy, "TCP negative set released");
    Admission u(8); auto udp = flow(1000,17);
    require(u.classify(udp, 59.99) == AdmissionResult::bypass, "UDP learns for minute");
    require(u.classify(flow(1001,17), 60) == AdmissionResult::proxy, "new UDP after minute");
    require(u.classify(udp, 3599.99) == AdmissionResult::bypass, "old UDP bypass");
    require(u.classify(udp, 3600) == AdmissionResult::proxy, "UDP negative set released");
    Admission small(1);
    require(small.classify(flow(), 0) == AdmissionResult::proxy, "first admission slot");
    require(small.classify(flow(1001), 0) == AdmissionResult::full, "no admission eviction");
    require(small.classify(flow(1001), 3600) == AdmissionResult::proxy, "admission full clears at boundary");
}
static void routes_test() {
    const auto now = Clock::time_point{};
    Codec c("hTX"); Stack labels; Envelope env, got;
    require(c.offer(stack({17,42}), 123, labels) && c.split(labels, env), "test envelope");
    Routes r(1, std::chrono::seconds(60));
    auto p = flow().flow;
    require(r.learn(p, env, true, now) == Learn::ok, "learn forward");
    require(r.lookup(reverse_key(p), true, now, got) && got.original() == stack({17,42}), "early proxy SYN/ACK context");
    require(!r.lookup(p, true, now, got), "wrong TUN direction rejected");
    auto response = env; response.body.values[1] = onward; response.base = stack({99,42});
    require(r.learn(reverse_key(p), response, false, now) == Learn::ok, "learn exit rewrite");
    require(r.lookup(p, false, now, got) && got.base == stack({17,42}), "server rewrite never corrupts forward context");
    require(r.lookup(reverse_key(p), true, now, got) && got.base == stack({99,42}), "directional return context");
    auto wrong = env; wrong.body.values[0] = 999;
    require(r.learn(p, wrong, true, now) == Learn::conflict, "overlapping origin rejected");
    require(r.learn(flow(1001).flow, env, true, now) == Learn::full, "live entries not evicted");
    require(r.lookup(p, false, now + std::chrono::seconds(59), got), "refresh last activity");
    r.maintain(now + std::chrono::seconds(60));
    require(r.lookup(p, false, now + std::chrono::seconds(60), got), "active route survives maintenance");
    require(!r.lookup(p, false, now + std::chrono::seconds(120), got), "idle route expires");
    r.maintain(now + std::chrono::seconds(120));
    require(r.learn(flow(1001).flow, env, true, now + std::chrono::seconds(120)) == Learn::ok, "expired capacity reusable");
}
static void packet_test() {
    std::array<std::uint8_t, 80> p{}; PacketInfo result;
    p[0] = 0x45; p[3] = 40; p[9] = 6; p[32] = 0x50; p[33] = 2;
    require(packet_info(p.data(), 40, result) && result.flags == 2, "IPv4 SYN");
    p[6] = 0x20;
    require(!packet_info(p.data(), 40, result), "first fragment rejected");
    p[6] = 0; p[32] = 0x40;
    require(!packet_info(p.data(), 40, result), "short TCP header rejected");
    p = {}; p[0] = 0x60; p[5] = 28; p[6] = 0; p[40] = 6; p[60] = 0x50; p[61] = 18;
    require(packet_info(p.data(), 68, result) && result.flags == 18 && result.flow.version == 6, "IPv6 with hop-by-hop header");
    require(!packet_info(p.data(), 67, result), "truncated IPv6 rejected");
    p = {}; p[0] = 0x45; p[3] = 28; p[9] = 17; p[25] = 8;
    require(packet_info(p.data(), 28, result), "complete UDP");
    p[25] = 7; require(!packet_info(p.data(), 28, result), "invalid UDP length");
}
int main() {
    try { codec_test(); admission_test(); routes_test(); packet_test();
        std::cout << "divert framing, admission, directional contexts, capacity and IP validation: OK\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
