// Manual benchmark: build once against the old src/ and once with
// -DTUNTOM_IPC_GATHER against the new src/. Use ipc_bench.py to run both.
#include "switch_client.hpp"
#include <atomic>
#include <iostream>
#include <sched.h>
#include <thread>
#include <time.h>

using Clock = std::chrono::steady_clock;

static double thread_seconds() {
    timespec time {};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) * 1e-9;
}

static void pin(int cpu) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    if (::sched_setaffinity(0, sizeof(mask), &mask) != 0)
        throw std::runtime_error("benchmark CPU affinity failed");
}

int main(int argc, char** argv) {
    if (argc != 7) return 2;
    const auto size = static_cast<std::size_t>(std::stoul(argv[2]));
    const double duration = std::stod(argv[3]);
    const double rate = std::stod(argv[4]);
    if (size < 8 or size > 65535 or duration <= 0 or rate < 0) return 2;
    pin(std::stoi(argv[5]));
    tuntom::SwitchClient source(argv[1], "source"), sink(argv[1], "sink");
    source.start_connect(Clock::now());
    sink.start_connect(Clock::now());
    if (not source.connected() or not sink.connected()) return 3;
    // Allow the benchmark's two registrations to be serviced before timing.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::vector<std::uint8_t> payload(size, 42);
    std::atomic<bool> stop {false};
    std::uint64_t received = 0, invalid = 0;
    double receive_cpu = 0;
    std::thread consumer([&] {
        pin(std::stoi(argv[6]));
        const double cpu = thread_seconds();
        std::vector<std::uint8_t> wire(65535 + 72);
        std::uint64_t previous = 0;
        for (;;) {
            const auto n = sink.receive(wire.data(), wire.size());
            if (n < 0 and (errno == EAGAIN or errno == EWOULDBLOCK)) {
                if (stop.load()) break;
                pollfd ready {sink.fd(), POLLIN, 0};
                ::poll(&ready, 1, 10);
                continue;
            }
            if (n <= 0) { ++invalid; break; }
            tuntom::SwitchFrameView frame;
            if (not tuntom::decode_switch_frame(wire.data(), static_cast<std::size_t>(n), frame) or
                frame.label_count != 1 or frame.label(0) != 99 or frame.payload_size != size) {
                ++invalid;
                continue;
            }
            const auto sequence = tuntom::load_be64(frame.payload);
            if (sequence <= previous) ++invalid;
            previous = sequence;
            ++received;
        }
        receive_cpu = thread_seconds() - cpu;
    });

    std::uint64_t sent = 0, backpressure = 0;
    const auto began = Clock::now();
    const double cpu = thread_seconds();
    const auto deadline = began + std::chrono::duration<double>(duration);
    bool failed = false;
    while (Clock::now() < deadline) {
        if (rate != 0 and sent % 8 == 0) {
            const auto next = began + std::chrono::duration<double>(static_cast<double>(sent) / rate);
            std::this_thread::sleep_until(next);
        }
        tuntom::store_be64(payload.data(), sent + 1);
        ssize_t n;
#ifdef TUNTOM_IPC_GATHER
        const std::uint64_t label = 17;
        n = source.send_frame(tuntom::SwitchOpcode::switch_packet, &label, 1,
                              payload.data(), payload.size());
#else
        const auto frame = tuntom::encode_switch_frame(tuntom::SwitchOpcode::switch_packet,
            {17}, payload.data(), payload.size());
        n = source.send(frame.data(), frame.size());
#endif
        if (n < 0 and (errno == EAGAIN or errno == EWOULDBLOCK)) {
            ++backpressure;
            pollfd ready {source.fd(), POLLOUT, 0};
            ::poll(&ready, 1, 10);
        } else if (n != static_cast<ssize_t>(payload.size() + 16)) {
            failed = true;
            break;
        } else ++sent;
    }
    const double send_cpu = thread_seconds() - cpu;
    const double elapsed = std::chrono::duration<double>(Clock::now() - began).count();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);
    consumer.join();
    std::cout << "{\"sent\":" << sent << ",\"received\":" << received
              << ",\"invalid\":" << invalid << ",\"source_backpressure\":" << backpressure
              << ",\"elapsed_s\":" << elapsed << ",\"source_cpu_s\":" << send_cpu
              << ",\"sink_cpu_s\":" << receive_cpu << "}\n";
    return failed or invalid != 0 or received == 0 ? 1 : 0;
}
