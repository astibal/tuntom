#pragma once

#include "config.hpp"
#include "queues.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace tuntom::mp {

using Clock = std::chrono::steady_clock;

class Fd {
    int value_ = -1;

  public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() {
        if (value_ >= 0)
            ::close(value_);
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&other) noexcept : value_(other.release()) {}
    Fd &operator=(Fd &&other) noexcept {
        if (this != &other) {
            if (value_ >= 0)
                ::close(value_);
            value_ = other.release();
        }
        return *this;
    }
    int get() const { return value_; }
    int release() {
        const int result = value_;
        value_ = -1;
        return result;
    }
};

// Fields below fd/kind are direction-owned, never concurrently mutated by the
// scheduler. Main accesses them only with every worker parked at the barrier.
struct Port {
    Fd fd;
    const std::string name;
    const std::uint64_t generation;
    const Kind kind;
    Pool pool;
    std::atomic<bool> disconnected{false};
    Clock::time_point rx_retry{};
    Buffer *pending = nullptr;
    Port *pending_source = nullptr;
    Clock::time_point tx_retry{};
    std::size_t source_cursor = 0;

    Port(Fd socket, std::string id, std::uint64_t serial, Kind role, std::size_t size)
        : fd(std::move(socket)), name(std::move(id)), generation(serial), kind(role), pool(size) {}
};

struct Link {
    std::shared_ptr<Port> source, target; // Keep the ingress pool alive until drained.
    Spsc<Buffer> queue;
    Link(std::shared_ptr<Port> input, std::shared_ptr<Port> output, std::size_t capacity)
        : source(std::move(input)), target(std::move(output)), queue(capacity) {}
    std::uint64_t discard() {
        std::uint64_t count = 0;
        while (auto *buffer = queue.pop()) {
            buffer->release();
            ++count;
        }
        return count;
    }
    ~Link() { discard(); } // Unpublished plans are safe to abandon as well.
};

struct ResolvedRoute {
    Link *link = nullptr;
    std::uint64_t label = 0;
    bool exit = false;
};
struct RxTask {
    Port *port;
    std::unordered_map<std::uint64_t, ResolvedRoute> routes;
    Link *fallback = nullptr;
};
struct TxTask {
    Port *port;
    std::vector<Link *> incoming;
};
struct Job {
    bool rx;
    std::size_t index;
};
struct WorkerPlan {
    std::vector<RxTask> rx;
    std::vector<TxTask> tx;
    std::vector<Job> jobs;
    std::vector<pollfd> poll; // Private scratch for this worker; preallocated by main.
};
using LinkKey = std::pair<std::uint64_t, std::uint64_t>;
struct Plan {
    std::uint64_t version = 0;
    std::vector<std::shared_ptr<Port>> ports;
    std::map<LinkKey, std::shared_ptr<Link>> links; // Sparse RX x TX matrix.
    std::vector<WorkerPlan> workers;
    Assignment assignment;
    std::unordered_map<Port *, std::size_t> tx_owner;
};

#define TUNTOM_MP_COUNTERS(X)                                                                      \
    X(frames_rx)                                                                                   \
    X(bytes_rx) X(frames_tx) X(bytes_tx) X(route_hits) X(route_misses) X(target_disconnected)      \
        X(default_back) X(exit_deliveries) X(malformed_frames) X(rx_errors) X(send_errors)         \
            X(queue_full_drops) X(pool_stalls) X(send_eagain) X(wake_calls) X(worker_poll_errors)

struct alignas(64) Counters {
#define MP_FIELD(name) std::atomic<std::uint64_t> name{0};
    TUNTOM_MP_COUNTERS(MP_FIELD)
#undef MP_FIELD
    // One writer per counter; readers (control) need no RMW on the packet path.
    static void add(std::atomic<std::uint64_t> &counter, std::uint64_t value = 1) {
        counter.store(counter.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
    }
};

class Engine {
    struct Worker {
        Wake wake;
        Counters counters;
        std::thread thread;
        std::string name;
        std::atomic<std::uint64_t> version{0}, cpu_ns{0}, polls{0};
        std::size_t cursor = 0;
    };
    const Config &config_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::unique_ptr<Plan> plan_;
    Wake control_wake_;
    std::atomic<bool> stopping_{false}, pausing_{false};
    std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t paused_ = 0;
    std::uint64_t reconfiguration_drops_ = 0, shutdown_drops_ = 0;
    std::uint64_t reconfigurations_ = 0, reconfiguration_ns_ = 0;

