// Two real processes, one ordered SOCK_SEQPACKET connection. No live services.
// Derived from ../ipc_mmap/bench.cpp; batching is test-only, not negotiated production IPC.
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
    std::uint64_t records = 0, successful_calls = 0, partial_calls = 0, max_batch = 0;
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


constexpr std::size_t max_batch = 16;
constexpr std::uint64_t sample_stride = 67; // Coprime to 4/8/16: sample all batch positions.

struct Transport {
    bool mapped = false, mmsg = false, grouped = false;
    std::size_t batch = 1;
};

// Every slot is copied into B's existing private frame buffer before release.
void copy_slot(Mapping &map, std::uint32_t slot, std::uint64_t token,
               std::uint32_t length, std::uint8_t *frame) {
    if (!map.base || slot >= map.slots || length > capacity || length < 17 ||
        !(token & 1) || __atomic_load_n(map.token(slot), __ATOMIC_ACQUIRE) != token)
        throw std::runtime_error("bad slot/token");
    std::memcpy(frame, map.data(slot), length);
    __atomic_store_n(map.token(slot), token + 1, __ATOMIC_RELEASE);
}

Stats consume(int socket, std::size_t profile, Transport transport) {
    Mapping map; setup_receiver(socket, map);
    const auto receive_width = transport.mmsg ? transport.batch : 1;
    std::vector<std::uint8_t> records(receive_width * capacity);
    std::vector<std::uint8_t> frame(capacity);
    std::array<iovec, max_batch> io{};
    std::array<mmsghdr, max_batch> messages{};
    for (std::size_t i = 0; i < receive_width; ++i) {
        io[i] = {records.data() + i*capacity, capacity};
        messages[i].msg_hdr.msg_iov = &io[i]; messages[i].msg_hdr.msg_iovlen = 1;
    }
    Stats s{}; rusage began{}; bool measuring = false, finished = false;
    std::uint64_t expected = 1;
    std::vector<double> latency; latency.reserve(200000);
    auto validate = [&](const std::uint8_t *data, std::size_t length, bool mapped) {
        if (mapped) ++s.mmap_packets; else ++s.inline_packets;
        SwitchFrameView view;
        bool valid = decode_switch_frame(data, length, view) &&
                view.opcode == SwitchOpcode::switch_packet && view.label_count == 1 &&
                view.label(0) == 17 && view.payload_size == payload_size(profile, expected);
        if (valid) {
            valid = load_be64(view.payload) == expected;
            if (measuring && expected % sample_stride == 0)
                latency.push_back(double(now_ns() - load_be64(view.payload + 8)) / 1000);
            valid &= view.payload[view.payload_size-1] ==
                     static_cast<std::uint8_t>((view.payload_size-1)*37+11);
            if (!measuring || expected % 1024 == 0) {
                for (std::size_t i = 16; i < view.payload_size; ++i)
                    valid &= view.payload[i] == static_cast<std::uint8_t>(i*37+11);
                ++s.full_checks;
            }
            s.bytes += view.payload_size;
        }
        if (!valid) throw std::runtime_error("payload/sequence validation");
        ++s.packets; ++expected;
    };
    while (!finished) {
        int count;
        for (;;) {
            for (std::size_t i = 0; i < receive_width; ++i)
                messages[i].msg_hdr.msg_flags = 0;
            ++s.calls;
            if (transport.mmsg) {
                count = recvmmsg(socket, messages.data(), static_cast<unsigned>(receive_width),
                                 MSG_DONTWAIT | MSG_TRUNC, nullptr);
            } else {
                const auto n = recvmsg(socket, &messages[0].msg_hdr, MSG_DONTWAIT | MSG_TRUNC);
                count = n > 0 ? 1 : static_cast<int>(n);
                if (n > 0) messages[0].msg_len = static_cast<unsigned>(n);
            }
            if (count > 0) { ++s.successful_calls; break; }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && errno == EAGAIN) { ready(socket, POLLIN, &s.polls); continue; }
            throw std::runtime_error("packet receive");
        }
        const auto before = s.packets;
        for (int i = 0; i < count; ++i) {
            const auto &message = messages[static_cast<std::size_t>(i)];
            const auto length = message.msg_len;
            const auto *data = records.data() + static_cast<std::size_t>(i)*capacity;
            if (!length || length > capacity || message.msg_hdr.msg_flags & (MSG_TRUNC | MSG_CTRUNC))
                throw std::runtime_error("truncated record");
            if (length == 1 && data[0] == 'B') {
                if (i != count-1) throw std::runtime_error("data before start ack");
                s = {}; expected = 1; measuring = true; began = usage();
                write_record(socket, "K", 1); continue;
            }
            if (length == 1 && data[0] == 'E') {
                if (i != count-1) throw std::runtime_error("data after end");
                s.done_ns = now_ns(); cpu_delta(s, began); finished = true; break;
            }
            ++s.records;
            if (data[0] != 'T') { validate(data, length, false); continue; }
            if (length < 24 || std::memcmp(data, "TTX\1", 4) || data[5] ||
                load_be16(data+6) != length || load_be64(data+8) != epoch || load_be32(data+16) != 1)
                throw std::runtime_error("bad extension");
            if (data[4] == 16) {
                if (length != 40 || load_be32(data+36)) throw std::runtime_error("bad reference");
                const auto size = load_be32(data+32);
                copy_slot(map, load_be32(data+20), load_be64(data+24), size, frame.data());
                validate(frame.data(), size, true);
            } else if (data[4] == 17 && transport.grouped) {
                const auto n = load_be32(data+20);
                if (!n || n > transport.batch || length != 24+16*n)
                    throw std::runtime_error("bad batch count/length");
                for (std::uint32_t j = 0; j < n; ++j) {
                    const auto *ref = data + 24 + 16*j;
                    const auto size = load_be32(ref+12);
                    copy_slot(map, load_be32(ref), load_be64(ref+4), size, frame.data());
                    validate(frame.data(), size, true);
                }
            } else throw std::runtime_error("bad extension type");
        }
        if (s.packets >= before) s.max_batch = std::max(s.max_batch, s.packets-before);
    }
    std::sort(latency.begin(), latency.end());
    if (!latency.empty()) {
        s.p50_us = latency[latency.size()/2];
        s.p99_us = latency[(latency.size()-1)*99/100];
    }
    return s;
}

