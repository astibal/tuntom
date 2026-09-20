// Two real processes, one ordered SOCK_SEQPACKET connection. No live services.
// This exercises the proposed payload path, not the complete new handshake.
#include "../../src/ipc/switch_protocol.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <sched.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

using namespace tuntom;
constexpr std::size_t capacity = 65607;
constexpr std::size_t stride = ((64 + capacity + 63) / 64) * 64;
constexpr std::uint64_t epoch = 1; // One new mapping per disposable connection.
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
static_assert(__atomic_always_lock_free(8, nullptr));

std::uint64_t now_ns() {
    timespec t{};
    if (clock_gettime(CLOCK_MONOTONIC, &t)) throw std::runtime_error("clock");
    return std::uint64_t(t.tv_sec) * 1000000000 + std::uint64_t(t.tv_nsec);
}
void pin(int cpu) {
    if (cpu < 0 || cpu >= CPU_SETSIZE) throw std::runtime_error("CPU range");
    cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(cpu, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask)) throw std::runtime_error("affinity");
}
void ready(int fd, short events, std::uint64_t *polls = nullptr) {
    pollfd p{fd, events, 0};
    if (polls) ++*polls;
    int n;
    do { n = poll(&p, 1, 5000); } while (n < 0 && errno == EINTR);
    if (n <= 0 || (p.revents & (POLLERR | POLLNVAL)))
        throw std::runtime_error("socket wait failed/timed out");
}
void write_record(int fd, const void *p, std::size_t size) {
    for (;;) {
        auto n = send(fd, p, size, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n == static_cast<ssize_t>(size)) return;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) { ready(fd, POLLOUT); continue; }
        throw std::runtime_error("write record");
    }
}
ssize_t read_record(int fd, void *p, std::size_t size, std::uint64_t *calls = nullptr,
                    std::uint64_t *polls = nullptr) {
    for (;;) {
        iovec io{p, size}; msghdr m{}; m.msg_iov = &io; m.msg_iovlen = 1;
        if (calls) ++*calls;
        auto n = recvmsg(fd, &m, MSG_DONTWAIT | MSG_TRUNC);
        if (n > 0 && !(m.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) return n;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) { ready(fd, POLLIN, polls); continue; }
        throw std::runtime_error("read record");
    }
}
struct Mapping {
    std::uint8_t *base = nullptr;
    std::size_t size = 0, slots = 0, cursor = 0;
    ~Mapping() { if (base) munmap(base, size); }
    void map(int fd, std::size_t count) {
        slots = count; size = 4096 + slots * stride;
        struct stat st{};
        if (fstat(fd, &st) || st.st_size != static_cast<off_t>(size))
            throw std::runtime_error("mapping size");
        auto p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) throw std::runtime_error("mmap");
        base = static_cast<std::uint8_t *>(p);
    }
    std::uint64_t *token(std::size_t i) {
        return reinterpret_cast<std::uint64_t *>(base + 4096 + i * stride);
    }
    std::uint8_t *data(std::size_t i) { return base + 4096 + i * stride + 64; }
};

