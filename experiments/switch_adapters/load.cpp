// Based on tests/ipc_scale.cpp. Uses the real SwitchClient.
// Multiple adapter IPC endpoints, with fixed tunnels and measured per-port
// delivery.
#include "switch_client.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <sched.h>
#include <sstream>
#include <sys/prctl.h>
#include <thread>
#include <time.h>
using Clock = std::chrono::steady_clock;
// Set once, before any worker starts; immutable throughout the run.
static unsigned tunnels = 8, adapters = 1, ports = 9;
static std::string direction = "duplex", shape = "equal";
static std::vector<unsigned> input_weights, output_weights;
static unsigned port_weight(unsigned port) { return input_weights[port]; }
static double cpu_now() {
  timespec t{};
  ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}
static std::uint64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}
static void pin(int cpu) {
  cpu_set_t m;
  CPU_ZERO(&m);
  CPU_SET(cpu, &m);
  if (::sched_setaffinity(0, sizeof(m), &m))
    throw std::runtime_error("affinity");
}
static std::vector<int> cpulist(const char *s) {
  std::istringstream in(s);
  std::string t;
  std::vector<int> v;
  while (std::getline(in, t, ','))
    v.push_back(std::stoi(t));
  return v;
}
static unsigned target(unsigned src, std::uint64_t seq) {
  if (src < tunnels)
    return tunnels + src % adapters;
  return ((seq - 1) % (tunnels / adapters)) * adapters + src - tunnels;
}
static std::size_t bytes(unsigned size, std::uint64_t seq) {
  return size ? size : (seq % 3 == 0 ? 64 : 9000);
}
static std::vector<std::vector<unsigned>>
groups(unsigned n, const std::vector<unsigned> &weights) {
  std::vector<std::vector<unsigned>> g(n);
  std::vector<unsigned> w(n), order(ports);
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
    return weights[a] > weights[b];
  });
  for (auto p : order) {
    if (!weights[p])
      continue;
    auto j = std::min_element(w.begin(), w.end()) - w.begin();
    g[j].push_back(p);
    w[j] += weights[p];
  }
  return g;
}
struct alignas(64) Sender {
  std::vector<std::uint64_t> sent, backpressure;
  Sender() : sent(ports), backpressure(ports) {}
  std::uint64_t offered = 0;
  double cpu = 0;
};
struct alignas(64) Receiver {
  std::vector<std::uint64_t> received;
  Receiver() : received(ports), port_latency(ports) {}
  std::vector<std::vector<std::uint64_t>> port_latency;
  std::uint64_t bytes = 0, invalid = 0, max_latency = 0;
  std::vector<std::uint64_t> latency;
  double cpu = 0;
};
int main(int argc, char **argv) {
  if (argc != 12)
    return 2;
  const bool poll_source = true;
  unsigned size = std::stoul(argv[2]);
  double duration = std::stod(argv[3]), rate = std::stod(argv[4]);
  auto source_cpus = cpulist(argv[5]), sink_cpus = cpulist(argv[6]);
  bool verify_all = std::stoi(argv[7]);
  tunnels = std::stoul(argv[8]);
  adapters = std::stoul(argv[9]);
  direction = argv[10];
  shape = argv[11];
  ports = tunnels + adapters;
  if (!tunnels || !adapters || ports > 128 || tunnels % adapters ||
      (direction != "duplex" && direction != "up" && direction != "down") ||
      (shape != "equal" && shape != "hot") ||
      (size && (size < 40 || size > 65535)) || duration <= 0 || rate < 0 ||
      source_cpus.empty() || sink_cpus.empty())
    return 2;
  input_weights.resize(ports);
  output_weights.resize(ports);
  for (unsigned i = 0; i < ports; ++i) {
    bool is_adapter = i >= tunnels;
    unsigned a = is_adapter ? i - tunnels : i % adapters;
    unsigned share =
        shape == "hot" && adapters > 1 && a == 0 ? 4 * (adapters - 1) : 1;
    unsigned weight = share * (is_adapter ? tunnels / adapters : 1);
    input_weights[i] = (direction == "up" && is_adapter) ||
                               (direction == "down" && !is_adapter)
                           ? 0
                           : weight;
    output_weights[i] = (direction == "up" && !is_adapter) ||
                                (direction == "down" && is_adapter)
                            ? 0
                            : weight;
  }
  const unsigned total_weight =
      std::accumulate(input_weights.begin(), input_weights.end(), 0u);
  std::vector<std::unique_ptr<tuntom::SwitchClient>> clients(ports);
  for (unsigned i = 0; i < ports; ++i) {
    clients[i] = std::make_unique<tuntom::SwitchClient>(
        argv[1], i >= tunnels ? "adapter" + std::to_string(i - tunnels)
                              : "tunnel" + std::to_string(i));
    clients[i]->start_connect(Clock::now());
    if (!clients[i]->connected())
      return 3;
  }
  std::cout << "READY\n" << std::flush;
  std::string start;
  if (!std::getline(std::cin, start) || start != "START")
    return 3;
  auto sg = groups(source_cpus.size(), input_weights),
       rg = groups(sink_cpus.size(), output_weights);
  std::vector<Sender> senders(source_cpus.size());
  std::vector<Receiver> receivers(sink_cpus.size());
  std::atomic<bool> go{false}, stop{false}, failed{false};
  std::atomic<unsigned> ready{0};
  Clock::time_point began;
  std::vector<std::thread> sources, sinks;
  for (unsigned k = 0; k < sink_cpus.size(); ++k)
    sinks.emplace_back([&, k] {
      pin(sink_cpus[k]);
      ::prctl(PR_SET_NAME, ("load-sink" + std::to_string(k)).c_str(), 0, 0, 0);
      auto &r = receivers[k];
      r.latency.reserve(200000);
      std::vector<std::uint8_t> wire(65535 + 72);
      std::vector<std::vector<std::uint64_t>> previous(
          ports, std::vector<std::uint64_t>(ports));
      std::vector<pollfd> ds;
      for (auto i : rg[k])
        ds.push_back({clients[i]->fd(), POLLIN, 0});
      ++ready;
      while (!go.load(std::memory_order_acquire))
        std::this_thread::yield();
      double cpu = cpu_now();
      std::size_t cursor = 0;
      while (true) {
        bool progress = false;
        for (std::size_t j = 0; j < rg[k].size(); ++j) {
          auto i = rg[k][cursor];
          cursor = (cursor + 1) % rg[k].size();
          for (unsigned step = 0; step < 16 * output_weights[i]; ++step) {
            auto n = clients[i]->receive(wire.data(), wire.size());
            if (n < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
              break;
            if (n <= 0) {
              ++r.invalid;
              failed.store(true);
              return;
            }
            progress = true;
            tuntom::SwitchFrameView f;
            if (std::size_t(n) > wire.size() ||
                !tuntom::decode_switch_frame(wire.data(), n, f) ||
                f.label_count != 1 || f.payload_size < 40) {
              ++r.invalid;
              continue;
            }
            auto src = tuntom::load_be64(f.payload),
                 seq = tuntom::load_be64(f.payload + 8),
                 stamp = tuntom::load_be64(f.payload + 16);
            bool ok = src < ports && seq && target(src, seq) == i &&
                      f.label(0) == 1000 + src &&
                      f.opcode == (i >= tunnels
                                       ? tuntom::SwitchOpcode::exit_packet
                                       : tuntom::SwitchOpcode::switch_packet) &&
                      f.payload_size == bytes(size, seq);
            if (ok)
              ok = seq > previous[src][i] &&
                   tuntom::load_be64(f.payload + f.payload_size - 16) == src &&
                   tuntom::load_be64(f.payload + f.payload_size - 8) == seq;
            if (ok && verify_all)
              for (std::size_t x = 24; x < f.payload_size - 16; ++x)
                if (f.payload[x] != 42) {
                  ok = false;
                  break;
                }
            if (!ok) {
              ++r.invalid;
              continue;
            }
            previous[src][i] = seq;
            ++r.received[i];
            r.bytes += f.payload_size;
            auto elapsed = now_ns() - stamp;
            r.max_latency = std::max(r.max_latency, elapsed);
            if (seq % 32 == 0) {
              r.latency.push_back(elapsed);
              r.port_latency[i].push_back(elapsed);
            }
          }
        }
        if (!progress) {
          if (stop.load())
            break;
          ::poll(ds.data(), ds.size(), 10);
        }
      }
      r.cpu = cpu_now() - cpu;
    });
  for (unsigned k = 0; k < source_cpus.size(); ++k)
    sources.emplace_back([&, k] {
      pin(source_cpus[k]);
      ::prctl(PR_SET_NAME, ("load-src" + std::to_string(k)).c_str(), 0, 0, 0);
      auto &s = senders[k];
      std::vector<unsigned> slots;
      for (auto p : sg[k])
        for (unsigned j = 0; j < port_weight(p); ++j)
          slots.push_back(p);
      std::vector<std::uint64_t> sequences(ports);
      std::vector<bool> blocked(ports);
      std::vector<pollfd> writable;
      writable.reserve(sg[k].size());
      std::vector<std::uint8_t> payload(size ? size : 9000, 42);
      std::size_t previous_size = 0;
      ++ready;
      while (!go.load(std::memory_order_acquire))
        std::this_thread::yield();
      double cpu = cpu_now();
      auto deadline = began + std::chrono::duration<double>(duration);
      std::uint64_t rounds = 0;
      while (Clock::now() < deadline && !failed.load()) {
        // Saturation is a capacity probe, not a tight loop hammering full
        // socket locks with millions of EAGAIN syscalls. Rate-limited tests
        // retain tuntom's drop-on-EAGAIN source behavior. "flood" reproduces
        // the intentionally hostile preliminary saturation generator.
        if (!rate && poll_source) {
          writable.clear();
          for (auto i : sg[k])
            if (blocked[i])
              writable.push_back({clients[i]->fd(), POLLOUT, 0});
          if (!writable.empty()) {
            ::poll(writable.data(), writable.size(),
                   writable.size() == sg[k].size() ? 10 : 0);
            std::size_t slot = 0;
            for (auto i : sg[k])
              if (blocked[i]) {
                if (writable[slot].revents & POLLOUT)
                  blocked[i] = false;
                ++slot;
              }
          }
          if (Clock::now() >= deadline)
            break;
        }
        if (rate) {
          auto next = began + std::chrono::duration<double>(
                                  rounds * total_weight / rate);
          std::this_thread::sleep_until(next);
          if (Clock::now() >= deadline)
            break;
        }
        ++rounds;
        for (auto i : slots) {
          if (!rate && poll_source && blocked[i])
            continue;
          auto seq = ++sequences[i];
          auto dest = target(i, seq);
          std::uint64_t label = i >= tunnels ? 100 + dest : 18;
          auto nbytes = bytes(size, seq);
          // Restore locations occupied by the preceding packet's tail.
          // The payload is reused exactly as in tuntom's scatter/gather IPC.
          if (previous_size)
            std::fill(payload.begin() + previous_size - 16,
                      payload.begin() + previous_size, 42);
          previous_size = nbytes;
          tuntom::store_be64(payload.data(), i);
          tuntom::store_be64(payload.data() + 8, seq);
          tuntom::store_be64(payload.data() + 16, now_ns());
          tuntom::store_be64(payload.data() + nbytes - 16, i);
          tuntom::store_be64(payload.data() + nbytes - 8, seq);
          ++s.offered;
          auto n = clients[i]->send_frame(tuntom::SwitchOpcode::switch_packet,
                                          &label, 1, payload.data(), nbytes);
          if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ++s.backpressure[i];
            blocked[i] = true;
          } else if (n != ssize_t(nbytes + 16)) {
            failed.store(true);
            break;
          } else
            ++s.sent[i];
        }
      }
      s.cpu = cpu_now() - cpu;
    });
  while (ready.load() != sources.size() + sinks.size())
    std::this_thread::yield();
  began = Clock::now();
  go.store(true, std::memory_order_release);
  for (auto &t : sources)
    t.join();
  double elapsed = std::chrono::duration<double>(Clock::now() - began).count();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto &t : sinks)
    t.join();
  std::vector<std::uint64_t> sent(ports), received(ports), backpressure(ports);
  std::uint64_t offered = 0, invalid = 0, total_bytes = 0, max_latency = 0;
  double source_cpu = 0, sink_cpu = 0;
  std::vector<std::uint64_t> latency;
  std::vector<double> port_p99(ports);
  for (auto &s : senders) {
    for (unsigned i = 0; i < ports; ++i) {
      sent[i] += s.sent[i];
      backpressure[i] += s.backpressure[i];
    }
    offered += s.offered;
    source_cpu += s.cpu;
  }
  for (auto &r : receivers) {
    for (unsigned i = 0; i < ports; ++i) {
      received[i] += r.received[i];
      auto &samples = r.port_latency[i];
      if (!samples.empty()) {
        std::sort(samples.begin(), samples.end());
        port_p99[i] = samples[std::size_t(.99 * (samples.size() - 1))] / 1000.0;
      }
    }
    invalid += r.invalid;
    total_bytes += r.bytes;
    max_latency = std::max(max_latency, r.max_latency);
    sink_cpu += r.cpu;
    latency.insert(latency.end(), r.latency.begin(), r.latency.end());
  }
  std::sort(latency.begin(), latency.end());
  auto pct = [&](double q) {
    return latency.empty()
               ? 0.0
               : latency[std::size_t(q * (latency.size() - 1))] / 1000.0;
  };
  auto arr = [&](const auto &a) {
    std::cout << '[';
    for (unsigned i = 0; i < a.size(); ++i)
      std::cout << (i ? "," : "") << a[i];
    std::cout << ']';
  };
  std::cout << std::setprecision(9) << "{\"sent_by_port\":";
  arr(sent);
  std::cout << ",\"received_by_port\":";
  arr(received);
  std::cout << ",\"source_backpressure_by_port\":";
  arr(backpressure);
  std::cout << ",\"source_cpu_by_thread\":[";
  for (unsigned i = 0; i < senders.size(); ++i)
    std::cout << (i ? "," : "") << senders[i].cpu;
  std::cout << "],\"sink_cpu_by_thread\":[";
  for (unsigned i = 0; i < receivers.size(); ++i)
    std::cout << (i ? "," : "") << receivers[i].cpu;
  std::cout << "],\"ports\":" << ports << ",\"direction\":\"" << direction
            << "\""
            << ",\"shape\":\"" << shape << "\""
            << ",\"adapters\":" << adapters << ",\"tunnels\":" << tunnels
            << ",\"offered\":" << offered
            << ",\"received_bytes\":" << total_bytes
            << ",\"elapsed_s\":" << elapsed << ",\"invalid\":" << invalid
            << ",\"source_cpu_s\":" << source_cpu
            << ",\"sink_cpu_s\":" << sink_cpu
            << ",\"latency_samples\":" << latency.size()
            << ",\"latency_p50_us\":" << pct(.5)
            << ",\"latency_p99_us\":" << pct(.99)
            << ",\"latency_max_us\":" << max_latency / 1000.0;
  std::cout << ",\"latency_p99_by_port_us\":";
  arr(port_p99);
  std::cout << ",\"input_weights\":";
  arr(input_weights);
  std::cout << ",\"output_weights\":";
  arr(output_weights);
  std::cout << ",\"source_port_groups\":[";
  for (unsigned i = 0; i < sg.size(); ++i) {
    if (i)
      std::cout << ',';
    arr(sg[i]);
  }
  std::cout << "],\"sink_port_groups\":[";
  for (unsigned i = 0; i < rg.size(); ++i) {
    if (i)
      std::cout << ',';
    arr(rg[i]);
  }
  std::cout << "]}\n";
  return failed || invalid || latency.empty() ? 1 : 0;
}
