#include "../src/tun_fd_socket.hpp"

#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    int pair[2]{};
    assert(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
    const int source = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(source >= 0);
    tuntom::tun_fd_socket::send(pair[0], source, "ut42c", 1420);
    const auto received = tuntom::tun_fd_socket::receive(pair[1]);
    assert(received.fd >= 0);
    assert(received.interface_name == "ut42c");
    assert(received.mtu == 1420);
    assert((::fcntl(received.fd, F_GETFD) & FD_CLOEXEC) != 0);
    assert((::fcntl(received.fd, F_GETFL) & O_NONBLOCK) != 0);
    ::close(received.fd);
    ::close(source);
    ::close(pair[0]);
    ::close(pair[1]);
}