// FD transfer happens before timing; B maps its own virtual address range.
void setup_sender(int socket, Mapping &map, bool mapped, std::size_t slots) {
    int fd = -1;
    if (mapped) {
        fd = memfd_create("tuntom-ipc-mmap-bench", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd < 0 || ftruncate(fd, static_cast<off_t>(4096 + slots * stride)))
            throw std::runtime_error("memfd allocation");
        map.map(fd, slots);
        std::memset(map.base, 0, map.size); // Prefault before warmup/timing.
        if (fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL))
            throw std::runtime_error("seals");
    }
    std::uint32_t count = mapped ? static_cast<std::uint32_t>(slots) : 0;
    iovec io{&count, sizeof(count)}; msghdr m{}; m.msg_iov = &io; m.msg_iovlen = 1;
    alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int))]{};
    if (mapped) {
        m.msg_control = ancillary; m.msg_controllen = sizeof(ancillary);
        auto c = CMSG_FIRSTHDR(&m); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int)); std::memcpy(CMSG_DATA(c), &fd, sizeof(fd));
    }
    if (sendmsg(socket, &m, MSG_NOSIGNAL) != sizeof(count))
        throw std::runtime_error("setup send");
    if (fd >= 0) close(fd);
    char ack; if (read_record(socket, &ack, 1) != 1 || ack != 'K')
        throw std::runtime_error("setup ack");
}
void setup_receiver(int socket, Mapping &map) {
    ready(socket, POLLIN);
    std::uint32_t count = 0;
    iovec io{&count, sizeof(count)}; msghdr m{}; m.msg_iov = &io; m.msg_iovlen = 1;
    alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int))]{};
    m.msg_control = ancillary; m.msg_controllen = sizeof(ancillary);
    if (recvmsg(socket, &m, MSG_CMSG_CLOEXEC) != sizeof(count) ||
        (m.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) throw std::runtime_error("setup recv");
    if (count) {
        auto c = CMSG_FIRSTHDR(&m);
        if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
            c->cmsg_len != CMSG_LEN(sizeof(int))) throw std::runtime_error("setup fd");
        int fd; std::memcpy(&fd, CMSG_DATA(c), sizeof(fd));
        map.map(fd, count); close(fd);
    }
    write_record(socket, "K", 1);
}
double seconds(timeval t) { return double(t.tv_sec) + double(t.tv_usec) / 1e6; }
struct Stats {
    std::uint64_t packets = 0, bytes = 0, mmap_packets = 0, inline_packets = 0;
    std::uint64_t calls = 0, polls = 0, eagain = 0, pool_full = 0, invalid = 0;
    std::uint64_t full_checks = 0, wire_bytes = 0, done_ns = 0;
    double user_s = 0, sys_s = 0, voluntary = 0, involuntary = 0;
    double p50_us = 0, p99_us = 0;
};
rusage usage() { rusage r{}; getrusage(RUSAGE_SELF, &r); return r; }
void cpu_delta(Stats &s, const rusage &start) {
    auto end = usage(); s.user_s = seconds(end.ru_utime) - seconds(start.ru_utime);
    s.sys_s = seconds(end.ru_stime) - seconds(start.ru_stime);
    s.voluntary = double(end.ru_nvcsw - start.ru_nvcsw);
    s.involuntary = double(end.ru_nivcsw - start.ru_nivcsw);
}
std::size_t payload_size(std::size_t profile, std::uint64_t seq) {
    return profile ? profile : (seq % 3 == 0 ? 64 : 9000);
}

Stats consume(int socket, std::size_t profile) {
    Mapping map; setup_receiver(socket, map);
    std::vector<std::uint8_t> frame(capacity);
    Stats s{}; rusage began{}; bool measuring = false;
    std::uint64_t expected = 1;
    std::vector<double> latency; latency.reserve(200000);
    for (;;) {
        auto n = read_record(socket, frame.data(), frame.size(), &s.calls, &s.polls);
        if (n == 1 && frame[0] == 'B') {
            s = {}; expected = 1; measuring = true; began = usage();
            write_record(socket, "K", 1); continue;
        }
        if (n == 1 && frame[0] == 'E') {
            s.done_ns = now_ns(); cpu_delta(s, began); break;
        }
        bool valid = true;
        if (frame[0] == 'T') {
            if (n != 40 || std::memcmp(frame.data(), "TTX\1\20\0\0\50", 8) ||
                load_be64(frame.data()+8) != epoch || load_be32(frame.data()+16) != 1 ||
                load_be32(frame.data()+36)) throw std::runtime_error("bad reference");
            const auto slot = load_be32(frame.data()+20);
            const auto token = load_be64(frame.data()+24);
            const auto length = load_be32(frame.data()+32);
            if (!map.base || slot >= map.slots || length > capacity || length < 17 ||
                !(token & 1) || __atomic_load_n(map.token(slot), __ATOMIC_ACQUIRE) != token)
                throw std::runtime_error("bad slot/token");
            // Keep the existing receiver-buffer API: no zero-copy shortcut.
            std::memcpy(frame.data(), map.data(slot), length);
            __atomic_store_n(map.token(slot), token + 1, __ATOMIC_RELEASE);
            n = length; ++s.mmap_packets;
        } else ++s.inline_packets;
        SwitchFrameView view;
        valid = decode_switch_frame(frame.data(), static_cast<std::size_t>(n), view) &&
                view.opcode == SwitchOpcode::switch_packet && view.label_count == 1 &&
                view.label(0) == 17 && view.payload_size == payload_size(profile, expected);
        if (valid) {
            valid = load_be64(view.payload) == expected;
            if (measuring && expected % 64 == 0) {
                auto stamp = load_be64(view.payload + 8);
                latency.push_back(double(now_ns() - stamp) / 1000);
            }
            valid &= view.payload[view.payload_size-1] ==
                     static_cast<std::uint8_t>((view.payload_size-1)*37+11);
            if (!measuring || expected % 1024 == 0) {
                for (std::size_t i = 16; i < view.payload_size; ++i)
                    valid &= view.payload[i] == static_cast<std::uint8_t>(i*37+11);
                ++s.full_checks;
            }
            s.bytes += view.payload_size;
        }
        if (!valid) {
            if (!measuring) throw std::runtime_error("warmup payload validation");
            ++s.invalid;
        }
        ++s.packets; ++expected;
    }
    std::sort(latency.begin(), latency.end());
    if (!latency.empty()) {
        s.p50_us = latency[latency.size()/2];
        s.p99_us = latency[(latency.size()-1)*99/100];
    }
    return s;
}

