#include "../src/divert/shared_flows.hpp"
#include <iostream>
#include <sstream>
#include <sys/wait.h>

using namespace tuntom;
using namespace tuntom::divert;
using namespace std::chrono;
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
struct File {
    char path[64] = "/tmp/tuntom-shared-test-XXXXXX";
    File() { const int fd = ::mkstemp(path); require(fd >= 0, "temporary file"); ::close(fd); }
    ~File() { ::unlink(path); }
};
static PacketInfo flow(unsigned port = 1000, unsigned protocol = 6, unsigned flags = 2) {
    PacketInfo p; p.flow.version = 4; p.flow.source[0] = 10; p.flow.destination[0] = 20;
    p.flow.protocol = static_cast<std::uint8_t>(protocol);
    p.flow.source_port = static_cast<std::uint16_t>(port); p.flow.destination_port = 80; p.flags = flags; return p;
}
static via::AdapterContext context() {
    via::Envelope e; e.cookie = 0x685458; e.chain = 7; e.origin_id = 123; e.present = true;
    e.base.push(17); e.base.push(42); e.saved = e.base; return {e};
}
static std::string stats(SharedFlows& f) { std::ostringstream out; f.stats(out); return out.str(); }
template<class F> static void fails(F f) {
    bool threw = false; try { f(); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "invalid configuration accepted");
}
static void joined(pid_t pid) {
    int status = 0; require(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child failed");
}

namespace tuntom::divert {
// Fault injection into the actual transaction, including a torn value and
// damaged indexes. The child exits while owning the process-shared mutex.
struct SharedFlowsTest {
    static void crash(SharedFlows& f, FlowKey key, unsigned mode) {
        key = canonical(key);
        auto lock = f.routes_->lock(key);
        const auto index = lock.find(key); require(index != 0, "fault injection flow");
        if (mode == 0) {
            lock.begin(index);
            lock.nodes_[index - 1].value.client.origin_id = 999;
            lock.nodes_[index - 1].present = false;
            lock.state_.free = 0;
            lock.state_.counts[0] = 123456;
            f.routes_->heads(lock.state_)[0] = 123456;
        } else if (mode == 1) {
            auto value = lock.value(index); value.client.base.values[0] = 99;
            lock.update(index, value); // Committed but mutex has not been released.
        } else {
            // An interrupted insertion must restore the old free node.
            const auto free = lock.state_.free; require(free != 0, "free fault injection slot");
            lock.begin(free);
            lock.nodes_[free - 1] = lock.nodes_[index - 1];
            lock.state_.counts[0]++;
        }
        ::_exit(0);
    }
};
}

static void routes_and_lifecycle() {
    File file; auto now = Clock::now(); auto p = flow().flow; auto env = context(); via::AdapterContext got;
    {
        SharedFlows a(file.path, "group", "a", 1024, seconds(60), 1024, false);
        SharedFlows b(file.path, "group", "b", 1024, seconds(60), 1024, false);
        fails([&] { SharedFlows duplicate(file.path, "group", "a", 1024, seconds(60), 1024, false); });
        fails([&] { SharedFlows wrong(file.path, "other", "c", 1024, seconds(60), 1024, false); });
        fails([&] { SharedFlows wrong(file.path, "group", "c", 1024, seconds(61), 1024, false); });
        fails([&] { SharedFlows wrong(file.path, "group", "c", 1024, seconds(60), 1024, true); });
        fails([&] { SharedFlows wrong(file.path, "group", "c", 1025, seconds(60), 1024, false); });
        require(a.learn(p, env, true, now) == Learn::ok, "learn forward");
        require(b.lookup(reverse_key(p), true, now, got) && got.same_context(env), "early reply from another worker");
        require(!b.lookup(p, true, now, got), "wrong direction");
        auto response = env; auto& e = std::get<via::Envelope>(response.value); e.base.values[0] = 99; e.reverse = true;
        require(b.learn(reverse_key(p), response, false, now) == Learn::ok, "learn reverse context");
        require(a.lookup(reverse_key(p), true, now, got) && std::get<via::Envelope>(got.value).base == e.base, "shared reverse rewrite");
        require(b.lookup(p, false, now, got) && std::get<via::Envelope>(got.value).base.values[0] == 17, "forward context preserved");
        auto wrong = env; std::get<via::Envelope>(wrong.value).origin_id++;
        require(b.learn(p, wrong, true, now) == Learn::conflict, "origin conflict");
        require(b.learn(p, env, false, now) == Learn::conflict, "direction conflict");
        require(b.learn(reverse_key(flow(1001).flow), response, false, now) == Learn::ok, "reverse first learn");
        require(a.lookup(flow(1001).flow, false, now, got), "reverse first reply");
        require(!b.lookup(p, false, now + seconds(60), got), "idle expiry");
        require(a.learn(p, env, true, now + seconds(60)) == Learn::ok, "replace expired context");
        {
            SharedFlows restarted(file.path, "group", "c", 1024, seconds(60), 1024, false);
            require(restarted.lookup(p, false, now + seconds(60), got), "joining worker retains flow");
        }
        require(a.classify(flow(), now) == AdmissionResult::proxy && b.active(), "shared activation");
    }
    SharedFlows fresh(file.path, "new group", "a", 1, seconds(60), 1, false);
    require(!fresh.active() && !fresh.lookup(p, false, now, got), "last worker exit resets group");
    require(fresh.learn(p, env, true, now) == Learn::ok, "first capacity slot");
    require(fresh.learn(flow(1001).flow, env, true, now) == Learn::full, "no eviction at capacity");
    require(fresh.learn(flow(1001).flow, env, true, now + seconds(60)) == Learn::ok, "expired slot reused");
}
static void admission() {
    File file; auto now = Clock::now(); auto old = flow(1000, 6, 16), fresh = flow(1001), udp = flow(1000, 17);
    SharedFlows a(file.path, "admission", "a", 1024, seconds(60), 1024, true);
    SharedFlows b(file.path, "admission", "b", 1024, seconds(60), 1024, true);
    require(a.classify(old, now) == AdmissionResult::bypass, "existing TCP");
    require(a.classify(fresh, now) == AdmissionResult::proxy, "new TCP");
    fresh.flags = 16;
    require(b.classify(fresh, now + seconds(1)) == AdmissionResult::proxy, "shared positive admission");
    old.flow = reverse_key(old.flow);
    require(b.classify(old, now + seconds(1)) == AdmissionResult::bypass, "shared bidirectional negative admission");
    require(a.classify(udp, now + seconds(59)) == AdmissionResult::bypass, "UDP warmup");
    require(b.classify(flow(1001, 17), now + seconds(60)) == AdmissionResult::proxy, "new UDP");
    require(b.classify(udp, now + seconds(3599)) == AdmissionResult::bypass, "shared UDP warmup state");
    require(b.classify(udp, now + seconds(3600)) == AdmissionResult::proxy, "UDP expiry");
    require(b.classify(flow(1002, 6, 16), now + seconds(3600)) == AdmissionResult::proxy, "TCP learning ends");
    require(b.classify(old, now + seconds(86399)) == AdmissionResult::bypass, "TCP bypass lasts day");
    require(b.classify(old, now + seconds(86400)) == AdmissionResult::proxy, "TCP expiry");
    File small;
    SharedFlows s(small.path, "small", "a", 1, seconds(60), 1, true);
    require(s.classify(flow(), now) == AdmissionResult::proxy, "first admission slot");
    require(s.classify(flow(1001), now) == AdmissionResult::full, "admission capacity");
    require(s.classify(flow(1002, 6, 16), now) == AdmissionResult::bypass, "independent admission class quota");
}
static void concurrency_and_recovery() {
    File file; auto now = Clock::now(); auto env = context(); auto p = flow().flow; via::AdapterContext got;
    auto parent = std::make_unique<SharedFlows>(file.path, "processes", "parent", 4096, seconds(60), 4096, false);
    require(parent->learn(p, env, true, now) == Learn::ok, "parent learn");
    for (unsigned mode = 0; mode < 3; ++mode) {
        const auto child = ::fork(); require(child >= 0, "fork");
        if (!child) {
            parent.reset(); // Drop the inherited descriptor; keep a distinct worker membership.
            SharedFlows f(file.path, "processes", "crasher", 4096, seconds(60), 4096, false);
            SharedFlowsTest::crash(f, p, mode);
        }
        joined(child);
        require(parent->lookup(p, false, now, got), "owner-death lookup");
        const auto& e = std::get<via::Envelope>(got.value);
        require(e.origin_id == 123 && e.base.values[0] == (mode == 0 ? 17U : 99U), "undo incomplete write, retain committed write");
        require(stats(*parent).find("flow_entries=1\n") != std::string::npos, "recovered indexes/counts");
    }
    require(stats(*parent).find("shared_lock_recoveries=3\n") != std::string::npos, "recovery counter");
    pid_t children[4];
    for (unsigned worker = 0; worker < 4; ++worker) {
        children[worker] = ::fork(); require(children[worker] >= 0, "concurrent fork");
        if (!children[worker]) {
            parent.reset();
            try {
                SharedFlows f(file.path, "processes", std::to_string(worker), 4096, seconds(60), 4096, false);
                for (unsigned round = 0; round < 20; ++round) for (unsigned i = 0; i < 100; ++i) {
                    const auto key = flow(2000 + i).flow;
                    require(f.learn(key, env, true, Clock::now()) == Learn::ok, "concurrent learn");
                    require(f.lookup(reverse_key(key), true, Clock::now(), got) && got.same_context(env), "concurrent return");
                }
            } catch (...) { ::_exit(1); }
            ::_exit(0);
        }
    }
    for (auto child : children) joined(child);
    require(stats(*parent).find("flow_entries=101\n") != std::string::npos, "concurrent flow count");
    SharedFlows restarted(file.path, "processes", "crasher", 4096, seconds(60), 4096, false);
    require(restarted.lookup(p, false, Clock::now(), got), "crashed worker restart");
}
static void file_safety() {
    File file;
    int fd = ::open(file.path, O_RDWR); require(fd >= 0, "test file open");
    require(::write(fd, "unrelated data", 14) == 14, "test file write"); ::close(fd);
    fails([&] { SharedFlows f(file.path, "group", "a", 1, seconds(60), 1, false); });
    char value[14]{}; fd = ::open(file.path, O_RDONLY); require(fd >= 0, "test file reopen");
    require(::read(fd, value, sizeof(value)) == sizeof(value) && std::string(value, sizeof(value)) == "unrelated data", "unrelated file preserved");
    ::close(fd);
    File writable; require(::chmod(writable.path, 0666) == 0, "test mode");
    fails([&] { SharedFlows f(writable.path, "group", "a", 1, seconds(60), 1, false); });
}
int main() {
    try { routes_and_lifecycle(); admission(); concurrency_and_recovery(); file_safety();
        std::cout << "shared contexts, admission, process concurrency, lifecycle and owner-death recovery: OK\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
