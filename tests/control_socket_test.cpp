#include "../src/control_socket.hpp"
#include "../src/runtime_recovery.hpp"
#include <cstdlib>
#include <iostream>
#include <vector>

static bool handling = false;
static int send_error = 0;
extern "C" int __real_poll(pollfd*, nfds_t, int);
extern "C" int __wrap_poll(pollfd* fds, nfds_t count, int timeout) {
    if (handling and timeout != 0) std::abort();
    return __real_poll(fds, count, timeout);
}
extern "C" ssize_t __real_send(int, const void*, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void* data, size_t size, int flags) {
    if (handling and send_error) { errno = send_error; return -1; }
    return __real_send(fd, data, size, flags);
}
static void require(bool value) { if (not value) std::abort(); }

int main() {
    char directory[] = "/tmp/tuntom-control-state.XXXXXX";
    require(::mkdtemp(directory));
    const std::string path = std::string(directory) + "/control";
    {
        tuntom::ControlSocket control(path);
        unsigned snapshots = 0;
        const auto handle = [&] {
            handling = true;
            control.handle([&] { ++snapshots; return std::string("packets=42\n"); });
            handling = false;
        };
        const auto connect = [&] {
            const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            require(fd >= 0);
            sockaddr_un address {};
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
            require(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
            return fd;
        };
        const auto request = [](int fd) { require(::send(fd, "show stats", 10, MSG_NOSIGNAL) == 10); };
        const auto response = [](int fd) {
            char data[128] {};
            const auto size = ::recv(fd, data, sizeof(data), 0);
            require(size == 11 and std::string(data, 11) == "packets=42\n");
        };
        // Accept before any request arrives. The handler must never wait.
        const int idle = connect();
        handle();
        require(snapshots == 0);
        pollfd clients[tuntom::ControlSocket::max_clients];
        control.poll_clients(clients);
        require(clients[0].fd >= 0 and clients[0].events == POLLIN);
        require(control.poll_timeout_ms(std::chrono::steady_clock::now(), 1000) <= 10);
        request(idle);
        // During resource recovery the pending client's request also wakes pselect.
        tuntom::RuntimeRecovery recovery;
        recovery.allocation_failed();
        require(recovery.wait_for_retry(-1, clients, tuntom::ControlSocket::max_clients, 10));
        handle();
        response(idle);
        ::close(idle);

        // Scheduler delay must not discard a request already queued in the socket.
        const int queued = connect();
        handle();
        request(queued);
        ::usleep(20000);
        handle();
        response(queued);
        ::close(queued);

        // Backpressured output keeps exactly one snapshot and waits for POLLOUT.
        const int delayed = connect();
        request(delayed);
        send_error = EAGAIN;
        handle();
        const auto generated = snapshots;
        control.poll_clients(clients);
        require(clients[0].events == POLLOUT);
        send_error = EINTR;
        handle();
        require(snapshots == generated);
        send_error = 0;
        handle();
        response(delayed);
        require(snapshots == generated);
        ::close(delayed);

        const int stalled = connect();
        request(stalled);
        send_error = EAGAIN;
        handle();
        ::usleep(20000);
        handle();
        send_error = 0;
        char closed;
        require(::recv(stalled, &closed, 1, 0) == 0);
        ::close(stalled);
        const int gone = connect();
        handle();
        ::close(gone);
        handle();
        control.poll_clients(clients);
        for (const auto& client : clients) require(client.fd == -1);

        // Saturation removes only the listener; expiration frees every slot.
        std::vector<int> idle_peers;
        for (std::size_t i = 0; i < tuntom::ControlSocket::max_clients; ++i) {
            idle_peers.push_back(connect());
            handle();
        }
        require(control.poll_fd() == -1);
        ::usleep(20000);
        require(control.poll_timeout_ms(std::chrono::steady_clock::now(), 1000) == 0);
        handle();
        require(control.poll_fd() >= 0);
        for (int fd : idle_peers) {
            char byte;
            require(::recv(fd, &byte, 1, 0) == 0);
            ::close(fd);
        }
        // An oversized command must not be accepted by truncating its suffix.
        const int oversized = connect();
        const std::string command = "show stats" + std::string(54, '\n') + "evil";
        require(::send(oversized, command.data(), command.size(), MSG_NOSIGNAL) ==
                static_cast<ssize_t>(command.size()));
        handle();
        char buffer[64] {};
        const auto size = ::recv(oversized, buffer, sizeof(buffer), 0);
        require(size > 0 and std::string(buffer, static_cast<std::size_t>(size)) == "error=unknown_command\n");
        ::close(oversized);
    }
    ::rmdir(directory);
    std::cout << "PASS: nonblocking control, deferred request/output, recovery wakeup, limits and expiry\n";
}
