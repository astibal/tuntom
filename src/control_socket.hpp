#pragma once

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>

namespace tuntom {

class ControlSocket {
public:
    explicit ControlSocket(const std::string& path) : path_(path) {
        if (path.empty() or path.size() >= sizeof(sockaddr_un::sun_path))
            throw std::runtime_error("Invalid control socket path");
        fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd_ < 0) fail("control socket() failed");
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
            fail("control bind(" + path + ") failed");
        bound_ = true;
        if (::chmod(path.c_str(), 0660) < 0 or ::listen(fd_, 8) < 0)
            fail("Cannot prepare control socket");
    }

    ~ControlSocket() {
        if (fd_ >= 0) ::close(fd_);
        if (bound_) ::unlink(path_.c_str());
    }

    int fd() const { return fd_; }

    template<typename StatsProvider>
    void handle(StatsProvider provider) {
        const int client = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client < 0) return;
        struct CloseClient {
            int fd;
            ~CloseClient() { ::close(fd); }
        } close_client {client};
        try {
            char request[64] {};
            pollfd descriptor {client, POLLIN, 0};
            const ssize_t size = ::poll(&descriptor, 1, 10) > 0
                ? ::recv(client, request, sizeof(request), 0)
                : -1;
            std::string response;
            if (size > 0) {
                std::string command(request, static_cast<std::size_t>(size));
                while (not command.empty() and
                       (command.back() == '\n' or command.back() == '\r'))
                    command.pop_back();
                response = command == "show stats"
                    ? provider()
                    : "error=unknown_command\n";
            } else {
                response = "error=empty_command\n";
            }
            ::send(client, response.data(), response.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        } catch (const std::bad_alloc&) {
            // Report without allocating, then let the loop count/back off.
            constexpr char error[] = "error=out_of_memory\n";
            ::send(client, error, sizeof(error) - 1, MSG_DONTWAIT | MSG_NOSIGNAL);
            throw;
        }
    }

private:
    [[noreturn]] void fail(const std::string& message) {
        const std::string error = std::strerror(errno);
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        if (bound_) ::unlink(path_.c_str());
        throw std::runtime_error(message + ": " + error);
    }

    int fd_ = -1;
    bool bound_ = false;
    std::string path_;
};

} // namespace tuntom
