#include "../src/session.hpp"
#include "../src/adapter/lru_cache.hpp"
#include "../src/control_socket.hpp"
#include <cstdlib>
#include <iostream>
#include <new>

// Fail each individual allocation in turn, then retry against the same state.
static long fail_after = -1;
static bool allocation_failed = false;
static long live_allocations = 0;
// Keep replacement allocation functions out of caller optimization; otherwise
// GCC diagnoses their intentional malloc/free implementation as mismatched new/delete.
[[gnu::noinline]] void* operator new(std::size_t size) {
    if (fail_after == 0) {
        fail_after = -1;
        allocation_failed = true;
        throw std::bad_alloc();
    }
    if (fail_after > 0) --fail_after;
    void* memory = std::malloc(size ? size : 1);
    if (not memory) throw std::bad_alloc();
    ++live_allocations;
    return memory;
}
[[gnu::noinline]] void operator delete(void* memory) noexcept {
    if (memory) { --live_allocations; std::free(memory); }
}
[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept { ::operator delete(memory); }

static int random_error = 0;
static int random_fail_after = -1;
static unsigned random_calls = 0;
extern "C" ssize_t __real_getrandom(void*, size_t, unsigned);
extern "C" ssize_t __wrap_getrandom(void* output, size_t size, unsigned flags) {
    ++random_calls;
    if (flags != GRND_NONBLOCK) std::abort();
    if (random_fail_after == 0) { errno = random_error; return -1; }
    if (random_fail_after > 0) --random_fail_after;
    return __real_getrandom(output, size, flags);
}

using namespace tuntom;
using namespace std::chrono_literals;
using SP = SessionProtocol;
using Wire = std::vector<std::uint8_t>;
static const auto start = SP::Time {} + 100s;
static constexpr std::int64_t wall = 100000;
static void require(bool ok, const char* reason) {
    if (not ok) throw std::runtime_error(reason);
}
static SP::Received receive(SP& session, const Wire& wire, SP::Time now) {
    Packet packet;
    Wire scratch;
    return session.receive(wire.data(), wire.size(), packet, scratch, now, wall);
}
static Wire data(SP& session) {
    Packet packet;
    packet.type = PacketType::data;
    packet.tunnel_id = 42;
    packet.original_length = 4;
    packet.payload = {0x45, 1, 2, 3};
    return session.encode(packet);
}
struct Pair {
    SP client, server;
    Pair(int suite) : client(42, {}, false, 1500, suite != 0, 300, suite == 2),
                      server(42, {}, true, 1500, suite != 0, 300, suite == 2) {}
    void handshake(SP::Time now) {
        const auto init = client.begin(now, wall);
        const auto response = receive(server, init, now).reply;
        const auto confirm = receive(client, response, now).reply;
        const auto ack = receive(server, confirm, now).reply;
        require(receive(client, ack, now).activated, "handshake completion");
    }
    void traffic(SP::Time now) {
        const auto wire = data(client);
        require(receive(server, wire, now).data, "client DATA after recovery");
        require(receive(server, wire, now).replay_drop, "replay history after recovery");
        require(receive(client, data(server), now).data, "server DATA after recovery");
    }
};

static void allocation_sweep(int suite, int stage) {
    unsigned failures = 0;
    for (long fault = 0; fault < 256; ++fault) {
        Pair pair(suite);
        pair.handshake(start);
        Wire init, response, confirm, ack;
        const auto now = start + 1s;
        if (stage > 0) init = pair.client.begin(now, wall);
        if (stage > 1) response = receive(pair.server, init, now).reply;
        if (stage > 2) confirm = receive(pair.client, response, now).reply;
        allocation_failed = false;
        fail_after = fault;
        try {
            if (stage == 0) init = pair.client.begin(now, wall);
            if (stage == 1) response = receive(pair.server, init, now).reply;
            if (stage == 2) confirm = receive(pair.client, response, now).reply;
            if (stage == 3) ack = receive(pair.server, confirm, now).reply;
        } catch (const std::bad_alloc&) {}
        fail_after = -1;
        if (allocation_failed) {
            ++failures;
            if (stage < 3) pair.traffic(now);
            else require(receive(pair.client, data(pair.server), now).data,
                         "old server session survived failed ACK construction");
        }
        // Replay the interrupted handshake step, without resetting either side.
        if (stage == 0 and init.empty()) init = pair.client.begin(now, wall);
        if (response.empty()) response = receive(pair.server, init, now).reply;
        if (confirm.empty()) confirm = receive(pair.client, response, now).reply;
        if (ack.empty()) ack = receive(pair.server, confirm, now).reply;
        require(receive(pair.client, ack, now).activated, "retry interrupted handshake step");
        pair.traffic(now);
        if (not allocation_failed) break;
        require(fault != 255, "allocation sweep bound");
    }
    require(failures != 0, "allocation injection was exercised");
}

static void random_failures() {
    for (int error : {EIO, EAGAIN, EINTR}) {
        // Fail exchange ID, client nonce, client PFS key, server nonce/key.
        for (int step = 0; step < 5; ++step) {
            Pair pair(2);
            pair.handshake(start);
            const auto now = start + 1s;
            Wire init;
            if (step >= 3) init = pair.client.begin(now, wall);
            random_error = error;
            random_fail_after = step < 3 ? step : step - 3;
            if (step < 3) require(pair.client.begin(now, wall).empty(), "defer client RNG failure");
            else require(receive(pair.server, init, now).reply.empty(), "defer server RNG failure");
            const auto calls = random_calls;
            for (int i = 1; i < 1000; ++i) {
                if (step < 3) require(pair.client.tick(now + std::chrono::milliseconds(i), wall).empty(),
                                      "client RNG backoff");
                else require(receive(pair.server, init, now + std::chrono::milliseconds(i)).reply.empty(),
                             "server RNG backoff");
            }
            require(random_calls == calls, "RNG retry must be rate limited");
            pair.traffic(now);
            random_fail_after = -1;
            if (step < 3) init = pair.client.tick(now + 1s, wall);
            const auto response = receive(pair.server, init, now + 1s).reply;
            const auto confirm = receive(pair.client, response, now + 1s).reply;
            const auto ack = receive(pair.server, confirm, now + 1s).reply;
            require(receive(pair.client, ack, now + 1s).activated, "RNG recovery handshake");
            pair.traffic(now + 1s);
        }
    }
    Pair pair(2);
    auto init = pair.client.begin(start, wall);
    auto response = receive(pair.server, init, start).reply;
    require(not receive(pair.client, response, start).reply.empty(), "waiting for ACK");
    random_error = EIO;
    random_fail_after = 0;
    require(pair.client.tick(start + 5s, wall).empty() and not pair.client.ready(),
            "expired candidate cannot survive RNG failure");
    require(receive(pair.client, response, start + 5s).reply.empty(), "expired RESPONSE rejected");
    random_fail_after = -1;
    pair.handshake(start + 6s);
    pair.traffic(start + 6s);
}

static void cache_allocations() {
    for (long fault = 0; fault < 8; ++fault) {
        LruCache<int, Wire, std::hash<int>> cache(2, 10s);
        Wire value(64, 7), found;
        cache.put(1, value, start);
        const auto live = live_allocations;
        allocation_failed = false;
        fail_after = fault;
        try { cache.put(2, value, start); } catch (const std::bad_alloc&) {}
        fail_after = -1;
        if (allocation_failed) require(live_allocations == live and cache.size() == 1,
                                       "failed LRU insertion leaked list/index state");
        require(cache.get(1, found, start) and found == value, "existing route survives OOM");
        cache.put(3, value, start);
        cache.put(4, value, start);
        require(cache.size() == 2 and not cache.get(1, found, start) and
                cache.get(3, found, start) and cache.get(4, found, start), "LRU after recovery");
        if (not allocation_failed) break;
    }
}

static void transmit_counter() {
    Pair pair(2);
    pair.handshake(start);
    auto before = data(pair.client);
    Packet packet;
    packet.type = PacketType::data;
    packet.original_length = 32;
    packet.payload.resize(32);
    fail_after = 0;
    try { pair.client.encode(packet); } catch (const std::bad_alloc&) {}
    fail_after = -1;
    auto after = data(pair.client);
    require((load_be64(after.data() + 1) & SP::counter_mask) >=
            (load_be64(before.data() + 1) & SP::counter_mask) + 2,
            "failed encryption must not roll back the nonce counter");
}

static void reassembly_allocations() {
    for (long fault = 0; fault < 16; ++fault) {
        ReassemblyMetrics metrics;
        {
            Reassembler reassembly(1500, &metrics);
            Packet fragment;
            fragment.message_id = 1;
            fragment.original_length = 100;
            fragment.payload.resize(50, 7);
            Wire complete;
            allocation_failed = false;
            fail_after = fault;
            try { reassembly.accept(fragment, complete, nullptr, start); }
            catch (const std::bad_alloc&) {}
            fail_after = -1;
            require(metrics.active_entries <= 1 and metrics.active_bytes == metrics.active_entries * 100,
                    "reassembly pool accounting after failed insertion");
            if (allocation_failed) reassembly.accept(fragment, complete, nullptr, start);
            fragment.fragment_offset = 50;
            require(reassembly.accept(fragment, complete, nullptr, start) and complete == Wire(100, 7),
                    "reassembly continues after allocation failure");
        }
        require(metrics.active_entries == 0 and metrics.active_bytes == 0, "reassembly gauges after destruction");
        if (not allocation_failed) break;
    }
}

static void control_allocation_failure() {
    char directory[] = "/tmp/tuntom-control-oom.XXXXXX";
    require(::mkdtemp(directory) != nullptr, "control test directory");
    const auto path = std::string(directory) + "/control";
    {
        ControlSocket control(path);
        const int peer = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        require(peer >= 0, "control peer socket");
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        require(::connect(peer, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "connect control test peer");
        require(::send(peer, "show stats", 10, MSG_NOSIGNAL) == 10, "control request");
        try {
            control.handle([]() -> std::string { throw std::bad_alloc(); });
            require(false, "allocation error must reach runtime accounting");
        } catch (const std::bad_alloc&) {}
        char buffer[64] {};
        const auto size = ::recv(peer, buffer, sizeof(buffer), 0);
        require(size > 0 and std::string(buffer, static_cast<std::size_t>(size)) == "error=out_of_memory\n",
                "allocation-free control error response");
        require(::recv(peer, buffer, sizeof(buffer), 0) == 0, "control client FD closed after exception");
        ::close(peer);
    }
    ::rmdir(directory);
}

int main() {
    for (int suite = 0; suite < 3; ++suite)
        for (int stage = 0; stage < 4; ++stage) allocation_sweep(suite, stage);
    random_failures();
    cache_allocations();
    reassembly_allocations();
    transmit_counter();
    control_allocation_failure();
    std::cout << "PASS: handshake/LRU/reassembly allocation failures, RNG backoff, nonce safety and control cleanup\n";
}