    bool checkpoint() {
        if (pausing_.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (pausing_.load(std::memory_order_relaxed)) {
                ++paused_;
                condition_.notify_all();
                condition_.wait(lock, [&] {
                    return !pausing_.load(std::memory_order_relaxed) || stopping_.load();
                });
                --paused_;
                condition_.notify_all();
            }
        }
        return !stopping_.load(std::memory_order_relaxed);
    }

    void pause() {
        std::unique_lock<std::mutex> lock(mutex_);
        pausing_.store(true, std::memory_order_release);
        for (auto &worker : workers_)
            worker->wake.poke();
        condition_.wait(lock, [&] { return paused_ == workers_.size(); });
    }
    void resume() {
        std::unique_lock<std::mutex> lock(mutex_);
        pausing_.store(false, std::memory_order_release);
        condition_.notify_all();
        // Don't start a second barrier before everyone has left the first one.
        condition_.wait(lock, [&] { return paused_ == 0; });
    }
    bool interrupted() const {
        return pausing_.load(std::memory_order_relaxed) ||
               stopping_.load(std::memory_order_relaxed);
    }
    void disconnect(Port &port) {
        if (!port.disconnected.exchange(true, std::memory_order_relaxed))
            control_wake_.poke();
    }

    bool receive(RxTask &task, Worker &worker, const Plan &plan) {
        auto &port = *task.port;
        if (port.disconnected.load(std::memory_order_relaxed))
            return false;
        if (port.rx_retry != Clock::time_point{} && Clock::now() < port.rx_retry)
            return false;
        port.rx_retry = {};
        auto *buffer = port.pool.acquire();
        auto &stats = worker.counters;
        if (!buffer) {
            Counters::add(stats.pool_stalls);
            port.rx_retry = Clock::now() + std::chrono::microseconds(50);
            return false;
        }
        const auto received =
            ::recv(port.fd.get(), buffer->data, wire_capacity, MSG_DONTWAIT | MSG_TRUNC);
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            buffer->release();
            return false;
        }
        if (received <= 0 || static_cast<std::size_t>(received) > wire_capacity) {
            if (received < 0)
                Counters::add(stats.rx_errors);
            buffer->release();
            disconnect(port);
            return true;
        }
        buffer->size = static_cast<std::size_t>(received);
        SwitchFrameView frame;
        if (!decode_switch_frame(buffer->data, buffer->size, frame) ||
            frame.opcode != SwitchOpcode::switch_packet) {
            Counters::add(stats.malformed_frames);
            buffer->release();
            return true;
        }
        Counters::add(stats.frames_rx);
        Counters::add(stats.bytes_rx, buffer->size);
        Link *link = nullptr;
        const auto route = task.routes.find(frame.label(0));
        if (route != task.routes.end()) {
            Counters::add(stats.route_hits);
            link = route->second.link;
            if (!link || link->target->disconnected.load(std::memory_order_relaxed)) {
                Counters::add(stats.target_disconnected);
                link = nullptr;
            } else {
                store_be64(buffer->data + switch_base_header_size, route->second.label);
                if (route->second.exit) {
                    buffer->data[1] = static_cast<std::uint8_t>(SwitchOpcode::exit_packet);
                    Counters::add(stats.exit_deliveries);
                }
            }
        } else {
            Counters::add(stats.route_misses);
            link = task.fallback;
            if (link) {
                buffer->data[1] = static_cast<std::uint8_t>(SwitchOpcode::exit_packet);
                Counters::add(stats.default_back);
            }
        }
        if (link) {
            if (link->queue.push(buffer)) {
                // Owner is immutable throughout this plan's lifetime.
                if (workers_[plan.tx_owner.at(link->target.get())]->wake.notify())
                    Counters::add(stats.wake_calls);
                return true;
            }
            Counters::add(stats.queue_full_drops);
        }
        buffer->release();
        return true;
    }

