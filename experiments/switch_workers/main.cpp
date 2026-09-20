// EXPERIMENTAL shared RX/TX workers. Never installed by mk_switch.sh.
// Real tuntom IPC v1, the production route key/hash and forwarding semantics.
// Registration is completed before workers start; reconnect is intentionally
// outside this throughput draft. FDs and pools live until all workers join.
#include "../switch_mt/queues.hpp"
#include "common.hpp"
#include "control_socket.hpp"
#include "ipc/switch_protocol.hpp"
#include "poll_wake.hpp"
#include <algorithm>
#include <csignal>
#include <iostream>
#include <poll.h>
#include <sched.h>
#include <sstream>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using namespace switch_mt;
using Clock = std::chrono::steady_clock;
static volatile std::sig_atomic_t interrupted = 0;
static void stop_signal(int) { interrupted = 1; }

// Kept byte-for-byte equivalent in behavior to src/switch/main.cpp.
struct RouteKey {
  std::string port;
  std::uint64_t label = 0;
  bool operator==(const RouteKey &other) const {
    return port == other.port && label == other.label;
  }
};
struct RouteKeyHash {
  std::size_t operator()(const RouteKey &key) const {
    const auto label_hash = std::hash<std::uint64_t>{}(key.label);
    const auto port_hash = std::hash<std::string>{}(key.port);
    return label_hash ^
           (port_hash + 0x9e3779b9U + (label_hash << 6) + (label_hash >> 2));
  }
};
struct Target {
  std::string port;
  std::uint64_t label;
};
using Routes = std::unordered_map<RouteKey, Target, RouteKeyHash>;
using Queue = Spsc<Buffer>;

// One writer per statistics block; relaxed atomics allow control snapshots.
// These are stores of a local running total, not shared fetch_add counters.
struct alignas(64) Stats {
#define COUNTERS(X)                                                            \
  X(frames_rx)                                                                 \
  X(bytes_rx)                                                                  \
  X(frames_tx)                                                                 \
  X(bytes_tx) X(route_hits) X(route_misses) X(target_disconnected)             \
      X(default_back) X(exit_deliveries) X(malformed_frames) X(send_errors)    \
          X(send_backpressure_drops) X(queue_full_drops) X(send_eagain)        \
              X(pool_stalls) X(pool_probes) X(wake_calls) X(rx_errors)         \
                  X(shutdown_drops)
#define FIELD(n) std::atomic<std::uint64_t> n{0};
  COUNTERS(FIELD)
#undef FIELD
  static void add(std::atomic<std::uint64_t> &c, std::uint64_t n = 1) {
    c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
  }
};
struct Port {
  std::string name;
  int fd = -1;
  std::unique_ptr<Pool> pool;
  std::vector<std::unique_ptr<Queue>> incoming;
  std::size_t rx_owner = 0, tx_owner = 0;
  Stats rx, tx;
  std::atomic<bool> disconnected{false};
  Port(std::string id, std::size_t pool_size)
      : name(std::move(id)), pool(new Pool(pool_size)) {}
};
struct alignas(64) Worker {
  std::vector<std::size_t> ports;
  std::unique_ptr<PollWake> wake;
  std::atomic<double> cpu{0};
  std::atomic<std::uint64_t> polls{0};
};
static double cpu_now() {
  timespec t{};
  ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}
static void pin(int cpu) {
  if (cpu < 0)
    return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (::sched_setaffinity(0, sizeof(set), &set))
    throw std::runtime_error("worker affinity failed");
}
static int listener(const std::string &path) {
  if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
    throw std::runtime_error("socket path");
  int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    throw std::runtime_error("socket: " + std::string(std::strerror(errno)));
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) ||
      ::chmod(path.c_str(), 0660) || ::listen(fd, 64)) {
    ::close(fd);
    throw std::runtime_error("listen: " + std::string(std::strerror(errno)));
  }
  return fd;
}
static std::pair<std::string, std::uint64_t> endpoint(const std::string &s) {
  auto at = s.rfind(':');
  if (at == s.npos || at == 0)
    throw std::runtime_error("route endpoint");
  std::size_t used = 0;
  auto label = std::stoull(s.substr(at + 1), &used, 0);
  if (used != s.size() - at - 1 || s[at + 1] == '-')
    throw std::runtime_error("label");
  return {s.substr(0, at), label};
}

