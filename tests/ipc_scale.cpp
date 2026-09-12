// Manual capacity benchmark: ten tunnel ports and one exit adapter, full duplex.
// Build with -O3 -std=c++17 -pthread -Isrc; run via ipc_scale.py.
#include "switch_client.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sched.h>
#include <thread>
#include <time.h>

using Clock = std::chrono::steady_clock;
constexpr unsigned ports = 11;

static double thread_seconds() {
    timespec t {};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static std::uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}

static void pin(int cpu) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    if (::sched_setaffinity(0, sizeof(mask), &mask))
        throw std::runtime_error("CPU affinity failed");
}

static unsigned destination(unsigned source, std::uint64_t sequence, unsigned adapter_weight) {
    return source == 10 ? (sequence - 1) % 10 :
        (sequence % (10 / adapter_weight) == 0 ? 10 : (source ^ 1));
}

static std::size_t payload_size(unsigned size, std::uint64_t sequence) {
    // size=0 models two jumbo packets per small reverse/ACK-sized packet.
    return size ? size : (sequence % 3 == 0 ? 64 : 9000);
}

int main(int argc, char** argv) {
    if (argc != 8) return 2;
    const unsigned size = std::stoul(argv[2]);
    const double duration = std::stod(argv[3]), rate = std::stod(argv[4]);
    const unsigned adapter_weight = std::stoul(argv[7]);
    if ((size != 0 and (size < 24 or size > 65535)) or duration <= 0 or rate < 0)
        return 2;
    if (adapter_weight != 1 and adapter_weight != 2) return 2;
    const unsigned slots = 10 + adapter_weight;
    pin(std::stoi(argv[5]));
    std::array<std::unique_ptr<tuntom::SwitchClient>, ports> clients;
    for (unsigned i = 0; i < ports; ++i) {
        clients[i] = std::make_unique<tuntom::SwitchClient>(argv[1],
            i == 10 ? "adapter" : "tunnel" + std::to_string(i));
        clients[i]->start_connect(Clock::now());
        if (not clients[i]->connected()) return 3;
    }
    // Registration readiness is confirmed by a control query in the runner.
    std::cout << "READY\n" << std::flush;
    std::string start;
    if (not std::getline(std::cin, start) or start != "START") return 3;

    std::atomic<bool> stop {false};
    std::atomic<bool> consumer_ready {false};
    std::array<std::uint64_t, ports> sent {}, received {}, backpressure {};
    std::uint64_t invalid = 0, received_bytes = 0, maximum_latency_ns = 0;
    std::vector<std::uint64_t> latency;
    latency.reserve(200000);
    double receive_cpu = 0;
    std::thread consumer([&] {
        pin(std::stoi(argv[6]));
        const double began = thread_seconds();
        std::vector<std::uint8_t> wire(65535 + 72);
        std::array<std::array<std::uint64_t, ports>, ports> previous {};
        std::array<pollfd, ports> descriptors {};
        for (unsigned i = 0; i < ports; ++i)
            descriptors[i] = {clients[i]->fd(), POLLIN, 0};
        consumer_ready.store(true);
        while (true) {
            bool progress = false;
            // Drain bounded bursts so a switch batch does not cause ten empty
            // socket probes for every record received on its active output.
            for (unsigned i = 0; i < ports; ++i) {
                const unsigned quota = 16 * (i == 10 ? adapter_weight : 1);
                for (unsigned step = 0; step < quota; ++step) {
                    const auto n = clients[i]->receive(wire.data(), wire.size());
                    if (n < 0 and (errno == EAGAIN or errno == EWOULDBLOCK or errno == EINTR))
                        break;
                    if (n <= 0) { ++invalid; stop.store(true); return; }
                    progress = true;
                    tuntom::SwitchFrameView frame;
                    if (static_cast<std::size_t>(n) > wire.size() or
                        not tuntom::decode_switch_frame(wire.data(), n, frame) or
                        frame.label_count != 1 or frame.payload_size < 24) {
                        ++invalid;
                        continue;
                    }
                    const auto source = tuntom::load_be64(frame.payload);
                    const auto sequence = tuntom::load_be64(frame.payload + 8);
                    const auto timestamp = tuntom::load_be64(frame.payload + 16);
                    if (source >= ports or sequence == 0 or
                        destination(source, sequence, adapter_weight) != i or frame.label(0) != 1000 + source or
                        frame.opcode != (i == 10 ? tuntom::SwitchOpcode::exit_packet :
                                                  tuntom::SwitchOpcode::switch_packet) or
                        frame.payload_size != payload_size(size, sequence) or
                        sequence <= previous[source][i]) {
                        ++invalid;
                        continue;
                    }
                    previous[source][i] = sequence;
                    ++received[i];
                    received_bytes += frame.payload_size;
                    const auto elapsed = now_ns() - timestamp;
                    maximum_latency_ns = std::max(maximum_latency_ns, elapsed);
                    if (sequence % 32 == 0) latency.push_back(elapsed);
                }
            }
            if (not progress) {
                if (stop.load()) break;
                ::poll(descriptors.data(), descriptors.size(), 10);
            }
        }
        receive_cpu = thread_seconds() - began;
    });

    std::vector<std::uint8_t> payload(size ? size : 9000, 42);
    while (not consumer_ready.load()) std::this_thread::yield();
    const auto began = Clock::now();
    const auto deadline = began + std::chrono::duration<double>(duration);
    const double cpu = thread_seconds();
    std::uint64_t rounds = 0;
    std::array<std::uint64_t, ports> sequences {};
    bool failed = false;
    while (Clock::now() < deadline and not stop.load()) {
        if (rate) {
            const auto next = began + std::chrono::duration<double>(rounds * slots / rate);
            std::this_thread::sleep_until(next);
            if (Clock::now() >= deadline) break;
        }
        ++rounds;
        for (unsigned slot = 0; slot < slots; ++slot) {
            const unsigned i = std::min(slot, 10U);
            const auto sequence = ++sequences[i];
            const auto target = destination(i, sequence, adapter_weight);
            const std::uint64_t label = i == 10 ? 100 + target : (target == 10 ? 18 : 17);
            tuntom::store_be64(payload.data(), i);
            tuntom::store_be64(payload.data() + 8, sequence);
            tuntom::store_be64(payload.data() + 16, now_ns());
            const auto bytes = payload_size(size, sequence);
            const auto n = clients[i]->send_frame(tuntom::SwitchOpcode::switch_packet,
                                                  &label, 1, payload.data(), bytes);
            if (n < 0 and (errno == EAGAIN or errno == EWOULDBLOCK)) ++backpressure[i];
            else if (n != static_cast<ssize_t>(bytes + 16)) { failed = true; break; }
            else ++sent[i];
        }
        if (failed) break;
    }
    const double send_cpu = thread_seconds() - cpu;
    const double elapsed = std::chrono::duration<double>(Clock::now() - began).count();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true);
    consumer.join();
    std::sort(latency.begin(), latency.end());
    const auto percentile_us = [&](double fraction) {
        return latency.empty() ? 0.0 : latency[static_cast<std::size_t>(fraction * (latency.size()-1))] / 1000.0;
    };
    const auto array_json = [](const auto& array) {
        std::cout << '[';
        for (unsigned i = 0; i < ports; ++i) std::cout << (i ? "," : "") << array[i];
        std::cout << ']';
    };
    std::cout << std::setprecision(9) << "{\"sent_by_port\":";
    array_json(sent);
    std::cout << ",\"received_by_port\":";
    array_json(received);
    std::cout << ",\"source_backpressure_by_port\":";
    array_json(backpressure);
    std::cout << ",\"offered\":" << rounds * slots
              << ",\"received_bytes\":" << received_bytes
              << ",\"elapsed_s\":" << elapsed << ",\"invalid\":" << invalid
              << ",\"source_cpu_s\":" << send_cpu << ",\"sink_cpu_s\":" << receive_cpu
              << ",\"latency_samples\":" << latency.size()
              << ",\"latency_p50_us\":" << percentile_us(0.50)
              << ",\"latency_p99_us\":" << percentile_us(0.99)
              << ",\"latency_max_us\":" << maximum_latency_ns / 1000.0 << "}\n";
    return failed or invalid or latency.empty() ? 1 : 0;
}