    bool transmit(TxTask &task, Worker &worker) {
        auto &port = *task.port;
        if (port.disconnected.load(std::memory_order_relaxed))
            return false;
        if (port.tx_retry != Clock::time_point{} && Clock::now() < port.tx_retry)
            return false;
        port.tx_retry = {};
        if (!port.pending && !task.incoming.empty()) {
            for (std::size_t step = 0; step < task.incoming.size(); ++step) {
                const auto index = port.source_cursor % task.incoming.size();
                port.source_cursor = (index + 1) % task.incoming.size();
                auto *link = task.incoming[index];
                if ((port.pending = link->queue.pop())) {
                    port.pending_source = link->source.get();
                    break; // RR quota 1; next invocation starts at the next RX.
                }
            }
        }
        if (!port.pending)
            return false;
        const auto size = port.pending->size;
        const auto sent =
            ::send(port.fd.get(), port.pending->data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (errno != EINTR)
                Counters::add(worker.counters.send_eagain);
            port.tx_retry = Clock::now() + std::chrono::microseconds(50);
            return false; // Retain this pointer; poll POLLOUT without blocking other jobs.
        }
        port.pending->release();
        port.pending = nullptr;
        port.pending_source = nullptr;
        if (sent == static_cast<ssize_t>(size)) {
            Counters::add(worker.counters.frames_tx);
            Counters::add(worker.counters.bytes_tx, size);
        } else {
            Counters::add(worker.counters.send_errors);
            disconnect(port);
        }
        return true;
    }

    bool pass(WorkerPlan &tasks, Worker &worker, const Plan &plan) {
        bool progress = false;
        const auto count = tasks.jobs.size();
        for (std::size_t step = 0; step < count && !interrupted(); ++step) {
            const auto &job = tasks.jobs[(worker.cursor + step) % count];
            progress |= job.rx ? receive(tasks.rx[job.index], worker, plan)
                               : transmit(tasks.tx[job.index], worker);
        }
        if (count)
            worker.cursor = (worker.cursor + 1) % count;
        return progress;
    }

    void wait(WorkerPlan &tasks, Worker &worker) {
        tasks.poll.clear();
        tasks.poll.push_back({worker.wake.fd(), POLLIN, 0});
        auto earliest = Clock::time_point::max();
        const auto now = Clock::now();
        for (auto &task : tasks.rx) {
            auto &port = *task.port;
            int fd = port.disconnected.load(std::memory_order_relaxed) ? -1 : port.fd.get();
            if (fd >= 0 && port.rx_retry != Clock::time_point{}) {
                earliest = std::min(earliest, port.rx_retry);
                fd = -1;
            }
            tasks.poll.push_back({fd, POLLIN, 0});
        }
        for (auto &task : tasks.tx) {
            auto &port = *task.port;
            const auto fd = port.pending && !port.disconnected.load(std::memory_order_relaxed)
                                ? port.fd.get()
                                : -1;
            tasks.poll.push_back({fd, POLLOUT, 0});
        }
        timespec timeout{};
        timespec *timeout_ptr = nullptr;
        if (earliest != Clock::time_point::max()) {
            const auto ns = std::max<std::int64_t>(
                0, std::chrono::duration_cast<std::chrono::nanoseconds>(earliest - now).count());
            timeout.tv_sec = static_cast<time_t>(ns / 1000000000);
            timeout.tv_nsec = static_cast<long>(ns % 1000000000);
            timeout_ptr = &timeout;
        }
        const auto ready = ::ppoll(tasks.poll.data(), tasks.poll.size(), timeout_ptr, nullptr);
        Counters::add(worker.polls);
        if (ready < 0 && errno != EINTR) {
            Counters::add(worker.counters.worker_poll_errors);
            // Allocation-free bounded recovery; keep scheduler/shutdown wakeable.
            pollfd recovery{worker.wake.fd(), POLLIN, 0};
            (void)::poll(&recovery, 1, 10);
        }
        for (std::size_t i = 0; i < tasks.tx.size(); ++i)
            if (tasks.poll[1 + tasks.rx.size() + i].revents != 0)
                tasks.tx[i].port->tx_retry = {};
    }

    static std::uint64_t cpu_now() {
        timespec time{};
        (void)::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
        return static_cast<std::uint64_t>(time.tv_sec) * 1000000000 +
               static_cast<std::uint64_t>(time.tv_nsec);
    }
    void run(std::size_t index) {
        auto &worker = *workers_[index];
        (void)::prctl(PR_SET_NAME, worker.name.c_str(), 0UL, 0UL, 0UL);
        std::size_t samples = 0;
        while (checkpoint()) {
            // The mutex barrier is the publication fence. No plan, FD, task,
            // queue or pool can be freed while this iteration is in flight.
            auto &plan = *plan_;
            auto &tasks = plan.workers[index];
            worker.version.store(plan.version, std::memory_order_relaxed);
            if (++samples % 256 == 0)
                worker.cpu_ns.store(cpu_now(), std::memory_order_relaxed);
            if (pass(tasks, worker, plan) || interrupted())
                continue;
            worker.wake.drain();
            worker.wake.arm();
            if (!pass(tasks, worker, plan) && !interrupted()) {
                worker.cpu_ns.store(cpu_now(), std::memory_order_relaxed);
                wait(tasks, worker);
            }
            worker.wake.cancel();
        }
        worker.cpu_ns.store(cpu_now(), std::memory_order_relaxed);
    }

