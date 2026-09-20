#include "../src/via/switch.hpp"
#include "../src/via/adapter.hpp"
#include "../src/divert/flows.hpp"
#include "../src/divert/paths.hpp"
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
    std::size_t path = 0;
    check(routes.learn(flow, context, true, now, 3) == divert::Learn::ok, "learn SYN");
    check(routes.lookup(reverse_key(flow), true, now, response, &path) && path == 3, "generated SYNACK has context and path before upstream SYNACK");
    codec.onward(response, 0);
    const auto& reply = std::get<Envelope>(response.value);
    check(reply.reverse && reply.action == onward && reply.step == 1, "direction from client TUN only");
    codec.onward(context, 1);
    check(!std::get<Envelope>(context.value).reverse, "direction from server TUN only");
    env.reverse = true; check(Codec::attach(env, labels) && codec.receive(labels, 1, context), "reverse OFFER");
    check(routes.learn(reverse_key(flow), context, false, now, 7) == divert::Learn::ok, "reverse ingress migrates transport path");
    check(routes.lookup(flow, false, now, response, &path) && path == 7, "forward output uses migrated path");
    check(routes.lookup(reverse_key(flow), true, now, response, &path) && path == 7, "reverse output uses migrated path");
    auto conflict = context; ++std::get<Envelope>(conflict.value).chain;
    check(routes.learn(reverse_key(flow), conflict, false, now, 9) == divert::Learn::conflict, "conflicting chain rejected");
    check(routes.lookup(flow, false, now, response, &path) && path == 7, "conflict cannot replace path");
    divert::BasicRoutes<AdapterContext> migrated(10, std::chrono::seconds(60), true);
    check(migrated.learn(reverse_key(flow), context, false, now) == divert::Learn::ok, "hash migration may start with reverse packet");
    check(migrated.lookup(reverse_key(flow), true, now, response), "reverse-first flow orientation");
}
static void paths() {
    using divert::adapter_paths;
    auto single = adapter_paths("/tmp/relay", {}, "smith#0", "in", "out");
    check(single.size() == 1 && single[0].input == "in~via:c:smith#0", "single-path CLI unchanged");
    auto multi = adapter_paths("", {"west=/tmp/a", "east=/tmp/b"}, "smith#0", "in", "out");
    check(multi.size() == 2 && multi[0].input == "in.west~via:c:smith#0#path-west" &&
        multi[0].output == "out.west~via:s:smith#0#path-west", "stable paired registration");
    auto split = adapter_paths("", {"west=/tmp/a"}, "smith#0", "in", "out", true);
    check(split[0].input == "in.west~via:c:smith#0" && split[0].output == "out.west~via:s:smith#0",
        "split workers share proxy identity, with distinct attachments");
    auto reordered = adapter_paths("", {"east=/tmp/b", "west=/tmp/a"}, "smith#0", "in", "out");
    check(reordered[1].input == multi[0].input, "path identity independent of argument order");
    for (const auto& values : std::vector<std::vector<std::string>>{{"bad"}, {"=/tmp/a"}, {"a="},
            {"a=/tmp/a", "a=/tmp/b"}, {"a=/tmp/a", "b=/tmp/a"}, {"a.b=/tmp/a"},
            {std::string(64, 'a') + "=/tmp/a"}, std::vector<std::string>(17, "a=/tmp/a")}) {
        bool threw = false;
        try { adapter_paths("", values, "smith#0", "in", "out"); } catch (const std::runtime_error&) { threw = true; }
        check(threw, "invalid path specification rejected before opening TUNs");
    }
    for (bool legacy : {false, true}) {
        bool threw = false;
        try { adapter_paths(legacy ? "" : "/tmp/old", {"a=/tmp/a"}, legacy ? "" : "smith#0", "in", "out"); }
        catch (const std::runtime_error&) { threw = true; }
        check(threw, "relay paths require VIA and cannot mix with switch socket");
    }
}
int main() {
    try { codec(); parser(); adapter(); paths(); std::cout << "VIA codec, parser, paths and adapter handshake contexts: OK\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
