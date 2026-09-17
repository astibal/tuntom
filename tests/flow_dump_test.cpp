#include "../src/adapter/exit_adapter.hpp"
#include "../src/control_socket.hpp"
#include "../src/divert/flows.hpp"
#include "../src/via/adapter.hpp"
#include <iostream>

using namespace tuntom;
using namespace std::chrono;
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static void contains(const std::string& text, const std::string& part) {
    require(text.find(part) != std::string::npos, part.c_str());
}
static FlowKey key() {
    FlowKey k; k.version = 4; k.source[0] = 10; k.source[3] = 1;
    k.destination[0] = 10; k.destination[3] = 2;
    k.protocol = 6; k.source_port = 12345; k.destination_port = 443; return k;
}
static via::AdapterContext context(std::uint64_t label) {
    via::Envelope e; e.base.push(label); e.saved.push(17); e.saved.push(UINT64_MAX);
    e.present = true; e.cookie = 0x616263; e.chain = 7; e.origin_id = 9; return {e};
}
static void caches() {
    const auto now = steady_clock::now();
    LruCache<FlowKey, int, FlowHash> cache(2, seconds(10));
    auto a = key(), b = a, c = a; ++b.source_port; c.source_port += 2;
    cache.put(a, 1, now); cache.put(b, 2, now);
    unsigned count = 0;
    cache.visit_live(now + seconds(5), [&](auto&, auto&, auto) { ++count; });
    require(count == 2 && cache.evictions() == 0 && cache.expirations() == 0, "read-only visit");
    cache.put(c, 3, now + seconds(5)); int value;
    require(!cache.get(a, value, now + seconds(5)), "dump must not change LRU");
    count = 0;
    cache.visit_live(now + seconds(11), [&](auto&, auto&, auto) { ++count; });
    require(count == 1 && cache.expirations() == 0, "dump filters expiry without accounting changes");

    std::uint8_t packet[24]{}; packet[0] = 0x45; packet[3] = 24; packet[9] = 17;
    packet[12] = 10; packet[15] = 1; packet[16] = 10; packet[19] = 2;
    packet[20] = 0x30; packet[21] = 0x39; packet[22] = 1; packet[23] = 0xbb;
    ExitAdapterRoutes routes(8, 8, seconds(10), seconds(20));
    require(routes.learn(packet, sizeof(packet), {17, UINT64_MAX}, now), "learn");
    auto dump = routes.dump_flows(now + seconds(5));
    contains(dump, "src=10.0.0.2 dst=10.0.0.1 protocol=17 src_port=443 dst_port=12345");
    contains(dump, "labels=[0x0000000000000011,0xffffffffffffffff]");
    contains(dump, "idle_ms=5000"); contains(dump, "flow_count=2\n");
    contains(routes.dump_flows(now + seconds(11)), "flow_count=1\n");
    contains(routes.dump_flows(now + seconds(21)), "flow_count=0\n");
    require(routes.l3_size() == 1 && routes.l4_size() == 1 && routes.l4_hits() == 0 && routes.l3_expirations() == 0, "dump preserves caches and counters");
    FlowDump v6; auto ipv6 = key(); ipv6.version = 6; ipv6.source = {}; ipv6.destination = {};
    ipv6.source[15] = 1; ipv6.destination[15] = 2;
    v6.add("l4", ipv6, [](auto&) {}); contains(v6.finish(), "src=::1 dst=::2");
}
static void divert_routes() {
    using namespace divert;
    const auto now = Clock::now(); auto flow = key();
    BasicRoutes<via::AdapterContext> routes(4, seconds(10));
    require(routes.learn(flow, context(17), true, now) == Learn::ok, "learn client");
    require(routes.learn(reverse_key(flow), context(42), false, now, 3) == Learn::ok, "learn server");
    FlowDump dump; routes.dump_flows(dump, now + seconds(2)); auto text = dump.finish();
    contains(text, "path=3 client_labels=[0x0000000000000011]");
    contains(text, "server_labels=[0x000000000000002a]");
    contains(text, "client_saved_labels=[0x0000000000000011,0xffffffffffffffff]");
    FlowDump expired; routes.dump_flows(expired, now + seconds(10)); contains(expired.finish(), "flow_count=0\n");
    via::AdapterContext found;
    require(!routes.lookup(flow, false, now + seconds(10), found), "dump must not refresh routes");
    Admission admission(4); PacketInfo packet{flow, 16}; admission.classify(packet, 0);
    FlowDump learning; admission.dump_flows(learning, 1); contains(learning.finish(), "labels=unknown");
    FlowDump old; admission.dump_flows(old, 86400); contains(old.finish(), "flow_count=0\n");
    Routes legacy(4, seconds(10)); Envelope env; env.base.push(123); env.body.push(1); env.body.push(0); env.body.push(123);
    require(legacy.learn(flow, env, true, now) == Learn::ok, "legacy learn");
    FlowDump legacy_dump; legacy.dump_flows(legacy_dump, now);
    contains(legacy_dump.finish(), "client_divert_body=[0x0000000000000001,0x0000000000000000,0x000000000000007b]");


}
static void protocol() {
    const auto path = "/tmp/flow-dump-control-" + std::to_string(::getpid());
    ControlSocket server(path);
    const auto exchange = [&](const std::string& command, const std::string& content, bool failure = false, bool defaults = false) {
        const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0); require(fd >= 0, "socket");
        sockaddr_un address{}; address.sun_family = AF_UNIX; std::copy(path.begin(), path.end(), address.sun_path);
        require(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect");
        require(::send(fd, command.data(), command.size(), 0) == static_cast<ssize_t>(command.size()), "send");
        std::string response, header; auto deadline = steady_clock::now() + seconds(4);
        while (steady_clock::now() < deadline) {
            auto stats = [] { return std::string("stats=ok\n"); };
            if (defaults) server.handle(stats);
            else server.handle(stats, [](auto&, auto&) { return std::string("rules=ok\n"); }, [&]() -> std::string {
                if (failure) throw std::runtime_error("snapshot failed");
                return content;
            });
            char bytes[control_chunk_size]; auto n = ::recv(fd, bytes, sizeof(bytes), MSG_DONTWAIT | MSG_TRUNC);
            if (n == 0) break;
            if (n < 0) { require(errno == EAGAIN || errno == EWOULDBLOCK, "recv"); continue; }
            require(n <= static_cast<ssize_t>(sizeof(bytes)), "untruncated chunk");
            if (header.empty()) header.assign(bytes, static_cast<std::size_t>(n));
            else response.append(bytes, static_cast<std::size_t>(n));
        }
        ::close(fd);
        if (command == "show stats") { require(header == "stats=ok\n", "legacy stats"); return; }
        const auto expected = defaults ? FlowDump("none").finish() : failure ? "snapshot failed\n" : content;
        require(header == std::string(failure ? "ERROR " : "OK ") + std::to_string(expected.size()) + "\n", "framed header");
        require(response == expected, "complete framed response");
    };
    exchange("show flows", std::string(control_max_body + 12345, 'x'));
    exchange("show flows\n", "", true);
    exchange("show flows", "", false, true);
    exchange("show stats", "");
    require(control_length("268435456", control_max_flows) == control_max_flows, "flow response limit");
    for (const std::string bad : {"1048577", "184467440737095516160", "-1", ""}) {
        bool failed = false; try { control_length(bad); } catch (const std::exception&) { failed = true; }
        require(failed, "rules limit and overflow rejection");
    }
}
int main() {
    caches(); divert_routes(); protocol();
    std::cout << "PASS: flow snapshots, labels, expiry and framed control transport\n";
}