    std::unique_ptr<Plan> make_plan(std::vector<std::shared_ptr<Port>> ports) {
        auto plan = std::make_unique<Plan>();
        plan->version = plan_ ? plan_->version + 1 : 1;
        plan->ports = std::move(ports);
        plan->workers.resize(workers_.size());
        std::vector<Kind> kinds;
        std::unordered_map<std::string, std::shared_ptr<Port>> names;
        for (const auto &port : plan->ports) {
            kinds.push_back(port->kind);
            names.emplace(port->name, port);
        }
        plan->assignment = schedule(kinds, workers_.size(), config_.policy);
        for (std::size_t i = 0; i < plan->ports.size(); ++i)
            plan->tx_owner.emplace(plan->ports[i].get(), plan->assignment.owners[i][1]);
        const auto link_for = [&](const std::shared_ptr<Port> &source,
                                  const std::shared_ptr<Port> &target) {
            const LinkKey key{source->generation, target->generation};
            auto existing = plan->links.find(key);
            if (existing != plan->links.end())
                return existing->second.get();
            std::shared_ptr<Link> link;
            if (plan_) {
                const auto previous = plan_->links.find(key);
                if (previous != plan_->links.end())
                    link = previous->second;
            }
            if (!link)
                link = std::make_shared<Link>(source, target, config_.queue_size);
            auto *result = link.get();
            plan->links.emplace(key, std::move(link));
            return result;
        };
        for (std::size_t i = 0; i < plan->ports.size(); ++i) {
            auto &port = plan->ports[i];
            RxTask rx{port.get(), {}, nullptr};
            const auto routes = config_.routes.find(port->name);
            if (routes != config_.routes.end()) {
                for (const auto &item : routes->second) {
                    const auto target = names.find(item.second.port);
                    Link *link = target == names.end() ? nullptr : link_for(port, target->second);
                    rx.routes.emplace(item.first,
                                      ResolvedRoute{link, item.second.label,
                                                    config_.exits.count(item.second.port) != 0});
                }
            }
            if (config_.default_back)
                rx.fallback = link_for(port, port);
            plan->workers[plan->assignment.owners[i][0]].rx.push_back(std::move(rx));
        }
        for (std::size_t i = 0; i < plan->ports.size(); ++i) {
            TxTask tx{plan->ports[i].get(), {}};
            for (const auto &link : plan->links)
                if (link.second->target == plan->ports[i])
                    tx.incoming.push_back(link.second.get());
            plan->workers[plan->assignment.owners[i][1]].tx.push_back(std::move(tx));
        }
        for (auto &worker : plan->workers) {
            // Interleave RX/TX when roles must share a worker on small CPUs.
            for (std::size_t i = 0; i < std::max(worker.rx.size(), worker.tx.size()); ++i) {
                if (i < worker.rx.size())
                    worker.jobs.push_back({true, i});
                if (i < worker.tx.size())
                    worker.jobs.push_back({false, i});
            }
            worker.poll.reserve(1 + worker.rx.size() + worker.tx.size());
        }
        return plan;
    }

    static void discard_pending(Port &port) {
        port.pending->release();
        port.pending = nullptr;
        port.pending_source = nullptr;
        port.tx_retry = {};
    }

  public:
    Engine(const Config &config, std::size_t budget) : config_(config) {
        for (std::size_t i = 0; i < budget; ++i) {
            auto worker = std::make_unique<Worker>();
            worker->name = "tomtom-mp-" + std::to_string(i);
            workers_.push_back(std::move(worker));
        }
        plan_ = make_plan({});
    }
    ~Engine() { stop(); }
    void start() {
        try {
            for (std::size_t i = 0; i < workers_.size(); ++i)
                workers_[i]->thread = std::thread([this, i] { run(i); });
        } catch (...) {
            stop();
            throw;
        }
    }
    void stop() {
        stopping_.store(true, std::memory_order_relaxed);
        condition_.notify_all();
        for (auto &worker : workers_)
            worker->wake.poke();
        for (auto &worker : workers_)
            if (worker->thread.joinable())
                worker->thread.join();
        if (!plan_)
            return;
        for (auto &port : plan_->ports)
            if (port->pending) {
                discard_pending(*port);
                ++shutdown_drops_;
            }
        for (auto &link : plan_->links)
            shutdown_drops_ += link.second->discard();
    }
    int event_fd() const { return control_wake_.fd(); }
    void drain_events() { control_wake_.drain(); }
    const Plan &plan() const { return *plan_; } // Main only, except direction-owned fields.