struct Pending {
    std::vector<std::uint8_t> payload;
    SwitchFrameHeader header{};
    std::array<std::uint8_t, 40> ref{};
    std::array<iovec, 2> io{};
    std::size_t size = 0, h = 0, slot = 0;
    std::uint64_t token = 0;
    bool mapped = false;
    Pending() : payload(65535) {
        for (std::size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<std::uint8_t>(i*37+11);
    }
    void prepare(Mapping &map, std::size_t length, std::uint64_t seq, Stats &s) {
        size = length; mapped = false;
        store_be64(payload.data(), seq);
        store_be64(payload.data()+8, seq % sample_stride == 0 ? now_ns() : 0);
        const std::uint64_t label = 17;
        h = encode_switch_header(header, SwitchOpcode::switch_packet, &label, 1, size);
        io = {{{header.data(), h}, {payload.data(), size}}};
        if (!map.base) return;
        for (std::size_t i = 0; i < map.slots; ++i) {
            const auto candidate = map.cursor; map.cursor = (map.cursor+1) % map.slots;
            const auto free_token = __atomic_load_n(map.token(candidate), __ATOMIC_ACQUIRE);
            if (free_token & 1) continue;
            slot = candidate; token = free_token+1; mapped = true;
            std::memcpy(map.data(slot), header.data(), h);
            std::memcpy(map.data(slot)+h, payload.data(), size);
            __atomic_store_n(map.token(slot), token, __ATOMIC_RELEASE);
            std::memcpy(ref.data(), "TTX\1\20\0\0\50", 8);
            store_be64(ref.data()+8, epoch); store_be32(ref.data()+16, 1);
            store_be32(ref.data()+20, static_cast<std::uint32_t>(slot));
            store_be64(ref.data()+24, token);
            store_be32(ref.data()+32, static_cast<std::uint32_t>(h+size));
            io[0] = {ref.data(), ref.size()}; return;
        }
        ++s.pool_full; // Inline fallback remains ordered among mapped records.
    }
    std::size_t wire_size() const { return mapped ? ref.size() : h+size; }
    void accepted(Stats &s) const {
        ++s.packets; s.bytes += size;
        if (mapped) ++s.mmap_packets; else ++s.inline_packets;
    }
};

void produce_group(int socket, Mapping &map, std::array<Pending, max_batch> &pending,
                   std::size_t profile, std::uint64_t seq, std::size_t count,
                   Transport transport, Stats &s) {
    std::array<mmsghdr, max_batch> messages{};
    for (std::size_t i = 0; i < count; ++i) {
        auto &p = pending[i]; p.prepare(map, payload_size(profile,seq+i), seq+i, s);
        messages[i].msg_hdr.msg_iov = p.io.data();
        messages[i].msg_hdr.msg_iovlen = p.mapped ? 1 : 2;
    }
    std::size_t cursor = 0;
    while (cursor < count) {
        std::array<std::uint8_t, 24+16*max_batch> group{};
        auto *message = &messages[cursor].msg_hdr;
        std::size_t group_count = 1;
        iovec group_io{}; msghdr group_message{};
        const bool grouped = transport.grouped && pending[cursor].mapped;
        if (grouped) {
            // Batch only a contiguous run of references; an inline fallback is a boundary.
            while (cursor+group_count < count && pending[cursor+group_count].mapped) ++group_count;
            std::memcpy(group.data(), "TTX\1\21\0", 6);
            store_be16(group.data()+6, static_cast<std::uint16_t>(24+16*group_count));
            store_be64(group.data()+8, epoch); store_be32(group.data()+16, 1);
            store_be32(group.data()+20, static_cast<std::uint32_t>(group_count));
            for (std::size_t i = 0; i < group_count; ++i) {
                const auto &p = pending[cursor+i]; auto *ref = group.data()+24+16*i;
                store_be32(ref, static_cast<std::uint32_t>(p.slot)); store_be64(ref+4, p.token);
                store_be32(ref+12, static_cast<std::uint32_t>(p.h+p.size));
            }
            group_io = {group.data(), 24+16*group_count};
            group_message.msg_iov = &group_io; group_message.msg_iovlen = 1; message = &group_message;
        }
        ++s.calls;
        int accepted = 0;
        if (transport.mmsg) {
            const auto n = sendmmsg(socket, messages.data()+cursor, static_cast<unsigned>(count-cursor),
                                   MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n > 0) {
                accepted = n;
                if (static_cast<std::size_t>(n) < count-cursor) ++s.partial_calls;
                for (int i = 0; i < n; ++i) {
                    const auto j = cursor+static_cast<std::size_t>(i);
                    if (messages[j].msg_len != pending[j].wire_size()) throw std::runtime_error("short record");
                    s.wire_bytes += messages[j].msg_len;
                }
                s.records += static_cast<std::uint64_t>(n);
            }
        } else {
            const auto n = sendmsg(socket, message, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n >= 0) {
                if (static_cast<std::size_t>(n) != (grouped ? group_io.iov_len : pending[cursor].wire_size()))
                    throw std::runtime_error("short record");
                accepted = static_cast<int>(group_count); ++s.records;
                s.wire_bytes += static_cast<std::uint64_t>(n);
            }
        }
        if (accepted > 0) {
            ++s.successful_calls;
            s.max_batch = std::max(s.max_batch, static_cast<std::uint64_t>(accepted));
            for (int i = 0; i < accepted; ++i) pending[cursor++].accepted(s);
            continue;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN) {
            ++s.eagain; ready(socket, POLLOUT, &s.polls); continue;
        }
        throw std::runtime_error("packet send");
        // On failure the test exits and destroys its mapping. While retrying EAGAIN,
        // retain only unsent records and their slots; never replay an accepted prefix.
    }
}

void produce_phase(int socket, Mapping &map, std::array<Pending, max_batch> &pending,
                   std::size_t profile, Transport transport, Stats &s,
                   std::uint64_t begin, std::uint64_t end, double rate) {
    for (std::uint64_t seq = 1;;) {
        auto now = now_ns();
        if (now >= end) break;
        std::size_t count = transport.batch;
        if (rate) {
            const auto due = begin + static_cast<std::uint64_t>(double(seq-1)*1e9/rate);
            if (due >= end) break;
            if (now < due) {
                timespec target{static_cast<time_t>(due/1000000000), static_cast<long>(due%1000000000)};
                int error;
                do { error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, nullptr); }
                while (error == EINTR);
                if (error) throw std::runtime_error("pacing sleep");
                now = now_ns();
                if (now >= end) break;
            }
            const auto available = 1+static_cast<std::uint64_t>(double(now-begin)*rate/1e9);
            count = std::min(count, static_cast<std::size_t>(available-seq+1));
        }
        // Only frames already due are eligible. No timer waits to fill a batch.
        produce_group(socket, map, pending, profile, seq, count, transport, s);
        seq += count;
    }
}

