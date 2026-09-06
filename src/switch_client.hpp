#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace tuntom {

class SwitchClient {
public:
    explicit SwitchClient(const std::string& path) {
        if (path.size() >= sizeof(sockaddr_un::sun_path)) {
            throw std::runtime_error("Switch socket path is too long");
        }

        fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            throw std::runtime_error(
                "Cannot create switch socket: " +
                std::string(std::strerror(errno)));
        }

        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (::connect(
                fd_,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) < 0) {
            const std::string error = std::strerror(errno);
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error(
                "Cannot connect switch socket " + path + ": " + error);
        }

        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0 or ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
            const std::string error = std::strerror(errno);
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error(
                "Cannot make switch socket nonblocking: " + error);
        }
    }

    ~SwitchClient() {
        if (fd_ >= 0) ::close(fd_);
    }

    int fd() const { return fd_; }

    ssize_t receive(std::uint8_t* buffer, std::size_t size) {
        return ::recv(fd_, buffer, size, MSG_TRUNC);
    }

    ssize_t send(const std::uint8_t* buffer, std::size_t size) {
        return ::send(fd_, buffer, size, MSG_DONTWAIT | MSG_NOSIGNAL);
    }

private:
    int fd_ = -1;
};

} // namespace tuntom