    void replace(std::vector<std::shared_ptr<Port>> ports) {
        // All allocation (including private lookup tables and poll scratch)
        // precedes the barrier. Failure leaves the current plan fully usable.
        auto next = make_plan(std::move(ports));
        const auto began = Clock::now();
        pause();
        for (auto &port : plan_->ports) {
            if (port->pending && (!next->tx_owner.count(port.get()) ||
                                  !next->tx_owner.count(port->pending_source))) {
                discard_pending(*port);
                ++reconfiguration_drops_;
            }
        }
        for (auto &link : plan_->links)
            if (!next->links.count(link.first))
                reconfiguration_drops_ += link.second->discard();
        plan_.swap(next);
        next.reset(); // Close retired FDs and release pools while every worker is parked.
        ++reconfigurations_;
        resume();
        reconfiguration_ns_ += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - began).count());
    }

    std::uint64_t frames_rx() const {
        std::uint64_t n = 0;
        for (auto &w : workers_)
            n += w->counters.frames_rx.load();
        return n;
    }
    std::uint64_t frames_tx() const {
        std::uint64_t n = 0;
        for (auto &w : workers_)
            n += w->counters.frames_tx.load();
        return n;
    }
    std::uint64_t bytes_rx() const {
        std::uint64_t n = 0;
        for (auto &w : workers_)
            n += w->counters.bytes_rx.load();
        return n;
    }
    std::uint64_t bytes_tx() const {
        std::uint64_t n = 0;
        for (auto &w : workers_)
            n += w->counters.bytes_tx.load();
        return n;
    }
    void write_stats(std::ostream &out) const {
        out << "scheduler_version=" << plan_->version << "\nworkers_pool=" << workers_.size()
            << "\nworkers_active=" << plan_->assignment.active
            << "\nworkers_idle=" << workers_.size() - plan_->assignment.active
            << "\nreconfigurations=" << reconfigurations_
            << "\nreconfiguration_ns=" << reconfiguration_ns_
            << "\nreconfiguration_drops=" << reconfiguration_drops_
            << "\nshutdown_drops=" << shutdown_drops_
            << "\nsend_backpressure_drops=0\nmatrix_queues=" << plan_->links.size() << '\n';
        for (std::size_t role = 0; role < 4; ++role)
            out << "role_" << role_name(role) << "_shards=" << plan_->assignment.shards[role]
                << '\n';
#define MP_SUM(name)                                                                               \
    {                                                                                              \
        std::uint64_t sum = 0;                                                                     \
        for (auto &w : workers_)                                                                   \
            sum += w->counters.name.load(std::memory_order_relaxed);                               \
        out << #name "=" << sum << '\n';                                                           \
    }
        TUNTOM_MP_COUNTERS(MP_SUM)
#undef MP_SUM
        std::size_t in_use = 0;
        for (std::size_t i = 0; i < plan_->ports.size(); ++i) {
            const auto &port = *plan_->ports[i];
            in_use += port.pool.in_use();
            out << "port_" << i << "_name=" << port.name << "\nport_" << i
                << "_generation=" << port.generation << "\nport_" << i
                << "_kind=" << kind_name(port.kind) << "\nport_" << i
                << "_rx_owner=" << plan_->assignment.owners[i][0] << "\nport_" << i
                << "_tx_owner=" << plan_->assignment.owners[i][1] << '\n';
        }
        out << "buffers_in_use=" << in_use << '\n';
        for (std::size_t i = 0; i < workers_.size(); ++i) {
            out << "worker_" << i << "_roles=";
            bool first = true;
            for (std::size_t role = 0; role < 4; ++role)
                if (plan_->assignment.worker_roles[i] & (1U << role)) {
                    out << (first ? "" : ",") << role_name(role);
                    first = false;
                }
            out << (first ? "idle" : "") << "\nworker_" << i
                << "_version=" << workers_[i]->version.load() << "\nworker_" << i
                << "_cpu_ns=" << workers_[i]->cpu_ns.load() << "\nworker_" << i
                << "_poll_calls=" << workers_[i]->polls.load() << '\n';
        }
    }
};

#undef TUNTOM_MP_COUNTERS
} // namespace tuntom::mp