int main(int argc, char **argv) {
    pid_t child = -1;
    try {
        if (argc != 9 && argc != 10) throw std::runtime_error("usage: bench inline|mmap|inline-mmsg|mmap-mmsg|mmap-batch profile seconds pps cpuA cpuB slots batch [sndbuf]");
        const std::string mode = argv[1], profile = argv[2];
        const std::size_t fixed_size = profile == "mixed" ? 0 : std::stoul(profile);
        const double duration = std::stod(argv[3]), rate = std::stod(argv[4]);
        const int cpuA = std::stoi(argv[5]), cpuB = std::stoi(argv[6]);
        const auto slots = static_cast<std::size_t>(std::stoul(argv[7]));
        const auto batch = static_cast<std::size_t>(std::stoul(argv[8]));
        const int requested_sndbuf = argc == 10 ? std::stoi(argv[9]) : 0;
        Transport transport{mode != "inline" && mode != "inline-mmsg",
                            mode == "inline-mmsg" || mode == "mmap-mmsg", mode == "mmap-batch", batch};
        if ((mode != "inline" && mode != "mmap" && mode != "inline-mmsg" &&
             mode != "mmap-mmsg" && mode != "mmap-batch") || !batch || batch > max_batch ||
            ((!transport.mmsg && !transport.grouped) && batch != 1) || requested_sndbuf < 0 ||
            duration <= 0 || rate < 0 ||
            !slots || slots > 4096 || payload_size(fixed_size,1) < 32 || payload_size(fixed_size,1) > 65535)
            throw std::runtime_error("bad arguments");
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets))
            throw std::runtime_error("socketpair");
        if (requested_sndbuf && setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
                                           &requested_sndbuf, sizeof(requested_sndbuf)))
            throw std::runtime_error("sndbuf");
        child = fork();
        if (child < 0) throw std::runtime_error("fork");
        if (!child) {
            close(sockets[0]); pin(cpuB);
            auto stats = consume(sockets[1], fixed_size, transport);
            write_record(sockets[1], &stats, sizeof(stats)); close(sockets[1]);
            return stats.invalid ? 1 : 0;
        }
        close(sockets[1]); pin(cpuA); Mapping map;
        setup_sender(sockets[0], map, transport.mapped, slots);
        std::array<Pending, max_batch> pending;
        Stats warm;
        const auto warm_begin = now_ns();
        produce_phase(sockets[0], map, pending, fixed_size, transport, warm,
                      warm_begin, warm_begin+200000000, 0);
        write_record(sockets[0], "B", 1);
        char ack; read_record(sockets[0], &ack, 1);
        if (ack != 'K') throw std::runtime_error("start ack");
        Stats a{}, b{}; const auto start_usage = usage(); const auto begin = now_ns();
        const auto end = begin + static_cast<std::uint64_t>(duration*1e9);
        produce_phase(sockets[0], map, pending, fixed_size, transport, a, begin, end, rate);
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
          << ",\"batch\":" << batch << ",\"sndbuf\":" << sndbuf << ",\"rcvbuf\":" << rcvbuf << ",\"elapsed_s\":" << elapsed
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
          << ",\"send_records\":" << a.records << ",\"recv_records\":" << b.records
          << ",\"send_success_calls\":" << a.successful_calls << ",\"recv_success_calls\":" << b.successful_calls
          << ",\"send_partial_calls\":" << a.partial_calls
          << ",\"send_max_batch\":" << a.max_batch << ",\"recv_max_batch\":" << b.max_batch
          << ",\"p50_us\":" << b.p50_us << ",\"p99_us\":" << b.p99_us << "}\n";
        return !WIFEXITED(status) || WEXITSTATUS(status) || a.packets != b.packets || a.records != b.records || a.mmap_packets != b.mmap_packets || b.invalid || !b.packets;
    } catch (const std::exception &e) {
        if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        std::cerr << e.what() << '\n'; return 2;
    }
}
