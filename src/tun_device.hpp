#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
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

        fd_ = ::open("/dev/net/tun", O_RDWR | O_CLOEXEC);
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

    ssize_t read_packet(std::uint8_t* buffer, std::size_t size) {
        return ::read(fd_, buffer, size);
    }

    ssize_t write_packet(const std::uint8_t* buffer, std::size_t size) {
        return ::write(fd_, buffer, size);
    }

private:
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
