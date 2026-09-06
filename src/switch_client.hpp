#pragma once

#include "ipc/switch_protocol.hpp"
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
    SwitchClient(const std::string& path, const std::string& port_id)
        : path_(path), port_id_(port_id) {
        if (path.size() >= sizeof(sockaddr_un::sun_path)) {
            throw std::runtime_error("Switch socket path is too long");
        }
        // Validate the registration once. Connection failures are runtime
        // state and must not prevent the tuntom link from starting.
        encode_switch_registration(port_id_);
    }

    ~SwitchClient() { disconnect(); }

    bool connect_now() {
        disconnect();
        last_error_ = 0;

        fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            last_error_ = errno;
            return false;
        }

        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
        if (::connect(
                fd_,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) < 0) {
            last_error_ = errno;
            disconnect();
            return false;
        }

        const auto registration = encode_switch_registration(port_id_);
        const ssize_t registered = ::send(
            fd_, registration.data(), registration.size(), MSG_NOSIGNAL);
        if (registered != static_cast<ssize_t>(registration.size())) {
            last_error_ = registered < 0 ? errno : EIO;
            disconnect();
            return false;
        }

        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0 or ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
            last_error_ = errno;
            disconnect();
            return false;
        }
        return true;
    }

    void disconnect() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd() const { return fd_; }
    bool connected() const { return fd_ >= 0; }
    int last_error() const { return last_error_; }

    ssize_t receive(std::uint8_t* buffer, std::size_t size) {
        if (fd_ < 0) {
            errno = ENOTCONN;
            return -1;
        }
        return ::recv(fd_, buffer, size, MSG_TRUNC);
    }

    ssize_t send(const std::uint8_t* buffer, std::size_t size) {
        if (fd_ < 0) {
            errno = ENOTCONN;
            return -1;
        }
        return ::send(fd_, buffer, size, MSG_DONTWAIT | MSG_NOSIGNAL);
    }

private:
    int fd_ = -1;
    int last_error_ = 0;
    std::string path_;
    std::string port_id_;
};

} // namespace tuntom
