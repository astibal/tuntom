#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;

class Deadline {
public:
    int remaining_ms() const {
        const auto remaining = end_ - Clock::now();
        if (remaining <= Clock::duration::zero())
            throw std::runtime_error("Control request timed out after 5 seconds");
        return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
    }

    void wait(int fd, short events) const {
        for (;;) {
            pollfd descriptor {fd, events, 0};
            const int result = ::poll(&descriptor, 1, remaining_ms());
            if (result > 0) return; // The next syscall reports errors and peer closure.
            if (result < 0 and errno != EINTR)
                throw std::runtime_error("control poll failed: " + std::string(std::strerror(errno)));
        }
    }

    void retry_connect() const {
        // AF_UNIX EAGAIN means a full backlog, not an asynchronous connect.
        if (::poll(nullptr, 0, std::min(10, remaining_ms())) < 0 and errno != EINTR)
            throw std::runtime_error("control retry wait failed");
    }

private:
    Clock::time_point end_ = Clock::now() + std::chrono::seconds(5);
};

struct Socket {
    int fd;
    ~Socket() { if (fd >= 0) ::close(fd); }
};
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4 or std::string(argv[2]) != "show" or
            std::string(argv[3]) != "stats") {
            std::cerr << "Usage: " << argv[0] << " <control-socket> show stats\n";
            return 1;
        }
        const std::string path = argv[1];
        if (path.empty() or path.size() >= sizeof(sockaddr_un::sun_path))
            throw std::runtime_error("Invalid control socket path");
        const Deadline deadline;
        const Socket socket {::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
        const int fd = socket.fd;
        if (fd < 0) throw std::runtime_error(std::strerror(errno));
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        for (;;) {
            deadline.remaining_ms();
            if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
                break;
            if (errno == EINTR) continue;
            if (errno == EAGAIN or errno == EWOULDBLOCK) {
                deadline.retry_connect();
                continue;
            }
            if (errno == EINPROGRESS or errno == EALREADY) {
                deadline.wait(fd, POLLOUT);
                int error = 0;
                socklen_t length = sizeof(error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
                    throw std::runtime_error("Cannot read control connection error");
                if (error == 0) break;
                errno = error;
            }
            throw std::runtime_error("connect(" + path + ") failed: " + std::strerror(errno));
        }
        const std::string command = "show stats";
        for (;;) {
            deadline.remaining_ms();
            const ssize_t sent = ::send(fd, command.data(), command.size(), MSG_NOSIGNAL);
            if (sent == static_cast<ssize_t>(command.size())) break;
            if (sent < 0 and errno == EINTR) continue;
            if (sent < 0 and (errno == EAGAIN or errno == EWOULDBLOCK)) {
                deadline.wait(fd, POLLOUT);
                continue;
            }
            throw std::runtime_error("Cannot send control command");
        }
        std::vector<char> response(65536);
        ssize_t size;
        for (;;) {
            deadline.remaining_ms();
            size = ::recv(fd, response.data(), response.size(), MSG_TRUNC);
            if (size >= 0) break;
            if (errno == EINTR) continue;
            if (errno == EAGAIN or errno == EWOULDBLOCK) {
                deadline.wait(fd, POLLIN);
                continue;
            }
            throw std::runtime_error("Cannot receive control response");
        }
        if (size == 0 or static_cast<std::size_t>(size) > response.size())
            throw std::runtime_error("Invalid control response");
        std::cout.write(response.data(), size);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << "\n";
        return 1;
    }
}
