#pragma once

#include "config.hpp"
#include "../switch_ecmp.hpp"
#include "queues.hpp"
#include "../relay/registry.hpp"
#include "../ipc/switch_transport.hpp"
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
#include <sys/epoll.h>
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
    const std::uint64_t identity;
    std::atomic<bool> disconnected{false};
    relay::Registry relay_registry; // Controller-owned; workers read the immutable Plan copy.
    std::atomic<std::int64_t> relay_expires{0};
    Spsc<Buffer> relay_control{16};
    bool relay_live() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count() < relay_expires.load(std::memory_order_relaxed);
    }
    ~Port() { while (auto* buffer = relay_control.pop()) buffer->release(); }
    alignas(64) Pool pool; // RX-owned state must not share TX's writable cache line.
    Clock::time_point rx_retry{};
    std::unique_ptr<ipc::Transport> transport;
    struct PendingFrame { Buffer *buffer = nullptr; Port *source = nullptr; };
    alignas(64) std::array<PendingFrame, ipc::max_batch> pending{};
    std::size_t pending_count = 0;
    std::size_t source_cursor = 0;

    Port(Fd socket, std::string id, std::uint64_t serial, Kind role, std::size_t size,
         std::unique_ptr<ipc::Transport> ipc_transport = {})
        : fd(std::move(socket)), name(std::move(id)), generation(serial), kind(role),
          identity(ecmp_port_identity(name)), pool(size),
          transport(std::move(ipc_transport)) {}
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
    std::size_t tx_worker = 0; // Resolved once per immutable plan, including fallback.
    std::uint64_t label = 0;
    bool exit = false;
    std::uint32_t channel = 0;
    std::uint64_t epoch = 0;
};
struct ResolvedGroup {
    std::vector<ResolvedRoute> members;
};
struct ResolvedMapping {
    const RuleStatement *rule = nullptr;
    ResolvedGroup targets;
};
struct RxTask {
    Port *port;
    std::unordered_map<std::uint64_t, ResolvedGroup> routes;
    ResolvedRoute fallback;
    bool ready = false; // Retain readiness across RR turns until recv returns EAGAIN.
    bool rules_enabled = false;
    RulesProgram program;
    std::vector<ResolvedMapping> mappings;
    std::unordered_map<const RuleStatement *, std::size_t> mapping_indices;
    std::map<std::string, ResolvedRoute> divert_routes;
};
struct TxTask {
    Port *port;
    std::vector<Link *> incoming;
    std::size_t io_index = 0;
    bool blocked = false;
};
struct Job {
    bool rx;
    std::size_t index;
};
constexpr std::size_t no_task = static_cast<std::size_t>(-1);
struct IoPort {
    Port *port;
    std::size_t rx = no_task, tx = no_task;
    std::uint32_t interest = EPOLLET | EPOLLRDHUP;
};
struct PollSetupError : std::runtime_error {
    int error;
    explicit PollSetupError(int value) : std::runtime_error("Cannot prepare worker epoll"), error(value) {}
};
struct RulesPlanError : std::runtime_error { using std::runtime_error::runtime_error; };
struct WorkerPlan {
    std::vector<RxTask> rx;
    std::vector<TxTask> tx;
    std::vector<Job> jobs;
    Fd poller;
    std::vector<IoPort> io;
    std::vector<epoll_event> events;
    bool fresh = true;
    bool io_fault = false;
};
using LinkKey = std::pair<std::uint64_t, std::uint64_t>;
struct Plan {
    std::uint64_t version = 0;
    std::vector<std::shared_ptr<Port>> ports;
    std::map<LinkKey, std::shared_ptr<Link>> links; // Sparse RX x TX matrix.
    std::vector<WorkerPlan> workers;
    Assignment assignment;
    std::unordered_map<Port *, std::size_t> tx_owner;
    std::shared_ptr<const SwitchRuleset> ruleset;
    std::vector<Kind> kinds;
    std::unique_ptr<divert::SwitchPath> divert_path;
    std::unique_ptr<via::SwitchPath> via_path;
    bool via_guard = false;
    std::map<std::string, Port*> divert_ports;
    std::map<Port*,relay::Directory> relay_directories;
    std::map<std::string,std::pair<std::uint32_t,std::uint64_t>> relay_targets;
};

