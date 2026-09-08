#include "../src/switch_client.hpp"
#include <cstdlib>
#include <fcntl.h>
#include <iostream>

using Client = tuntom::SwitchClient;
using namespace std::chrono_literals;

static void require(bool ok, const char* message) {
    if (not ok) throw std::runtime_error(message);
}

// Linker wrappers keep real Unix sockets, with deterministic coverage of
// uncommon connect/registration errors without changing production code.
static int connect_error = 0;
static int send_error = 0;
static int socket_error = 0;
static int getsockopt_error = 0;
static bool short_send = false;
static unsigned sends = 0;

extern "C" int __real_connect(int, const sockaddr*, socklen_t);
extern "C" ssize_t __real_send(int, const void*, size_t, int);
extern "C" int __real_getsockopt(int, int, int, void*, socklen_t*);

extern "C" int __wrap_connect(int fd, const sockaddr* address, socklen_t size) {
    require((::fcntl(fd, F_GETFL) & O_NONBLOCK) != 0, "connect must be nonblocking");
    require((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "socket must be close-on-exec");
    if (connect_error != 0 and connect_error != EINPROGRESS) {
        errno = connect_error;
        return -1;
    }
    const int result = __real_connect(fd, address, size);
    if (result == 0 and connect_error == EINPROGRESS) {
        errno = EINPROGRESS;
        return -1;
    }
    return result;
}

extern "C" ssize_t __wrap_send(int fd, const void* data, size_t size, int flags) {
    ++sends;
    require((flags & MSG_DONTWAIT) != 0, "send must not wait");
    require((flags & MSG_NOSIGNAL) != 0, "send must suppress SIGPIPE");
    if (send_error != 0) {
        errno = send_error;
        return -1;
    }
    if (short_send) return static_cast<ssize_t>(size) - 1;
    return __real_send(fd, data, size, flags);
}

extern "C" int __wrap_getsockopt(int fd, int level, int option, void* value, socklen_t* size) {
    if (getsockopt_error != 0) {
        errno = getsockopt_error;
        return -1;
    }
    if (socket_error != 0) {
        *static_cast<int*>(value) = socket_error;
        return 0;
    }
    return __real_getsockopt(fd, level, option, value, size);
}

struct Listener {
    char directory[64] = "/tmp/tuntom-switch-client.XXXXXX";
    std::string path;
    int fd = -1;

    Listener() {
        require(::mkdtemp(directory) != nullptr, "mkdtemp");
        path = std::string(directory) + "/switch.sock";
        fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        require(fd >= 0, "listener socket");
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        require(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind");
        require(::listen(fd, 1) == 0, "listen");
    }
    int accept() {
        const int peer = ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        require(peer >= 0, "accept");
        return peer;
    }
    ~Listener() {
        ::close(fd);
        ::unlink(path.c_str());
        ::rmdir(directory);
    }
};

static void registration_and_data(Client& client, int peer) {
    const auto expected = tuntom::encode_switch_registration("test");
    std::uint8_t buffer[128] {};
    const auto count = ::recv(peer, buffer, sizeof(buffer), MSG_DONTWAIT);
    require(count == static_cast<ssize_t>(expected.size()) and
        std::equal(expected.begin(), expected.end(), buffer), "exact registration record");
    const std::uint8_t payload[] {1, 2, 3};
    require(client.send(payload, sizeof(payload)) == 3, "DATA after registration");
    require(::recv(peer, buffer, sizeof(buffer), MSG_DONTWAIT) == 3, "peer receives DATA");
    require(__real_send(peer, payload, sizeof(payload), MSG_NOSIGNAL) == 3, "peer sends DATA");
    require(client.receive(buffer, sizeof(buffer)) == 3, "client receives DATA");
}

static void full_accept_queue() {
    Listener listener;
    Client first(listener.path, "test"), second(listener.path, "test"), retry(listener.path, "test");
    const auto now = Client::Clock::now();
    first.start_connect(now);
    second.start_connect(now);
    require(first.connected() and second.connected(), "fill accept queue");
    retry.start_connect(now);
    require(not retry.connected() and not retry.connecting() and retry.fd() == -1 and
        retry.last_error() == EAGAIN, "full queue must fail without pending socket");
    const int released = listener.accept();
    ::close(released);
    retry.start_connect(now + 1s);
    require(retry.connected() and retry.last_error() == 0, "recover after queue drains");
    ::close(listener.accept());
    const int peer = listener.accept();
    registration_and_data(retry, peer);
    ::close(peer);
}

static void pending_registration() {
    Listener listener;
    Client client(listener.path, "test");
    const auto now = Client::Time {};
    send_error = EAGAIN;
    client.start_connect(now);
    const int peer = listener.accept();
    require(client.connecting() and not client.connected() and client.poll_events() == POLLOUT,
        "registration backpressure must stay pending");
    std::uint8_t byte = 0;
    require(client.send(&byte, 1) == -1 and errno == ENOTCONN, "no DATA before registration");
    require(client.receive(&byte, 1) == -1 and errno == ENOTCONN, "no receive before registration");
    const auto before = sends;
    client.advance_connect(now + 1s, 0);
    require(sends == before, "no send retry without readiness");
    send_error = EINTR;
    client.advance_connect(now + 4s, POLLOUT);
    require(client.connecting(), "interrupted registration remains pending");
    require(client.poll_timeout_ms(now + 4999ms, 1000) == 1, "poll respects deadline");
    send_error = 0;
    client.advance_connect(now + 4999ms, POLLOUT);
    require(client.connected() and client.poll_events() == POLLIN, "registration resumes");
    registration_and_data(client, peer);
    ::close(peer);
}

static void pending_connect_and_timeout() {
    Listener listener;
    Client client(listener.path, "test");
    const auto now = Client::Time {};
    connect_error = EINPROGRESS;
    client.start_connect(now);
    const int peer = listener.accept();
    require(client.connecting() and not client.connected(), "async connect");
    getsockopt_error = EINTR;
    client.advance_connect(now + 1s, POLLOUT);
    require(client.connecting(), "interrupted SO_ERROR check");
    getsockopt_error = 0;
    send_error = EAGAIN;
    client.advance_connect(now + 4s, POLLOUT);
    require(client.connecting(), "connect completed but registration blocked");
    const int old_fd = client.fd();
    const auto before = sends;
    client.advance_connect(now + 5s, POLLOUT);
    require(client.fd() == -1 and client.last_error() == ETIMEDOUT and sends == before,
        "registration must not refresh connection deadline, even when writable");
    require(::fcntl(old_fd, F_GETFD) == -1 and errno == EBADF, "timed out socket closed");
    ::close(peer);
    connect_error = send_error = 0;
    client.start_connect(now + 6s);
    require(client.connected(), "fresh attempt after timeout");
    ::close(listener.accept());

    connect_error = EINPROGRESS;
    client.start_connect(now + 7s);
    const int stalled_peer = listener.accept();
    client.advance_connect(now + 12s, 0);
    require(client.fd() == -1 and client.last_error() == ETIMEDOUT, "timeout without readiness");
    ::close(stalled_peer);
    connect_error = 0;
}

static void failed_attempts() {
    Listener listener;
    Client client(listener.path, "test");
    const auto now = Client::Time {};
    for (int error : {ENOENT, EACCES, ECONNREFUSED, EINTR}) {
        connect_error = error;
        client.start_connect(now);
        require(client.fd() == -1 and client.last_error() == error, "failed connect cleaned up");
    }
    connect_error = EINPROGRESS;
    for (short event : {static_cast<short>(POLLOUT), static_cast<short>(POLLHUP),
                        static_cast<short>(POLLERR), static_cast<short>(POLLNVAL)}) {
        client.start_connect(now);
        const int peer = listener.accept();
        socket_error = event == POLLOUT ? ECONNREFUSED : 0;
        client.advance_connect(now + 1ms, event);
        require(client.fd() == -1 and client.last_error() != 0, "failed async completion");
        ::close(peer);
    }
    connect_error = socket_error = 0;
    for (int error : {EPIPE, ECONNRESET, 0}) {
        send_error = error;
        short_send = error == 0;
        client.start_connect(now);
        require(client.fd() == -1 and client.last_error() == (error ? error : EIO),
            "registration failure must close socket");
        ::close(listener.accept());
    }
    send_error = 0;
    short_send = false;
}

int main() {
    ::alarm(10); // A blocking-connect regression must fail, not hang the suite.
    full_accept_queue();
    pending_registration();
    pending_connect_and_timeout();
    failed_attempts();
    std::cout << "PASS: nonblocking switch connect, registration and deadlines\n";
}