void produce_frame(int socket, Mapping &map, std::vector<std::uint8_t> &payload,
                   std::size_t size, std::uint64_t seq, Stats &s) {
    store_be64(payload.data(), seq);
    store_be64(payload.data()+8, seq % 64 == 0 ? now_ns() : 0);
    SwitchFrameHeader header; const std::uint64_t label = 17;
    const auto h = encode_switch_header(header, SwitchOpcode::switch_packet, &label, 1, size);
    for (;;) {
        std::size_t slot = map.slots; std::uint64_t token = 0;
        if (map.base) {
            for (std::size_t i = 0; i < map.slots; ++i) {
                auto candidate = map.cursor; map.cursor = (map.cursor + 1) % map.slots;
                token = __atomic_load_n(map.token(candidate), __ATOMIC_ACQUIRE);
                if (!(token & 1)) { slot = candidate; break; }
            }
        }
        std::array<std::uint8_t, 40> ref{};
        iovec io[2]{{header.data(), h}, {payload.data(), size}};
        msghdr m{}; m.msg_iov = io; m.msg_iovlen = 2;
        const bool mapped = map.base && slot < map.slots;
        if (mapped) {
            // Same two user-space sources as sendmsg; copy them into the shared slot.
            std::memcpy(map.data(slot), header.data(), h);
            std::memcpy(map.data(slot)+h, payload.data(), size);
            __atomic_store_n(map.token(slot), token+1, __ATOMIC_RELEASE);
            std::memcpy(ref.data(), "TTX\1\20\0\0\50", 8);
            store_be64(ref.data()+8, epoch); store_be32(ref.data()+16, 1);
            store_be32(ref.data()+20, static_cast<std::uint32_t>(slot));
            store_be64(ref.data()+24, token+1);
            store_be32(ref.data()+32, static_cast<std::uint32_t>(h+size));
            io[0] = {ref.data(), ref.size()}; m.msg_iovlen = 1;
        } else if (map.base) ++s.pool_full;
        ++s.calls;
        const auto n = sendmsg(socket, &m, MSG_DONTWAIT | MSG_NOSIGNAL);
        const auto error = errno;
        if (n == static_cast<ssize_t>(mapped ? ref.size() : h+size)) {
            s.wire_bytes += static_cast<std::uint64_t>(n); ++s.packets; s.bytes += size;
            if (mapped) ++s.mmap_packets; else ++s.inline_packets;
            return;
        }
        // An unsent descriptor cannot be observed by B. Return its slot on failure.
        if (mapped) __atomic_store_n(map.token(slot), token+2, __ATOMIC_RELEASE);
        if (n < 0 && error == EINTR) continue;
        if (n < 0 && error == EAGAIN) {
            ++s.eagain; ready(socket, POLLOUT, &s.polls); continue;
        }
        throw std::runtime_error("packet send");
    }
}