#define TUNTOM_MP_COUNTERS(X)                                                                      \
    X(frames_rx)                                                                                   \
    X(bytes_rx) X(frames_tx) X(bytes_tx) X(route_hits) X(route_misses) X(target_disconnected) X(ecmp_packets) \
        X(default_back) X(exit_deliveries) X(malformed_frames) X(rx_errors) X(send_errors)         \
            X(queue_full_drops) X(pool_stalls) X(send_eagain) X(wake_calls) X(worker_poll_errors) \
        X(recv_calls) X(recv_eagain) X(send_calls) X(wake_reads) X(cpu_samples) X(policy_drops) X(rewrite_drops) \
        X(divert_forwarded) X(divert_invalid_drops) X(divert_overflow_drops)

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
        std::size_t since_events = 0;
        Clock::time_point next_cpu_sample{};
    };
    const Config &config_;
    std::unique_ptr<via::State> via_state_;
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

    bool receive(RxTask &task, Worker &worker) {
        auto &port = *task.port;
        if (!task.ready || port.disconnected.load(std::memory_order_relaxed))
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
        Counters::add(stats.recv_calls);
        const auto received =
            port.transport ? port.transport->receive(port.fd.get(), buffer->data, wire_capacity) :
            ::recv(port.fd.get(), buffer->data, wire_capacity, MSG_DONTWAIT | MSG_TRUNC);
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            buffer->release();
            if (errno == EINTR)
                return true; // Still runnable; ET may not deliver another edge.
            task.ready = false;
            Counters::add(stats.recv_eagain);
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
        const std::string* ingress = &port.name;
        if (relay::marked(buffer->data,buffer->size)) {
            relay::View record;
            if (!plan_->ruleset || !relay::configured(*plan_->ruleset,port.name) || !relay::decode(buffer->data,buffer->size,record)) {
                Counters::add(stats.malformed_frames); buffer->release(); return true;
            }
            if (record.type == relay::Type::snapshot) {
                if (!port.relay_control.push(buffer)) buffer->release();
                else control_wake_.poke();
                return true;
            }
            const auto directory = plan_->relay_directories.find(&port);
            if (record.type != relay::Type::data || !port.relay_live() || directory == plan_->relay_directories.end() ||
                record.epoch != directory->second.epoch) { buffer->release(); return true; }
            const auto channel = directory->second.channels.find(record.channel);
            if (channel == directory->second.channels.end()) { buffer->release(); return true; }
            ingress = &channel->second.name;
            buffer->size = record.size; std::memmove(buffer->data,record.payload,record.size);
        } else if (plan_->ruleset && relay::configured(*plan_->ruleset,port.name)) { buffer->release(); return true; }
        SwitchFrameView frame;
        if (!decode_switch_frame(buffer->data, buffer->size, frame) ||
            frame.opcode != SwitchOpcode::switch_packet || frame.payload_size > max_ip_packet_size) {
            Counters::add(stats.malformed_frames);
            buffer->release();
            return true;
        }
        Counters::add(stats.frames_rx);
        Counters::add(stats.bytes_rx, buffer->size);
        Link *link = nullptr;
        std::size_t tx_worker = 0;
        const auto route = task.routes.find(frame.label(0));
        if (plan_->via_guard && !plan_->via_path) {
            via::Envelope retired;
            if (via::reserved(port.name) || !via::Codec::split(divert::labels_of(frame), retired) || retired.present) {
                Counters::add(stats.divert_invalid_drops); buffer->release(); return true;
            }
        }
        divert::Decision decision;
        const auto live = [&](const std::string& name) {
            const auto found = plan_->divert_ports.find(name);
            return found != plan_->divert_ports.end() && !found->second->disconnected.load(std::memory_order_relaxed) &&
                (!plan_->relay_targets.count(name) || found->second->relay_live());
        };
        if (plan_->via_path) decision = plan_->via_path->route(*ingress, frame, live);
        else if (plan_->divert_path) decision = plan_->divert_path->route(port.name, frame, live);
        if (decision.result != divert::Result::normal) {
            using divert::Result;
            if (decision.result == Result::forward) {
                const auto found = task.divert_routes.find(*decision.target);
                if (found != task.divert_routes.end()) {
                    link = found->second.link; tx_worker = found->second.tx_worker;
                    rewrite_rules_frame(buffer->data, buffer->size, frame, decision.labels.values, decision.labels.size, decision.exit);
                    if (found->second.channel && !relay::wrap(buffer->data,buffer->size,wire_capacity,found->second.channel,found->second.epoch)) {
                        Counters::add(stats.rewrite_drops); buffer->release(); return true;
                    }
                    if (decision.exit) Counters::add(stats.exit_deliveries);
                    if (decision.multipath) Counters::add(stats.ecmp_packets);
                    Counters::add(stats.divert_forwarded);
                } else Counters::add(stats.target_disconnected);
            } else if (decision.result == Result::overflow) Counters::add(stats.divert_overflow_drops);
            else if (decision.result == Result::malformed) Counters::add(stats.divert_invalid_drops);
            else if (decision.result == Result::policy) Counters::add(stats.policy_drops);
            else Counters::add(stats.target_disconnected);
        } else if (ingress != &port.name) {
            Counters::add(stats.divert_invalid_drops); buffer->release(); return true;
        } else if (task.rules_enabled) {
            const auto *rule = task.program.mapping(frame);
            if (!rule) { Counters::add(stats.route_misses); buffer->release(); return true; }
            if (rule->type == RuleStatement::Type::policy) {
                Counters::add(stats.policy_drops); buffer->release(); return true;
            }
            const auto *mapping = &task.mappings[task.mapping_indices.at(rule)];
            Counters::add(stats.route_hits);
            std::array<std::uint64_t, switch_max_labels> labels{};
            std::size_t count = 0;
            if (!mapping->rule->stack.apply(frame, labels, count) ||
                switch_base_header_size + count * switch_label_size + frame.payload_size > wire_capacity) {
                Counters::add(stats.rewrite_drops); buffer->release(); return true;
            }
            EcmpSelector selector(frame);
            const ResolvedRoute *selected = nullptr;
            bool connected = false;
            for (const auto &member : mapping->targets.members) {
                const auto &target = *member.link->target;
                if (target.disconnected.load(std::memory_order_relaxed)) continue;
                connected = true;
                if (!task.program.allowed(rule, frame, target.name, labels.data(), count)) continue;
                if (selector.consider(target.identity, target.name)) selected = &member;
            }
            if (!selected) {
                if (connected) Counters::add(stats.policy_drops);
                else Counters::add(stats.target_disconnected);
            } else {
                if (selector.multipath()) Counters::add(stats.ecmp_packets);
                link = selected->link; tx_worker = selected->tx_worker;
                rewrite_rules_frame(buffer->data, buffer->size, frame, labels, count, selected->exit);
                if (selected->exit) Counters::add(stats.exit_deliveries);
            }
        } else if (route != task.routes.end()) {
            Counters::add(stats.route_hits);
            EcmpSelector selector(frame);
            const ResolvedRoute *selected = nullptr;
            for (const auto &member : route->second.members) {
                const auto &target = *member.link->target;
                if (target.disconnected.load(std::memory_order_relaxed)) continue;
                if (selector.consider(target.identity, target.name)) selected = &member;
            }
            if (selector.multipath()) Counters::add(stats.ecmp_packets);
            if (!selected) {
                Counters::add(stats.target_disconnected);
            } else {
                link = selected->link;
                tx_worker = selected->tx_worker;
                store_be64(buffer->data + switch_base_header_size, selected->label);
                if (selected->exit) {
                    buffer->data[1] = static_cast<std::uint8_t>(SwitchOpcode::exit_packet);
                    Counters::add(stats.exit_deliveries);
                }
            }
        } else {
            Counters::add(stats.route_misses);
            link = task.fallback.link;
            tx_worker = task.fallback.tx_worker;
            if (link) {
                buffer->data[1] = static_cast<std::uint8_t>(SwitchOpcode::exit_packet);
                Counters::add(stats.default_back);
            }
        }
        if (link) {
            if (link->queue.push(buffer)) {
                // Owner is immutable throughout this plan's lifetime.
                if (workers_[tx_worker].get() != &worker && workers_[tx_worker]->wake.notify())
                    Counters::add(stats.wake_calls);
                return true;
            }
            Counters::add(stats.queue_full_drops);
        }
        buffer->release();
        return true;
    }

    bool set_output_interest(WorkerPlan &tasks, TxTask &task, bool enabled) {
        auto &io = tasks.io[task.io_index];
        const auto interest = enabled ? io.interest | EPOLLOUT : io.interest & ~std::uint32_t(EPOLLOUT);
        if (interest == io.interest && !enabled)
            return true;
        epoll_event event{}; event.events = interest; event.data.u64 = task.io_index + 1;
        int result;
        do { result = ::epoll_ctl(tasks.poller.get(), EPOLL_CTL_MOD, io.port->fd.get(), &event); }
        while (result < 0 && errno == EINTR);
        if (result < 0) {
            tasks.io_fault = true;
            return false;
        }
        io.interest = interest;
        return true;
    }

    bool transmit(TxTask &task, Worker &worker, WorkerPlan &tasks) {
        auto &port = *task.port;
        if (task.blocked || port.disconnected.load(std::memory_order_relaxed))
            return false;
        const auto limit = port.transport ? port.transport->batch_limit() : 1;
        if (!port.pending_count && !task.incoming.empty()) {
            // One complete RR sweep, quota 1 per ingress queue. Never revisit a
            // source just to fill a batch. A partial batch is submitted now.
            for (std::size_t step = 0; step < task.incoming.size() && port.pending_count < limit; ++step) {
                const auto index = port.source_cursor % task.incoming.size();
                port.source_cursor = (index + 1) % task.incoming.size();
                auto *link = task.incoming[index];
                if (auto *buffer = link->queue.pop())
                    port.pending[port.pending_count++] = {buffer, link->source.get()};
            }
        }
        if (!port.pending_count) return false;
        std::array<ipc::FrameParts, ipc::max_batch> frames{};
        for (std::size_t i = 0; i < port.pending_count; ++i)
            frames[i] = {port.pending[i].buffer->data, port.pending[i].buffer->size};
        Counters::add(worker.counters.send_calls);
        ssize_t sent;
        if (port.transport) sent = port.transport->send_batch(port.fd.get(), frames.data(), port.pending_count);
        else {
            sent = ::send(port.fd.get(), frames[0].first, frames[0].first_size, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (sent == static_cast<ssize_t>(frames[0].size())) sent = 1;
            else if (sent >= 0) { errno = EIO; sent = -1; }
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (errno == EINTR) return true;
            Counters::add(worker.counters.send_eagain);
            task.blocked = set_output_interest(tasks, task, true);
            return false; // Retain the entire unsubmitted prefix until EPOLLOUT.
        }
        if (sent > 0 && static_cast<std::size_t>(sent) <= port.pending_count) {
            const auto done = static_cast<std::size_t>(sent);
            for (std::size_t i = 0; i < done; ++i) {
                Counters::add(worker.counters.frames_tx);
                Counters::add(worker.counters.bytes_tx, port.pending[i].buffer->size);
                port.pending[i].buffer->release();
            }
            port.pending_count -= done;
            for (std::size_t i = 0; i < port.pending_count; ++i) port.pending[i] = port.pending[i + done];
        } else {
            Counters::add(worker.counters.send_errors, port.pending_count);
            discard_pending(port);
            disconnect(port);
        }
        return true;
    }

    bool pass(WorkerPlan &tasks, Worker &worker) {
        bool progress = false;
        const auto count = tasks.jobs.size();
        for (std::size_t step = 0; step < count && !interrupted(); ++step) {
            const auto &job = tasks.jobs[(worker.cursor + step) % count];
            const bool worked = job.rx ? receive(tasks.rx[job.index], worker)
                                       : transmit(tasks.tx[job.index], worker, tasks);
            progress |= worked;
            worker.since_events += worked;
        }
        if (count)
            worker.cursor = (worker.cursor + 1) % count;
        return progress;
    }

    void poll_recovery(WorkerPlan &tasks, Worker &worker) {
        Counters::add(worker.counters.worker_poll_errors);
        // Preserve progress after transient poll/ctl failures without spinning.
        // Rescan once after the bounded pause: an ET notification may be unavailable.
        pollfd recovery{worker.wake.fd(), POLLIN, 0};
        if (::poll(&recovery, 1, 10) > 0 && (recovery.revents & POLLIN)) {
            worker.wake.drain();
            Counters::add(worker.counters.wake_reads);
        }
        for (auto &rx : tasks.rx)
            rx.ready = true;
        tasks.io_fault = false;
        for (auto &tx : tasks.tx) {
            tx.blocked = false;
            (void)set_output_interest(tasks, tx, false);
        }
    }

    void collect_events(WorkerPlan &tasks, Worker &worker, bool block) {
        if (tasks.io_fault) {
            poll_recovery(tasks, worker);
            return;
        }
        int timeout = block ? -1 : 0;
        if (block) {
            auto earliest = Clock::time_point::max();
            for (const auto &rx : tasks.rx)
                if (rx.port->rx_retry != Clock::time_point{})
                    earliest = std::min(earliest, rx.port->rx_retry);
            if (earliest != Clock::time_point::max()) {
                // epoll_wait has millisecond timeouts. A pollable epoll FD lets
                // ppoll retain the existing 50us pool retry without a timer FD
                // or a dependency on Linux 5.11's epoll_pwait2.
                const auto ns = std::max<std::int64_t>(0,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(earliest - Clock::now()).count());
                timespec until{static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)};
                pollfd epoll_fd{tasks.poller.get(), POLLIN, 0};
                const auto result = ::ppoll(&epoll_fd, 1, &until, nullptr);
                if (result < 0 && errno != EINTR) {
                    poll_recovery(tasks, worker);
                    return;
                }
                timeout = 0;
            }
        }
        const auto count = ::epoll_wait(tasks.poller.get(), tasks.events.data(),
                                       static_cast<int>(tasks.events.size()), timeout);
        Counters::add(worker.polls);
        if (count < 0) {
            if (errno != EINTR)
                poll_recovery(tasks, worker);
            return;
        }
        for (int i = 0; i < count; ++i) {
            const auto &event = tasks.events[static_cast<std::size_t>(i)];
            if (!event.data.u64) {
                worker.wake.drain();
                Counters::add(worker.counters.wake_reads);
                continue;
            }
            auto &io = tasks.io[event.data.u64 - 1];
            if (event.events & (EPOLLERR | EPOLLHUP)) {
                disconnect(*io.port);
                continue;
            }
            if (io.rx != no_task && (event.events & (EPOLLIN | EPOLLRDHUP)))
                tasks.rx[io.rx].ready = true;
            if (io.tx != no_task && (event.events & EPOLLOUT)) {
                auto &tx = tasks.tx[io.tx];
                tx.blocked = false;
                (void)set_output_interest(tasks, tx, false);
            }
        }
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
        while (checkpoint()) {
            // The mutex barrier publishes the complete plan and its epoll sets.
            auto &plan = *plan_;
            auto &tasks = plan.workers[index];
            if (tasks.fresh) {
                worker.version.store(plan.version, std::memory_order_relaxed);
                tasks.fresh = false;
                for (auto &rx : tasks.rx) {
                    rx.ready = rx.port->transport && rx.port->transport->receive_pending();
                    rx.port->rx_retry = {};
                } // Re-evaluate pool state under the new owner.
                collect_events(tasks, worker, false);
                worker.since_events = 0;
            }
            if (Clock::now() >= worker.next_cpu_sample) {
                worker.cpu_ns.store(cpu_now(), std::memory_order_relaxed);
                Counters::add(worker.counters.cpu_samples);
                worker.next_cpu_sample = Clock::now() + std::chrono::milliseconds(100);
            }
            const bool progress = pass(tasks, worker);
            if (interrupted())
                continue;
            if (tasks.io_fault || worker.since_events >= 32) {
                collect_events(tasks, worker, false);
                worker.since_events = 0;
            }
            if (progress)
                continue;
            worker.wake.arm();
            if (!pass(tasks, worker) && !interrupted())
                collect_events(tasks, worker, true);
            worker.wake.cancel();
        }
        worker.cpu_ns.store(cpu_now(), std::memory_order_relaxed);
    }

    std::unique_ptr<Plan> make_plan(std::vector<std::shared_ptr<Port>> ports,
                                  std::shared_ptr<const SwitchRuleset> ruleset) {
        auto plan = std::make_unique<Plan>();
        plan->version = plan_ ? plan_->version + 1 : 1;
        plan->ports = std::move(ports);
        plan->ruleset = std::move(ruleset);
        plan->via_guard = via::enabled(plan->ruleset) || (plan_ && plan_->via_guard);
        plan->workers.resize(workers_.size());
        std::vector<Kind> kinds;
        std::unordered_map<std::string, std::shared_ptr<Port>> names;
        for (const auto &port : plan->ports) {
            const bool divert_port = (via::enabled(plan->ruleset) && via::reserved(port->name)) ||
                (config_.divert_config && (port->name == config_.divert_config->input || port->name == config_.divert_config->output));
            kinds.push_back(divert_port ? Kind::adapter : plan->ruleset ?
                (plan->ruleset->role(port->name, RuleStatement::Type::exit) ? Kind::adapter :
                 plan->ruleset->role(port->name, RuleStatement::Type::trunk) ? Kind::trunk : Kind::tunnel) : port->kind);
            names.emplace(port->name, port);
        }
        if (config_.divert_config && config_.divert_config->via && !via::enabled(plan->ruleset))
            throw RulesPlanError("VIA divert requires rules format 3");
        if (config_.divert_config || via::enabled(plan->ruleset)) {
            std::vector<std::string> port_names;
            std::map<std::string,std::string> bindings;
            for (const auto& port : plan->ports) {
                port_names.push_back(port->name);
                plan->divert_ports.emplace(port->name, port.get());
                if (plan->ruleset && port->relay_registry.live()) {
                    auto directory = port->relay_registry.directory;
                    for (auto it = directory.channels.begin(); it != directory.channels.end();) {
                        if (!via::accepted(*plan->ruleset,it->second.name,port->name)) it = directory.channels.erase(it);
                        else {
                            const auto& name = it->second.name;
                            port_names.push_back(name); bindings.emplace(name,port->name);
                            plan->divert_ports.emplace(name,port.get()); names.emplace(name,port);
                            plan->relay_targets.emplace(name,std::make_pair(it->first,directory.epoch)); ++it;
                        }
                    }
                    plan->relay_directories.emplace(port.get(),std::move(directory));
                }
            }
            if (via::enabled(plan->ruleset)) {
                if (!plan_ || !via::enabled(plan_->ruleset)) {
                    for (const auto& port : plan->ports) {
                        if (!via::accepted(*plan->ruleset, port->name)) throw RulesPlanError("existing port conflicts with VIA registration: " + port->name);
                        for (const auto& other : plan->ports)
                            if (port != other && !via::compatible(*plan->ruleset, port->name, port->fd.get(), other->name, other->fd.get()))
                                throw RulesPlanError("existing ports conflict with VIA instance ownership");
                    }
                }
                if (!via_state_) via_state_ = std::make_unique<via::State>();
                plan->via_path = std::make_unique<via::SwitchPath>(*via_state_, plan->ruleset, config_.divert_config, std::move(port_names), bindings);
            } else plan->divert_path = std::make_unique<divert::SwitchPath>(config_.divert_config, plan->ruleset, std::move(port_names));
        }
        plan->assignment = schedule(kinds, workers_.size(), config_.policy);
        plan->kinds = std::move(kinds);
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
        std::size_t resolution_work = 0;
        for (std::size_t i = 0; i < plan->ports.size(); ++i) {
            auto &port = plan->ports[i];
            RxTask rx{port.get(), {}, {}, false, false, {}, {}, {}, {}};
            rx.rules_enabled = static_cast<bool>(plan->ruleset);
            if (plan->ruleset) {
                rx.program = RulesProgram(*plan->ruleset, port->name);
                for (const auto *mapping : rx.program.mappings) {
                    ResolvedMapping compiled{mapping, {}};
                    for (std::size_t target_index = 0; target_index < plan->ports.size(); ++target_index) {
                        const auto &target = plan->ports[target_index];
                        if (++resolution_work > 1024 * 1024)
                            throw RulesPlanError("ruleset resolution exceeds 1048576 mapping/port pairs");
                        if (!route_port_matches(mapping->output.port, target->name)) continue;
                        auto *link = link_for(port, target);
                        compiled.targets.members.push_back({link, plan->tx_owner.at(target.get()), 0,
                            plan->kinds[target_index] == Kind::adapter});
                    }
                    rx.mapping_indices.emplace(mapping, rx.mappings.size());
                    rx.mappings.push_back(std::move(compiled));
                }
            }
            const auto routes = plan->ruleset ? SwitchPortRoutes{} : routes_for_port(config_.routes, port->name);
            for (const auto &item : routes) {
                ResolvedGroup group;
                const auto add_member = [&](const std::shared_ptr<Port> &target) {
                    auto *link = link_for(port, target);
                    group.members.push_back({link, plan->tx_owner.at(target.get()), item.second.label,
                                             config_.exits.count(target->name) != 0});
                };
                if (wildcard_port(item.second.port)) {
                    for (const auto &target : plan->ports)
                        if (route_port_matches(item.second.port, target->name)) add_member(target);
                } else {
                    const auto target = names.find(item.second.port);
                    if (target != names.end()) add_member(target->second);
                }
                // Keep empty groups: a configured but unavailable target is not
                // a route miss and must never trigger default-back.
                rx.routes.emplace(item.first, std::move(group));
            }
            if (!plan->ruleset && config_.default_back)
                rx.fallback = {link_for(port, port), plan->tx_owner.at(port.get()), 0, true};
            if (plan->divert_path || plan->via_path) {
                std::vector<std::string> sources{port->name};
                const auto directory = plan->relay_directories.find(port.get());
                if (directory != plan->relay_directories.end()) for (const auto& item : directory->second.channels) sources.push_back(item.second.name);
                for (const auto& named : names) {
                    const auto& target = named.second;
                    bool possible = false;
                    for (const auto& source : sources) {
                        if (++resolution_work > 1024 * 1024) throw RulesPlanError("ruleset resolution exceeds 1048576 mapping/port pairs");
                        if (plan->via_path ? plan->via_path->possible(source,named.first) : plan->divert_path->possible(source,named.first)) { possible = true; break; }
                    }
                    if (!possible) continue;
                    const auto binding = plan->relay_targets.find(named.first);
                    rx.divert_routes.emplace(named.first, ResolvedRoute{
                        link_for(port, target), plan->tx_owner.at(target.get()), 0, false,
                        binding == plan->relay_targets.end() ? 0 : binding->second.first,
                        binding == plan->relay_targets.end() ? 0 : binding->second.second});
                }
            }
            plan->workers[plan->assignment.owners[i][0]].rx.push_back(std::move(rx));
        }
        for (std::size_t i = 0; i < plan->ports.size(); ++i) {
            TxTask tx{plan->ports[i].get(), {}};
            for (const auto &link : plan->links)
                if (link.second->target == plan->ports[i])
                    tx.incoming.push_back(link.second.get());
            plan->workers[plan->assignment.owners[i][1]].tx.push_back(std::move(tx));
        }
        for (std::size_t index = 0; index < plan->workers.size(); ++index) {
            auto &worker = plan->workers[index];
            // Interleave RX/TX when roles must share a worker on small CPUs.
            for (std::size_t i = 0; i < std::max(worker.rx.size(), worker.tx.size()); ++i) {
                if (i < worker.rx.size())
                    worker.jobs.push_back({true, i});
                if (i < worker.tx.size())
                    worker.jobs.push_back({false, i});
            }
            // A fresh epoll instance per plan prevents old readiness tokens from
            // referring to another generation/index after a topology change.
            worker.poller = Fd(::epoll_create1(EPOLL_CLOEXEC));
            if (worker.poller.get() < 0)
                throw PollSetupError(errno);
            epoll_event wake_event{}; wake_event.events = EPOLLIN;
            if (::epoll_ctl(worker.poller.get(), EPOLL_CTL_ADD, workers_[index]->wake.fd(), &wake_event) < 0)
                throw PollSetupError(errno);
            std::unordered_map<Port *, std::size_t> indices;
            const auto io_for = [&](Port *port) {
                auto result = indices.emplace(port, worker.io.size());
                if (result.second)
                    worker.io.push_back({port});
                return result.first->second;
            };
            for (std::size_t i = 0; i < worker.rx.size(); ++i) {
                auto &io = worker.io[io_for(worker.rx[i].port)];
                io.rx = i;
                io.interest |= EPOLLIN;
            }
            for (std::size_t i = 0; i < worker.tx.size(); ++i) {
                const auto io_index = io_for(worker.tx[i].port);
                worker.io[io_index].tx = i;
                worker.tx[i].io_index = io_index;
            }
            worker.events.resize(worker.io.size() + 1);
            for (std::size_t i = 0; i < worker.io.size(); ++i) {
                const auto &io = worker.io[i];
                epoll_event event{}; event.events = io.interest; event.data.u64 = i + 1;
                if (::epoll_ctl(worker.poller.get(), EPOLL_CTL_ADD, io.port->fd.get(), &event) < 0)
                    throw PollSetupError(errno);
            }
        }
        return plan;
    }

    static std::uint64_t discard_pending(Port &port, const Plan *next = nullptr) {
        std::size_t kept = 0;
        std::uint64_t dropped = 0;
        for (std::size_t i = 0; i < port.pending_count; ++i) {
            const auto frame = port.pending[i];
            if (next && next->tx_owner.count(&port) && next->tx_owner.count(frame.source))
                port.pending[kept++] = frame;
            else { frame.buffer->release(); ++dropped; }
        }
        port.pending_count = kept;
        return dropped;
    }

  public:
    Engine(const Config &config, std::size_t budget) : config_(config) {
        for (std::size_t i = 0; i < budget; ++i) {
            auto worker = std::make_unique<Worker>();
            worker->name = "tomtom-mp-" + std::to_string(i);
            workers_.push_back(std::move(worker));
        }
        plan_ = make_plan({}, config_.ruleset);
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
            shutdown_drops_ += discard_pending(*port);
        for (auto &link : plan_->links)
            shutdown_drops_ += link.second->discard();
    }
    int event_fd() const { return control_wake_.fd(); }
    void drain_events() { control_wake_.drain(); }
    void relay_maintenance() {
        const auto ports = plan_->ports;
        for (const auto& port : ports) for (unsigned i=0;i<16;++i) {
            auto* buffer = port->relay_control.pop(); if (!buffer) break;
            struct Release { Buffer* p; ~Release() { p->release(); } } release{buffer};
            relay::View record;
            if (!config_.ruleset || !relay::decode(buffer->data,buffer->size,record)) continue;
            std::vector<std::string> occupied;
            for (const auto& other : ports) if (other != port) {
                occupied.push_back(other->name);
                if (other->relay_registry.live()) for (const auto& c : other->relay_registry.directory.channels) occupied.push_back(c.second.name);
            }
            auto candidate = port->relay_registry; bool changed = false;
            if (!relay::update(candidate,*config_.ruleset,port->name,record,occupied,changed)) continue;
            auto previous = std::move(port->relay_registry); port->relay_registry = std::move(candidate);
            try { if (changed) replace(ports); }
            catch (...) { port->relay_registry = std::move(previous); throw; }
            port->relay_expires.store(std::chrono::duration_cast<std::chrono::milliseconds>(port->relay_registry.expires.time_since_epoch()).count(),std::memory_order_relaxed);
            const auto ack = relay::encode(relay::Type::acknowledged,record.channel,record.epoch);
            // Legacy SEQPACKET records are atomic. No direction-owned Transport
            // state is touched by this bounded control-plane send.
            (void)::send(port->fd.get(),ack.data(),ack.size(),MSG_DONTWAIT|MSG_NOSIGNAL);
        }
    }

    const Plan &plan() const { return *plan_; } // Main only, except direction-owned fields.

    void replace(std::vector<std::shared_ptr<Port>> ports) {
        // All allocation (including private lookup tables and poll scratch)
        // precedes the barrier. Failure leaves the current plan fully usable.
        publish(prepare(std::move(ports)));
    }
    std::unique_ptr<Plan> prepare(std::vector<std::shared_ptr<Port>> ports) {
        return make_plan(std::move(ports), config_.ruleset);
    }
    std::unique_ptr<Plan> prepare_rules(std::shared_ptr<const SwitchRuleset> ruleset) {
        return make_plan(plan_->ports, std::move(ruleset));
    }
    void publish(std::unique_ptr<Plan> next, bool rules_changed = false) {
        // ACTIVE has been queued. Everything below is allocation-free.
        const auto began = Clock::now();
        pause();
        for (auto &port : plan_->ports)
            reconfiguration_drops_ += discard_pending(*port, rules_changed ? nullptr : next.get());
        for (auto &link : plan_->links)
            if (rules_changed || !next->links.count(link.first))
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
        if (plan_->via_path) out << "via_enabled=1\n";
        if (config_.divert_config) out << config_.divert_config->command("divert.show");
        out << "scheduler_version=" << plan_->version << "\nworkers_pool=" << workers_.size()
            << "\nworker_io_backend=epoll\nworker_event_refresh_budget=32\nworker_cpu_sample_ms=100"
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
            if (port.transport) port.transport->write_stats(out, "port_" + std::to_string(i) + "_ipc_");
            out << "port_" << i << "_name=" << port.name << "\nport_" << i
                << "_generation=" << port.generation << "\nport_" << i
                << "_kind=" << kind_name(plan_->kinds[i]) << "\nport_" << i
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
