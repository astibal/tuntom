#include "../src/udp_endpoint.hpp"
#include <algorithm>
#include <fcntl.h>
#include <iostream>
#include <poll.h>

static void require(bool ok, const char* message) {
    if (not ok) throw std::runtime_error(message);
}

static std::vector<int> outcomes;
static std::vector<unsigned> batch_sizes;
static unsigned single_sends = 0;
static int single_error = 0;

extern "C" int __real_sendmmsg(int, mmsghdr*, unsigned, int);
extern "C" ssize_t __real_sendto(int, const void*, size_t, int, const sockaddr*, socklen_t);

// Real loopback delivery, with deterministic short batches and later errors.
extern "C" int __wrap_sendmmsg(int fd, mmsghdr* messages, unsigned count, int flags) {
    require((flags & MSG_DONTWAIT) != 0, "batch must not block");
    const std::size_t call = batch_sizes.size();
    batch_sizes.push_back(count);
    if (call < outcomes.size()) {
        const int outcome = outcomes[call];
        if (outcome <= 0) { errno = -outcome; return outcome == 0 ? 0 : -1; }
        count = std::min(count, static_cast<unsigned>(outcome));
    }
    return __real_sendmmsg(fd, messages, count, flags);
}

extern "C" ssize_t __wrap_sendto(int fd, const void* data, size_t size, int flags,
                                 const sockaddr* address, socklen_t length) {
    ++single_sends;
    require((::fcntl(fd, F_GETFL) & O_NONBLOCK) != 0, "single send must not block");
    if (single_error != 0) { errno = single_error; return -1; }
    return __real_sendto(fd, data, size, flags, address, length);
}

struct Receiver {
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_in address {};
    Receiver() {
        require(fd >= 0, "receiver socket");
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "receiver bind");
        socklen_t length = sizeof(address);
        require(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0,
                "receiver address");
    }
    ~Receiver() { ::close(fd); }

    void check(const std::vector<std::vector<std::uint8_t>>& buffers, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            pollfd ready {fd, POLLIN, 0};
            require(::poll(&ready, 1, 1000) == 1, "datagram delivery deadline");
            std::vector<std::uint8_t> received(2048);
            const auto size = ::recv(fd, received.data(), received.size(), 0);
            require(size == static_cast<ssize_t>(buffers[i].size()), "datagram boundary");
            require(std::equal(buffers[i].begin(), buffers[i].end(), received.begin()),
                    "datagram order and contents");
        }
        std::uint8_t extra;
        require(::recv(fd, &extra, 1, 0) == -1 and (errno == EAGAIN or errno == EWOULDBLOCK),
                "no duplicated or unexpected datagrams");
    }
};

static void exercise(std::size_t count, std::vector<int> script,
                     std::size_t expected_sent, int expected_error,
                     std::vector<unsigned> expected_calls) {
    Receiver receiver;
    tuntom::UdpEndpoint endpoint;
    endpoint.open_client("127.0.0.1", ntohs(receiver.address.sin_port));
    std::vector<std::vector<std::uint8_t>> buffers(count);
    for (std::size_t i = 0; i < count; ++i)
        buffers[i].assign(100 + i, static_cast<std::uint8_t>(i));
    outcomes = std::move(script);
    batch_sizes.clear();
    single_sends = 0;
    const auto result = endpoint.send_batch(buffers.data(), buffers.size());
    std::size_t expected_bytes = 0;
    for (std::size_t i = 0; i < expected_sent; ++i) expected_bytes += buffers[i].size();
    require(result.packets == expected_sent and result.bytes == expected_bytes and
            result.error == expected_error, "batch result preserves successful prefix and error");
    require(batch_sizes == expected_calls, "only retry unsent suffix; stop on error");
    require(single_sends == (count == 1 ? 1U : 0U), "single datagram fast path");
    receiver.check(buffers, expected_sent);
}

int main() {
    try {
        ::alarm(15);
        exercise(7, {}, 7, 0, {7});
        exercise(7, {3, 2}, 7, 0, {7, 4, 2});
        exercise(7, {3, -EMSGSIZE}, 3, EMSGSIZE, {7, 4});
        exercise(7, {2, -EAGAIN}, 2, EAGAIN, {7, 5});
        exercise(7, {-EAGAIN}, 0, EAGAIN, {7});
        exercise(7, {-EINTR}, 0, EINTR, {7});
        exercise(7, {0}, 0, EIO, {7});
        exercise(tuntom::max_fragments_per_packet, {},
                 tuntom::max_fragments_per_packet, 0, {64});
        exercise(1, {}, 1, 0, {});
        single_error = EMSGSIZE;
        exercise(1, {}, 0, EMSGSIZE, {});
        single_error = 0;

        tuntom::UdpEndpoint unopened;
        require(unopened.send_batch(nullptr, 0).error == 0, "empty batch");
        require(unopened.send_batch(nullptr, 65).error == EINVAL, "bounded batch");
        std::vector<std::uint8_t> packet {1};
        require(unopened.send_batch(&packet, 1).error == EDESTADDRREQ, "missing peer");
        std::cout << "PASS: UDP batch boundaries, short sends, errors and single send\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