int main(int argc, char **argv) {
    pid_t child = -1;
    try {
        if (argc != 8) throw std::runtime_error("usage: bench inline|mmap 64|1500|9000|mixed seconds pps cpuA cpuB slots");
        const std::string mode = argv[1], profile = argv[2];
        const std::size_t fixed_size = profile == "mixed" ? 0 : std::stoul(profile);
        const double duration = std::stod(argv[3]), rate = std::stod(argv[4]);
        const int cpuA = std::stoi(argv[5]), cpuB = std::stoi(argv[6]);
        const auto slots = static_cast<std::size_t>(std::stoul(argv[7]));
        if ((mode != "inline" && mode != "mmap") || duration <= 0 || rate < 0 ||
            !slots || slots > 4096 || payload_size(fixed_size,1) < 32 || payload_size(fixed_size,1) > 65535)
            throw std::runtime_error("bad arguments");
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets))
            throw std::runtime_error("socketpair");
        child = fork();
        if (child < 0) throw std::runtime_error("fork");
        if (!child) {
            close(sockets[0]); pin(cpuB);
            auto stats = consume(sockets[1], fixed_size);
            write_record(sockets[1], &stats, sizeof(stats)); close(sockets[1]);
            return stats.invalid ? 1 : 0;
        }
        close(sockets[1]); pin(cpuA); Mapping map;
        setup_sender(sockets[0], map, mode == "mmap", slots);
        std::vector<std::uint8_t> payload(65535);
        for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i*37+11);
        Stats warm;
        const auto until = now_ns() + 200000000;
        for (std::uint64_t seq=1; now_ns()<until; ++seq)
            produce_frame(sockets[0], map, payload, payload_size(fixed_size,seq), seq, warm);
        write_record(sockets[0], "B", 1);
        char ack; read_record(sockets[0], &ack, 1);
        if (ack != 'K') throw std::runtime_error("start ack");
        Stats a{}, b{}; const auto start_usage = usage(); const auto begin = now_ns();
        const auto end = begin + static_cast<std::uint64_t>(duration*1e9);
        for (std::uint64_t seq=1; now_ns()<end; ++seq) {
            if (rate) {
                const auto due = begin + static_cast<std::uint64_t>(double(seq-1)*1e9/rate);
                if (due >= end) break;
                if (now_ns() < due) {
                    timespec target{static_cast<time_t>(due/1000000000), static_cast<long>(due%1000000000)};
                    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, nullptr) == EINTR) {}
                }
            }
            produce_frame(sockets[0], map, payload, payload_size(fixed_size,seq), seq, a);
        }
        const auto send_end = now_ns(); write_record(sockets[0], "E", 1); cpu_delta(a, start_usage);
        if (read_record(sockets[0], &b, sizeof(b)) != sizeof(b)) throw std::runtime_error("stats");
        int status; waitpid(child, &status, 0); child = -1;
        int sndbuf = 0, rcvbuf = 0; socklen_t optlen = sizeof(int);
        getsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, &optlen);
        getsockopt(sockets[0], SOL_SOCKET, SO_RCVBUF, &rcvbuf, &optlen);
        close(sockets[0]);
        const auto elapsed = double(b.done_ns-begin)/1e9;
        const auto cpu = a.user_s+a.sys_s+b.user_s+b.sys_s;
        std::cout << std::setprecision(10) << "{\"mode\":\"" << mode << "\",\"profile\":\"" << profile
          << "\",\"rate\":" << rate << ",\"slots\":" << slots << ",\"cpu_a\":" << cpuA << ",\"cpu_b\":" << cpuB
          << ",\"sndbuf\":" << sndbuf << ",\"rcvbuf\":" << rcvbuf << ",\"elapsed_s\":" << elapsed
          << ",\"send_elapsed_s\":" << double(send_end-begin)/1e9
          << ",\"sent\":" << a.packets << ",\"received\":" << b.packets << ",\"invalid\":" << b.invalid
          << ",\"full_checks\":" << b.full_checks << ",\"pps\":" << double(b.packets)/elapsed
          << ",\"payload_mbps\":" << double(b.bytes)*8/elapsed/1e6 << ",\"cpu_us_per_frame\":" << cpu*1e6/double(b.packets)
          << ",\"a_user_s\":" << a.user_s << ",\"a_sys_s\":" << a.sys_s
          << ",\"b_user_s\":" << b.user_s << ",\"b_sys_s\":" << b.sys_s
          << ",\"a_vol_cs\":" << a.voluntary << ",\"b_vol_cs\":" << b.voluntary
          << ",\"a_invol_cs\":" << a.involuntary << ",\"b_invol_cs\":" << b.involuntary
          << ",\"send_calls\":" << a.calls << ",\"recv_calls\":" << b.calls
          << ",\"a_polls\":" << a.polls << ",\"b_polls\":" << b.polls << ",\"send_eagain\":" << a.eagain
          << ",\"pool_full_attempts\":" << a.pool_full << ",\"mmap_packets\":" << b.mmap_packets
          << ",\"inline_packets\":" << b.inline_packets << ",\"socket_bytes\":" << a.wire_bytes
          << ",\"p50_us\":" << b.p50_us << ",\"p99_us\":" << b.p99_us << "}\n";
        return !WIFEXITED(status) || WEXITSTATUS(status) || a.packets != b.packets || b.invalid || !b.packets;
    } catch (const std::exception &e) {
        if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        std::cerr << e.what() << '\n'; return 2;
    }
}
