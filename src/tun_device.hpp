#pragma once

#include "accept_backoff.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <ostream>
#include <poll.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tuntom {

class TunDevice {
public:
    TunDevice(
        const std::string& interface_name,
        std::size_t mtu)
        : interface_name_(interface_name) {

        fd_ = ::open("/dev/net/tun", O_RDWR | O_CLOEXEC | O_NONBLOCK);
        if (fd_ < 0) {
            throw std::runtime_error(
                "Cannot open /dev/net/tun: " +
                std::string(std::strerror(errno)));
        }

        ifreq request {};
        request.ifr_flags = IFF_TUN | IFF_NO_PI;
        std::strncpy(
            request.ifr_name,
            interface_name.c_str(),
            IFNAMSIZ - 1);

        if (::ioctl(fd_, TUNSETIFF, &request) < 0) {
            const std::string error = std::strerror(errno);
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("TUNSETIFF failed: " + error);
        }

        interface_name_ = request.ifr_name;
        set_mtu(mtu);
    }

    ~TunDevice() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    int fd() const {
        return fd_;
    }

    int poll_fd() const { return AcceptBackoff::Clock::now() >= resume_at_ ? fd_ : -1; }
    int poll_timeout_ms(AcceptBackoff::Time now, int maximum) const {
        return now < resume_at_ ? deadline_timeout_ms(now, resume_at_, maximum) : maximum;
    }
    void poll_events(short events) {
        if (fd_ >= 0 and (events & (POLLERR | POLLHUP | POLLNVAL)))
            retire(events & POLLNVAL ? EBADF : EIO);
    }
    void write_stats(std::ostream& out) const {
        out << "tun_endpoint_available=" << (fd_ >= 0 ? 1 : 0) << '\n'
            << "tun_endpoint_failures=" << failures_ << '\n'
            << "tun_endpoint_last_errno=" << last_error_ << '\n';
    }
    ssize_t read_packet(std::uint8_t* buffer, std::size_t size) {
        if (fd_ < 0) { errno = ENODEV; return -1; }
        if (poll_fd() < 0) { errno = EAGAIN; return -1; }
        const auto result = ::read(fd_, buffer, size);
        if (result == 0) retire(ENODEV);
        if (result < 0) {
            const int error = errno;
            if (error != EAGAIN and error != EWOULDBLOCK and error != EINTR and
                error != ENOBUFS and error != ENOMEM) retire(error);
            if (error == ENOBUFS or error == ENOMEM)
                resume_at_ = AcceptBackoff::Clock::now() + std::chrono::milliseconds(100);
            errno = error;
        }
        return result;
    }

    ssize_t write_packet(const std::uint8_t* buffer, std::size_t size) {
        if (fd_ < 0) { errno = ENODEV; return -1; }
        const auto result = ::write(fd_, buffer, size);
        if (result < 0) {
            const int error = errno;
            // Linux returns EIO while IFF_UP is clear. Drop this packet but keep
            // the fd: a later write can succeed as soon as the interface is UP.
            // Invalid packet data (e.g. EINVAL/EMSGSIZE) must not disable TUN.
            if (error == EBADF or error == ENODEV or error == ENXIO)
                retire(error);
            errno = error;
        }
        return result;
    }

    void set_up() {
        const int socket_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (socket_fd < 0) {
            throw std::runtime_error(
                "Cannot create interface ioctl socket: " +
                std::string(std::strerror(errno)));
        }

        ifreq request {};
        std::strncpy(request.ifr_name, interface_name_.c_str(), IFNAMSIZ - 1);
        if (::ioctl(socket_fd, SIOCGIFFLAGS, &request) < 0) {
            const std::string error = std::strerror(errno);
            ::close(socket_fd);
            throw std::runtime_error("SIOCGIFFLAGS failed: " + error);
        }
        request.ifr_flags = static_cast<short>(request.ifr_flags | IFF_UP);
        if (::ioctl(socket_fd, SIOCSIFFLAGS, &request) < 0) {
            const std::string error = std::strerror(errno);
            ::close(socket_fd);
            throw std::runtime_error("SIOCSIFFLAGS failed: " + error);
        }
        ::close(socket_fd);
    }

private:
    void retire(int error) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        ++failures_;
        last_error_ = error;
    }
    AcceptBackoff::Time resume_at_ {};
    std::uint64_t failures_ = 0;
    int last_error_ = 0;
    void set_mtu(std::size_t mtu) {
        const int socket_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (socket_fd < 0) {
            throw std::runtime_error(
                "Cannot create MTU ioctl socket: " +
                std::string(std::strerror(errno)));
        }

        ifreq request {};
        std::strncpy(
            request.ifr_name,
            interface_name_.c_str(),
            IFNAMSIZ - 1);
        request.ifr_mtu = static_cast<int>(mtu);

        if (::ioctl(socket_fd, SIOCSIFMTU, &request) < 0) {
            const std::string error = std::strerror(errno);
            ::close(socket_fd);
            throw std::runtime_error("SIOCSIFMTU failed: " + error);
        }

        ::close(socket_fd);
    }

    int fd_ = -1;
    std::string interface_name_;
};

} // namespace tuntom