int main(int argc, char **argv) {
  std::string path, control_path, stats_file;
  Routes routes;
  std::unordered_set<std::string> exits;
  std::vector<std::string> names;
  std::vector<int> cpus;
  std::size_t rx_count = 1, tx_count = 1;
  std::vector<std::string> dedicated_rx, dedicated_tx;
  std::unordered_map<std::string, std::size_t> explicit_rx, explicit_tx;
  std::size_t pool_size = 128, queue_size = 128;
  bool default_back = false, retry = true, spin = false;
  auto name = [&](const std::string &s) {
    if (std::find(names.begin(), names.end(), s) == names.end())
      names.push_back(s);
  };
  try {
    for (int i = 1; i < argc; ++i) {
      std::string key = argv[i];
      auto value = [&]() {
        if (++i >= argc)
          throw std::runtime_error("missing value");
        return std::string(argv[i]);
      };
      if (key == "--socket")
        path = value();
      else if (key == "--control-socket")
        control_path = value();
      else if (key == "--rx-workers")
        rx_count = std::stoul(value());
      else if (key == "--tx-workers")
        tx_count = std::stoul(value());
      else if (key == "--dedicated-rx")
        dedicated_rx.push_back(value());
      else if (key == "--dedicated-tx")
        dedicated_tx.push_back(value());
      else if (key == "--rx-owner" || key == "--tx-owner") {
        auto owner = endpoint(value());
        auto &mapping = key == "--rx-owner" ? explicit_rx : explicit_tx;
        if (!mapping.emplace(owner.first, owner.second).second)
          throw std::runtime_error("duplicate explicit owner");
      } else if (key == "--stats-file")
        stats_file = value();
      else if (key == "--pool-size")
        pool_size = std::stoul(value());
      else if (key == "--queue-size")
        queue_size = std::stoul(value());
      else if (key == "--port")
        name(value());
      else if (key == "--exit-port") {
        auto v = value();
        exits.insert(v);
        name(v);
      } else if (key == "--route") {
        auto v = value();
        auto at = v.find('=');
        if (at == v.npos)
          throw std::runtime_error("route assignment");
        auto a = endpoint(v.substr(0, at)), b = endpoint(v.substr(at + 1));
        name(a.first);
        name(b.first);
        if (!routes
                 .emplace(RouteKey{a.first, a.second},
                          Target{b.first, b.second})
                 .second)
          throw std::runtime_error("duplicate route");
      } else if (key == "--worker-cpus") {
        std::istringstream in(value());
        std::string item;
        while (std::getline(in, item, ',')) {
          int c = std::stoi(item);
          if (c < 0 || c >= CPU_SETSIZE)
            throw std::runtime_error("CPU range");
          cpus.push_back(c);
        }
      } else if (key == "--backpressure") {
        auto v = value();
        if (v != "retry" && v != "drop")
          throw std::runtime_error("backpressure");
        retry = v == "retry";
      } else if (key == "--idle") {
        auto v = value();
        if (v != "sleep" && v != "spin")
          throw std::runtime_error("idle");
        spin = v == "spin";
      } else if (key == "--default-back=on")
        default_back = true;
      else if (key == "--default-back=off")
        default_back = false;
      else
        throw std::runtime_error("unknown option " + key);
    }
    if (names.empty() || pool_size == 0 || pool_size > 4096 ||
        queue_size == 0 || queue_size > 65536)
      throw std::runtime_error("invalid capacity/topology");
    ::signal(SIGINT, stop_signal);
    ::signal(SIGTERM, stop_signal);
    ::signal(SIGPIPE, SIG_IGN);
    const int listen_fd = listener(path);
    std::unique_ptr<tuntom::ControlSocket> control;
    if (!control_path.empty())
      control.reset(new tuntom::ControlSocket(control_path));
    std::vector<std::unique_ptr<Port>> ports;
    for (auto &n : names)
      ports.emplace_back(new Port(n, pool_size));
    for (auto &p : ports)
      for (std::size_t i = 0; i < ports.size(); ++i)
        p->incoming.emplace_back(new Queue(queue_size));
    auto make_workers = [&](std::size_t count,
                            const std::vector<std::string> &dedicated,
                            bool rx) {
      const auto &mapping = rx ? explicit_rx : explicit_tx;
      if (!count || count > ports.size() || dedicated.size() > count ||
          (dedicated.size() < ports.size() && dedicated.size() == count))
        throw std::runtime_error("invalid worker allocation");
      if (!mapping.empty()) {
        if (!dedicated.empty() || mapping.size() != ports.size())
          throw std::runtime_error(
              "explicit owners need all ports and no dedicated options");
        for (const auto &item : mapping)
          if (std::find(names.begin(), names.end(), item.first) ==
                  names.end() ||
              item.second >= count)
            throw std::runtime_error("invalid explicit owner");
      }
      std::vector<std::unique_ptr<Worker>> workers;
      for (std::size_t i = 0; i < count; ++i) {
        auto worker = std::make_unique<Worker>();
        if (!rx)
          worker->wake = std::make_unique<PollWake>();
        workers.push_back(std::move(worker));
      }
      std::unordered_set<std::string> seen;
      for (auto &d : dedicated)
        if (std::find(names.begin(), names.end(), d) == names.end() ||
            !seen.insert(d).second)
          throw std::runtime_error("unknown/duplicate dedicated port");
      std::size_t next = 0;
      for (std::size_t i = 0; i < ports.size(); ++i) {
        auto it = std::find(dedicated.begin(), dedicated.end(), ports[i]->name);
        const auto owner =
            !mapping.empty() ? mapping.at(ports[i]->name)
            : it != dedicated.end()
                ? std::size_t(it - dedicated.begin())
                : dedicated.size() + next++ % (count - dedicated.size());
        workers[owner]->ports.push_back(i);
        (rx ? ports[i]->rx_owner : ports[i]->tx_owner) = owner;
      }
      for (const auto &worker : workers)
        if (worker->ports.empty())
          throw std::runtime_error("worker has no assigned ports");
      return workers;
    };
    auto rx_workers = make_workers(rx_count, dedicated_rx, true);
    auto tx_workers = make_workers(tx_count, dedicated_tx, false);
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    std::size_t registered = 0, rejected = 0;
    bool started = false;
    auto snapshot = [&] {
      std::ostringstream out;
      out << "format=txt\nformat_version=1\ncomponent=switch-workers-"
             "draft\nconnections_current="
          << registered << "\nregistrations_ok=" << registered
          << "\nregistrations_invalid=" << rejected
          << "\nworkers_started=" << started << "\nports=" << ports.size()
          << "\nrx_workers=" << rx_count << "\ntx_workers=" << tx_count
          << "\npool_size=" << pool_size << "\nqueue_size=" << queue_size
          << "\n";
#define SUM(n)                                                                 \
  {                                                                            \
    std::uint64_t sum = 0;                                                     \
    for (auto &p : ports)                                                      \
      sum += p->rx.n.load(std::memory_order_relaxed) +                         \
             p->tx.n.load(std::memory_order_relaxed);                          \
    out << #n << "=" << sum << "\n";                                           \
  }
      COUNTERS(SUM)
#undef SUM
      std::size_t in_use = 0;
      for (std::size_t i = 0; i < ports.size(); ++i) {
        auto &p = *ports[i];
        in_use += p.pool->in_use();
        out << "port_" << i << "_name=" << p.name << "\nport_" << i
            << "_rx=" << p.rx.frames_rx.load() << "\nport_" << i
            << "_tx=" << p.tx.frames_tx.load() << "\nport_" << i
            << "_rx_owner=" << p.rx_owner << "\nport_" << i
            << "_tx_owner=" << p.tx_owner << "\n";
      }
      for (auto rx : {true, false}) {
        auto &workers = rx ? rx_workers : tx_workers;
        const char *direction = rx ? "rx" : "tx";
        for (std::size_t i = 0; i < workers.size(); ++i) {
          out << "worker_" << direction << "_" << i
              << "_cpu_s=" << workers[i]->cpu.load() << "\n";
          out << "worker_" << direction << "_" << i
              << "_poll_calls=" << workers[i]->polls.load() << "\n";
        }
      }
      out << "buffers_in_use=" << in_use << "\n";
      return out.str();
    };
    struct alignas(64) Lookup {
      RouteKey key;
    };
    std::vector<Lookup> lookup_keys;
    for (auto &p : ports)
      lookup_keys.push_back({{p->name, 0}});
    // Route semantics and per-input-port pools/queues match the old draft.
    auto process_frame = [&](std::size_t i, Buffer *buffer, std::size_t n) {
      auto &p = *ports[i];
      auto &s = p.rx;
      auto &lookup = lookup_keys[i].key;
      buffer->size = n;
      tuntom::SwitchFrameView frame;
      if (!tuntom::decode_switch_frame(buffer->data, n, frame) ||
          frame.opcode != tuntom::SwitchOpcode::switch_packet) {
        Stats::add(s.malformed_frames);
        buffer->release();
        buffer = nullptr;
        return;
      }
      Stats::add(s.frames_rx);
      Stats::add(s.bytes_rx, n);
      lookup.label = frame.label(0);
      const auto route = routes.find(lookup);
      std::size_t dest = ports.size();
      if (route != routes.end()) {
        Stats::add(s.route_hits);
        // Preserve the reference switch's linear target-name lookup.
        for (std::size_t j = 0; j < ports.size(); ++j)
          if (ports[j]->name == route->second.port) {
            dest = j;
            break;
          }
        if (dest == ports.size() || ports[dest]->disconnected.load()) {
          Stats::add(s.target_disconnected);
          dest = ports.size();
        } else {
          tuntom::store_be64(buffer->data + tuntom::switch_base_header_size,
                             route->second.label);
          if (exits.count(route->second.port)) {
            buffer->data[1] = std::uint8_t(tuntom::SwitchOpcode::exit_packet);
            Stats::add(s.exit_deliveries);
          }
        }
      } else {
        Stats::add(s.route_misses);
        if (default_back) {
          dest = i;
          buffer->data[1] = std::uint8_t(tuntom::SwitchOpcode::exit_packet);
          Stats::add(s.default_back);
        }
      }
      if (dest != ports.size()) {
        auto &target = *ports[dest];
        if (target.incoming[i]->push(buffer)) {
          buffer = nullptr;
          if (!spin && tx_workers[target.tx_owner]->wake->notify())
            Stats::add(s.wake_calls);
        } else
          Stats::add(s.queue_full_drops);
      }
      if (buffer) {
        buffer->release();
        buffer = nullptr;
      }
    };
    auto rx_loop = [&](std::size_t worker_id) {
      auto &worker = *rx_workers[worker_id];
      pin(cpus.empty() ? -1 : cpus[worker_id % cpus.size()]);
      ::prctl(PR_SET_NAME, ("sw-rx" + std::to_string(worker_id)).c_str(), 0, 0,
              0);
      const double began = cpu_now();
      std::vector<Clock::time_point> retry_at(worker.ports.size());
      std::vector<pollfd> inputs(worker.ports.size());
      std::size_t cursor = 0;
      std::uint64_t polls = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        bool progress = false;
        auto earliest = Clock::time_point::max();
        for (std::size_t step = 0; step < worker.ports.size(); ++step) {
          const auto j = (cursor + step) % worker.ports.size();
          const auto i = worker.ports[j];
          auto &p = *ports[i];
          inputs[j] = {-1, POLLIN, 0};
          if (p.disconnected.load())
            continue;
          if (retry_at[j] != Clock::time_point{} &&
              Clock::now() < retry_at[j]) {
            earliest = std::min(earliest, retry_at[j]);
            continue;
          }
          retry_at[j] = {};
          std::uint64_t probes = 0;
          auto *buffer = p.pool->acquire(probes);
          Stats::add(p.rx.pool_probes, probes);
          if (!buffer) {
            Stats::add(p.rx.pool_stalls);
            retry_at[j] = Clock::now() + std::chrono::microseconds(50);
            earliest = std::min(earliest, retry_at[j]);
            continue;
          }
          inputs[j].fd = p.fd;
          const auto n = ::recv(p.fd, buffer->data, wire_capacity,
                                MSG_DONTWAIT | MSG_TRUNC);
          if (n < 0 &&
              (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            buffer->release();
            continue;
          }
          if (n <= 0 || std::size_t(n) > wire_capacity) {
            if (n < 0)
              Stats::add(p.rx.rx_errors);
            p.disconnected.store(true);
            inputs[j].fd = -1;
            buffer->release();
            continue;
          }
          progress = true;
          process_frame(i, buffer, n);
        }
        cursor = (cursor + 1) % worker.ports.size();
        if (progress || spin)
          continue;
        auto delay = std::chrono::nanoseconds(20000000);
        if (earliest != Clock::time_point::max())
          delay = std::max(
              std::chrono::nanoseconds(0),
              std::min(delay,
                       std::chrono::duration_cast<std::chrono::nanoseconds>(
                           earliest - Clock::now())));
        const timespec timeout{0, delay.count()};
        worker.cpu.store(cpu_now() - began);
        worker.polls.store(++polls, std::memory_order_relaxed);
        ::ppoll(inputs.data(), inputs.size(), &timeout, nullptr);
      }
      worker.cpu.store(cpu_now() - began);
    };
    auto tx_loop = [&](std::size_t worker_id) {
      auto &worker = *tx_workers[worker_id];
      pin(cpus.empty() ? -1 : cpus[(rx_count + worker_id) % cpus.size()]);
      ::prctl(PR_SET_NAME, ("sw-tx" + std::to_string(worker_id)).c_str(), 0, 0,
              0);
      const double began = cpu_now();
      auto &wake = *worker.wake;
      std::vector<Buffer *> pending_buffers(worker.ports.size());
      std::vector<bool> blocked(worker.ports.size());
      std::vector<std::size_t> source_cursor(worker.ports.size());
      std::vector<pollfd> outputs(worker.ports.size() + 1, {-1, POLLOUT, 0});
      outputs[0] = {wake.fd(), POLLIN, 0};
      std::size_t blocked_count = 0, cursor = 0;
      std::uint64_t polls = 0;
      auto unblock = [&] {
        for (std::size_t j = 0; j < worker.ports.size(); ++j)
          if (blocked[j] && outputs[j + 1].revents &
                                (POLLOUT | POLLERR | POLLHUP | POLLNVAL)) {
            blocked[j] = false;
            --blocked_count;
            outputs[j + 1].fd = -1;
          }
      };
      auto sweep = [&] {
        bool progress = false;
        for (std::size_t step = 0; step < worker.ports.size(); ++step) {
          const auto j = (cursor + step) % worker.ports.size();
          auto &p = *ports[worker.ports[j]];
          if (blocked[j])
            continue;
          auto *&pending_buffer = pending_buffers[j];
          if (!pending_buffer) {
            for (std::size_t k = 0; k < ports.size(); ++k) {
              const auto source = source_cursor[j];
              source_cursor[j] = (source + 1) % ports.size();
              if ((pending_buffer = p.incoming[source]->pop()))
                break;
            }
          }
          if (!pending_buffer)
            continue;
          const auto n =
              ::send(p.fd, pending_buffer->data, pending_buffer->size,
                     MSG_DONTWAIT | MSG_NOSIGNAL);
          if (n == ssize_t(pending_buffer->size)) {
            Stats::add(p.tx.frames_tx);
            Stats::add(p.tx.bytes_tx, n);
          } else if (n < 0 && errno == EINTR) {
            progress = true;
            continue;
          } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            Stats::add(p.tx.send_eagain);
            if (retry) {
              blocked[j] = true;
              ++blocked_count;
              outputs[j + 1] = {p.fd, POLLOUT, 0};
              continue;
            }
            Stats::add(p.tx.send_backpressure_drops);
          } else
            Stats::add(p.tx.send_errors);
          pending_buffer->release();
          pending_buffer = nullptr;
          progress = true;
        }
        cursor = (cursor + 1) % worker.ports.size();
        return progress;
      };
      while (!stop.load(std::memory_order_relaxed)) {
        if (blocked_count) {
          worker.polls.store(++polls, std::memory_order_relaxed);
          ::poll(outputs.data(), outputs.size(), 0);
          unblock();
        }
        if (sweep())
          continue;
        if (spin)
          continue;
        wake.arm();
        if (sweep()) {
          wake.cancel();
          continue;
        }
        worker.cpu.store(cpu_now() - began);
        worker.polls.store(++polls, std::memory_order_relaxed);
        ::poll(outputs.data(), outputs.size(), 20);
        wake.cancel();
        wake.drain();
        unblock();
      }
      for (std::size_t j = 0; j < pending_buffers.size(); ++j)
        if (pending_buffers[j]) {
          pending_buffers[j]->release();
          Stats::add(ports[worker.ports[j]]->tx.shutdown_drops);
        }
      worker.cpu.store(cpu_now() - began);
    };
    // Registration/control are a deliberately small fixed-topology harness.
    // This does not claim production admission/reconnect/recovery parity.
    struct Pending {
      int fd;
      Clock::time_point since;
    };
    std::vector<Pending> pending;
    auto deadline = Clock::now() + std::chrono::seconds(15);
    try {
      while (!interrupted) {
        if (!started && registered == ports.size()) {
          for (std::size_t i = 0; i < rx_workers.size(); ++i)
            threads.emplace_back(rx_loop, i);
          for (std::size_t i = 0; i < tx_workers.size(); ++i)
            threads.emplace_back(tx_loop, i);
          started = true;
        }
        if (!started && Clock::now() > deadline)
          throw std::runtime_error("fixed topology registration timeout");
        std::vector<pollfd> ds{{listen_fd, POLLIN, 0},
                               {control ? control->poll_fd() : -1, POLLIN, 0}};
        for (auto &c : pending)
          ds.push_back({c.fd, POLLIN, 0});
        ::poll(ds.data(), ds.size(), 20);
        if (control && (ds[1].revents & POLLIN))
          control->handle(snapshot);
        for (std::size_t j = 0; j < pending.size(); ++j) {
          auto &c = pending[j];
          if (ds[j + 2].revents & POLLIN) {
            std::uint8_t bytes[128];
            auto n =
                ::recv(c.fd, bytes, sizeof(bytes), MSG_DONTWAIT | MSG_TRUNC);
            std::string id;
            bool ok = n > 0 && n <= ssize_t(sizeof(bytes)) &&
                      tuntom::decode_switch_registration(bytes, n, id);
            if (ok) {
              ok = false;
              for (auto &p : ports)
                if (p->name == id && p->fd < 0 && !started) {
                  p->fd = c.fd;
                  c.fd = -1;
                  ++registered;
                  ok = true;
                  break;
                }
            }
            if (!ok) {
              ++rejected;
              ::close(c.fd);
              c.fd = -1;
            }
          } else if ((ds[j + 2].revents & (POLLERR | POLLHUP)) ||
                     Clock::now() - c.since > std::chrono::seconds(5)) {
            ::close(c.fd);
            c.fd = -1;
            ++rejected;
          }
        }
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [](auto &p) { return p.fd < 0; }),
                      pending.end());
        if (ds[0].revents & POLLIN) {
          int fd = ::accept4(listen_fd, nullptr, nullptr,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (fd >= 0) {
            if (pending.size() < 32)
              pending.push_back({fd, Clock::now()});
            else
              ::close(fd);
          }
        }
      }
    } catch (...) {
      stop.store(true);
      for (auto &w : tx_workers)
        w->wake->notify();
      for (auto &t : threads)
        t.join();
      throw;
    }
    stop.store(true);
    for (auto &w : tx_workers)
      w->wake->notify();
    for (auto &t : threads)
      t.join();
    for (auto &p : ports)
      for (auto &q : p->incoming)
        while (auto *b = q->pop()) {
          b->release();
          Stats::add(p->tx.shutdown_drops);
        }
    if (!stats_file.empty()) {
      FILE *f = std::fopen(stats_file.c_str(), "w");
      if (!f)
        throw std::runtime_error("stats file");
      auto s = snapshot();
      std::fwrite(s.data(), 1, s.size(), f);
      std::fclose(f);
    }
    for (auto &p : ports)
      if (p->fd >= 0)
        ::close(p->fd);
    for (auto &p : pending)
      ::close(p.fd);
    ::close(listen_fd);
    ::unlink(path.c_str());
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "switch-mt: " << e.what() << "\n";
    return 1;
  }
}
