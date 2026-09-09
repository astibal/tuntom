#include "../src/udp_endpoint.hpp"
#include "../src/tun_device.hpp"
#include <cstdlib>
#include <dirent.h>
#include <iostream>
#include <sstream>

using tuntom::UdpEndpoint;
static int socket_error = 0, bind_error = 0, option_error = 0, pending_error = 0;
static int read_error = 0, write_error = 0, tun_fd = -1;
static unsigned resolves = 0;
static void require(bool ok, const char* message) {
    if (not ok) throw std::runtime_error(message);
}
extern "C" int __real_socket(int, int, int);
extern "C" int __wrap_socket(int family, int type, int protocol) {
    if (socket_error) { errno = socket_error; return -1; }
    return __real_socket(family, type, protocol);
}
extern "C" int __real_bind(int, const sockaddr*, socklen_t);
extern "C" int __wrap_bind(int fd, const sockaddr* address, socklen_t length) {
    if (bind_error) { errno = bind_error; return -1; }
    return __real_bind(fd, address, length);
}
extern "C" int __real_setsockopt(int, int, int, const void*, socklen_t);
extern "C" int __wrap_setsockopt(int fd, int level, int option, const void* value, socklen_t length) {
    if (option_error) { errno = option_error; return -1; }
    return __real_setsockopt(fd, level, option, value, length);
}
extern "C" int __real_getsockopt(int, int, int, void*, socklen_t*);
extern "C" int __wrap_getsockopt(int fd, int level, int option, void* value, socklen_t* length) {
    if (option == SO_ERROR and pending_error) {
        *static_cast<int*>(value) = pending_error;
        pending_error = 0;
        return 0;
    }
    return __real_getsockopt(fd, level, option, value, length);
}
extern "C" int __real_getaddrinfo(const char*, const char*, const addrinfo*, addrinfo**);
extern "C" int __wrap_getaddrinfo(const char* host, const char* service, const addrinfo* hints, addrinfo** result) {
    ++resolves;
    return __real_getaddrinfo(host, service, hints, result);
}
extern "C" ssize_t __real_read(int, void*, size_t);
extern "C" ssize_t __wrap_read(int fd, void* data, size_t size) {
    if (fd == tun_fd and read_error) { errno = read_error; return -1; }
    return __real_read(fd, data, size);
}
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size) {
    if (fd == tun_fd and write_error) { errno = write_error; return -1; }
    return __real_write(fd, data, size);
}
static unsigned fd_count() {
    DIR* directory = ::opendir("/proc/self/fd");
    require(directory != nullptr, "count descriptors");
    unsigned count = 0;
    while (const auto* entry = ::readdir(directory))
        if (entry->d_name[0] != '.') ++count;
    ::closedir(directory);
    return count;
}
static unsigned port(int fd) {
    sockaddr_storage address {};
    socklen_t size = sizeof(address);
    require(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0, "getsockname");
    return address.ss_family == AF_INET
        ? ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port)
        : ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
}
static void exchange(UdpEndpoint& sender, UdpEndpoint& receiver) {
    const std::uint8_t message[] {1, 2, 3};
    require(sender.send(message, sizeof(message)) == 3, "UDP send");
    std::uint8_t result[8] {};
    sockaddr_storage source {};
    socklen_t length = 0;
    require(receiver.receive(result, sizeof(result), source, length) == 3, "UDP receive");
    require(std::memcmp(message, result, 3) == 0, "UDP contents");
    receiver.set_peer(source, length);
}
static void udp_case(const char* host) {
    UdpEndpoint server, client;
    server.open_server(0);
    const auto server_port = port(server.fd());
    client.open_client(host, static_cast<std::uint16_t>(server_port));
    const auto client_port = port(client.fd());
    const auto dns_calls = resolves;
    exchange(client, server);
    exchange(server, client);
    const auto now = UdpEndpoint::Clock::now();
    for (UdpEndpoint* endpoint : {&client, &server}) {
        const auto original_port = port(endpoint->fd());
        const int original_fd = endpoint->fd();
        pending_error = ECONNREFUSED;
        endpoint->poll_events(POLLERR);
        require(endpoint->fd() == original_fd and endpoint->poll_fd() < 0, "soft error preserves socket and pauses poll");
        require(endpoint->poll_timeout_ms(UdpEndpoint::Clock::now(), 1000) <= 100, "soft error has deadline");
        endpoint->poll_events(POLLHUP);
        require(endpoint->fd() == -1, "hard error closes UDP");
        socket_error = EMFILE;
        endpoint->maintain(now + std::chrono::seconds(2));
        std::ostringstream before, after;
        endpoint->write_stats(before);
        endpoint->maintain(now + std::chrono::seconds(2));
        endpoint->write_stats(after);
        require(before.str() == after.str(), "reopen failure backs off");
        socket_error = 0;
        bind_error = EADDRINUSE;
        endpoint->maintain(now + std::chrono::seconds(4));
        require(endpoint->fd() == -1, "bind failure leaves endpoint closed");
        bind_error = 0;
        option_error = ENOMEM;
        endpoint->maintain(now + std::chrono::seconds(6));
        require(endpoint->fd() == -1, "option failure leaves endpoint closed");
        option_error = 0;
        const auto count = fd_count();
        bind_error = EADDRINUSE;
        for (int attempt = 8; attempt < 40; ++attempt)
            endpoint->maintain(now + std::chrono::seconds(attempt));
        require(fd_count() == count, "failed reopen must not leak descriptors");
        bind_error = 0;
        endpoint->maintain(now + std::chrono::seconds(40));
        require(endpoint->fd() >= 0 and port(endpoint->fd()) == original_port, "reopen preserves bind");
        require((::fcntl(endpoint->fd(), F_GETFL) & O_NONBLOCK) != 0, "reopened socket nonblocking");
        require((::fcntl(endpoint->fd(), F_GETFD) & FD_CLOEXEC) != 0, "reopened socket cloexec");
        exchange(client, server);
        exchange(server, client);
    }
    require(resolves == dns_calls, "recovery must not resolve DNS");
    require(port(client.fd()) == client_port and port(server.fd()) == server_port, "stable ports");
}
static void tun_case() {
    int pair[2];
    require(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0, "fake TUN pair");
    const auto inherited = std::to_string(pair[0]);
    require(::setenv("TUNTOM_TEST_TUN_FD", inherited.c_str(), 1) == 0, "fake TUN environment");
    {
        tuntom::TunDevice tun("test", 1500);
        tun_fd = tun.fd();
        std::uint8_t buffer[8] {};
        read_error = EINTR;
        require(tun.read_packet(buffer, sizeof(buffer)) < 0 and tun.fd() >= 0, "EINTR retains TUN");
        read_error = ENOMEM;
        require(tun.read_packet(buffer, sizeof(buffer)) < 0 and tun.poll_fd() < 0, "resource error pauses TUN poll");
        read_error = 0;
        write_error = EINVAL;
        require(tun.write_packet(buffer, sizeof(buffer)) < 0 and tun.fd() >= 0, "bad packet retains TUN");
        write_error = EIO;
        for (int packet = 0; packet < 32; ++packet)
            require(tun.write_packet(buffer, sizeof(buffer)) < 0 and errno == EIO and
                tun.fd() == tun_fd, "DOWN drops packets without closing TUN");
        write_error = 0;
        require(::recv(pair[1], buffer, sizeof(buffer), MSG_DONTWAIT) < 0 and
            errno == EAGAIN, "DOWN packets are dropped");
        buffer[0] = 42;
        require(tun.write_packet(buffer, sizeof(buffer)) == sizeof(buffer), "UP resumes TUN writes");
        std::uint8_t received[8] {};
        require(::recv(pair[1], received, sizeof(received), MSG_DONTWAIT) == sizeof(received) and
            std::memcmp(buffer, received, sizeof(buffer)) == 0, "UP delivers fresh packet");
        require(::recv(pair[1], received, sizeof(received), MSG_DONTWAIT) < 0 and
            errno == EAGAIN, "UP does not replay dropped packets");
        std::ostringstream stats;
        tun.write_stats(stats);
        require(stats.str().find("tun_endpoint_failures=0\n") != std::string::npos,
            "DOWN is not a permanent endpoint failure");
    }
    for (int error : {EBADF, ENODEV, ENXIO}) {
        tuntom::TunDevice tun("test", 1500);
        tun_fd = tun.fd();
        write_error = error;
        std::uint8_t byte = 0;
        require(tun.write_packet(&byte, 1) < 0 and errno == error and tun.fd() == -1,
            "permanent write fault retires TUN");
        write_error = 0;
    }
    {
        tuntom::TunDevice tun("test", 1500);
        tun_fd = tun.fd();
        read_error = ENODEV;
        std::uint8_t byte;
        require(tun.read_packet(&byte, 1) < 0 and tun.fd() == -1, "read fault retires TUN");
        read_error = 0;
    }
    for (int event : {POLLERR, POLLHUP, POLLNVAL}) {
        tuntom::TunDevice tun("test", 1500);
        tun.poll_events(static_cast<short>(event));
        require(tun.fd() == -1, "poll fault retires TUN");
    }
    ::close(pair[0]);
    ::close(pair[1]);
}
int main() {
    udp_case("127.0.0.1");
    udp_case("::1");
    tun_case();
    std::cout << "PASS: UDP recovery/ports/DNS/backoff, TUN DOWN/UP and retirement\n";
}
