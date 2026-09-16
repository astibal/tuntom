#include "../src/via/switch.hpp"
#include "../src/via/adapter.hpp"
#include "../src/divert/flows.hpp"
#include <iostream>
using namespace tuntom;
using namespace tuntom::via;
static void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static Stack stack(std::initializer_list<std::uint64_t> values) {
    Stack s; for (auto value : values) check(s.push(value), "stack capacity"); return s;
}
static void codec() {
    Envelope env;
    env.base = env.saved = stack({17,42}); env.cookie = 0x685458; env.chain = 1; env.origin_id = 123;
    Stack labels;
    check(Codec::attach(env, labels) && labels.size == 7, "seven-label envelope");
    check(labels.values[2] == 0x6854585649410105ULL && labels.values[3] == 0x100000200ULL, "wire bit allocation");
    Envelope decoded;
    check(Codec::split(labels, decoded) && decoded.same_context(env), "round trip");
    auto broken = labels; broken.values[3] |= 0x10;
    check(!Codec::split(broken, decoded), "reserved CTX bits");
    broken = labels; broken.values[2] += 1;
    check(!Codec::split(broken, decoded), "length mismatch");
    broken = labels; broken.values[3] += 0x100;
    check(!Codec::split(broken, decoded), "saved count mismatch");
    broken = labels; broken.values[2] += 0x100;
    check(!Codec::split(broken, decoded), "unknown version");
    env.base = stack({1,2,3}); check(Codec::attach(env, labels) && labels.size == 8, "eight-label limit inclusive");
    env.saved = env.base; check(!Codec::attach(env, labels), "no stack truncation");
}
static std::string config() {
    return "format 3\nserial 1\nport edge id 123\nservice smithproxy {\n"
        " client-side proxy-in*\n server-side proxy-out*\n stickiness hash\n unavailable drop\n}\n"
        "service capture {\n client-side cap-in*\n server-side cap-out*\n stickiness failover\n"
        " instances [\"capture#1\",\"capture#0\"]\n unavailable pass\n}\n"
        "exit exit\nswitch edge,[17,42] to exit,[99,42] via [smithproxy,capture] allow bidir\n";
}
static void parser() {
    auto rules = parse_switch_ruleset(config());
    check(parse_switch_ruleset(rules->text())->text() == rules->text(), "canonical round trip");
    check(rules->statements.back().via.empty(), "bidir reverse cannot offer again");
    for (auto text : {"strict", "bidir"}) {
        auto bad = config(); auto at = bad.find("hash"); bad.replace(at, 4, text);
        bool threw = false; try { parse_switch_ruleset(bad); } catch (const std::runtime_error&) { threw = true; }
        check(threw, "removed stickiness rejected");
    }
    auto bad = config(); bad.replace(bad.find("smithproxy,capture"), 17, "unknown");
    bool threw = false; try { parse_switch_ruleset(bad); } catch (const std::runtime_error&) { threw = true; }
    check(threw, "unknown service rejected");
    Registration reg;
    check(registration(port_name("proxy-in0", "smithproxy#0", false), reg) && !reg.server && reg.instance == "smithproxy#0", "instance registration");
    check(!registration("x~via:c:", reg), "empty instance rejected");
}
static void adapter() {
    Envelope env; env.base = env.saved = stack({17,42}); env.cookie = 0x685458;
    env.chain = 1; env.origin_id = 123; env.step = 1;
    Stack labels; check(Codec::attach(env, labels), "offer encode");
    AdapterCodec codec(true, ""); AdapterContext context, response;
    check(codec.receive(labels, 0, context), "forward offer");
    divert::BasicRoutes<AdapterContext> routes(10, std::chrono::seconds(60), true);
    FlowKey flow{}; flow.version = 4; flow.protocol = 6; flow.source_port = 1000; flow.destination_port = 80;
    auto now = divert::Clock::time_point{};
    check(routes.learn(flow, context, true, now) == divert::Learn::ok, "learn SYN");
    check(routes.lookup(reverse_key(flow), true, now, response), "generated SYNACK has context before upstream SYNACK");
    codec.onward(response, 0);
    const auto& reply = std::get<Envelope>(response.value);
    check(reply.reverse && reply.action == onward && reply.step == 1, "direction from client TUN only");
    codec.onward(context, 1);
    check(!std::get<Envelope>(context.value).reverse, "direction from server TUN only");
    env.reverse = true; check(Codec::attach(env, labels) && codec.receive(labels, 1, context), "reverse OFFER");
    divert::BasicRoutes<AdapterContext> migrated(10, std::chrono::seconds(60), true);
    check(migrated.learn(reverse_key(flow), context, false, now) == divert::Learn::ok, "hash migration may start with reverse packet");
    check(migrated.lookup(reverse_key(flow), true, now, response), "reverse-first flow orientation");
}
int main() {
    try { codec(); parser(); adapter(); std::cout << "VIA codec, parser and adapter handshake contexts: OK\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
