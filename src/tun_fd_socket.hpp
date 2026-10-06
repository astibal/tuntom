#pragma once

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace tuntom::tun_fd_socket {

inline constexpr std::uint32_t protocol_magic = 0x5446554e; // "TFUN"
inline constexpr std::uint16_t protocol_version = 1;

struct Message {
    std::uint32_t magic = protocol_magic;
    std::uint16_t version = protocol_version;
    std::uint16_t reserved = 0;
    std::uint32_t mtu = 0;
    char interface_name[IFNAMSIZ]{};
};

struct Received {
    int fd = -1;
    std::string interface_name;
    std::size_t mtu = 0;
};

inline sockaddr_un address(const std::string& path) {
    if (path.empty()) throw std::runtime_error("TUN socket path must not be empty");
    sockaddr_un result{};
    result.sun_family = AF_UNIX;
    if (path.size() >= sizeof(result.sun_path))
        throw std::runtime_error("TUN socket path is too long");
    std::memcpy(result.sun_path, path.c_str(), path.size() + 1);
    return result;
}

inline int connect(const std::string& path) {
    const int socket_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (socket_fd < 0)
        throw std::runtime_error("Cannot create TUN fd socket: " + std::string(std::strerror(errno)));
    const auto endpoint = address(path);
    if (::connect(socket_fd, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) < 0) {
        const std::string error = std::strerror(errno);
        ::close(socket_fd);
        throw std::runtime_error("Cannot connect to TUN fd socket " + path + ": " + error);
    }
    return socket_fd;
}

inline Received receive(int socket_fd) {
    Message message{};
    std::array<char, CMSG_SPACE(sizeof(int))> control{};
    iovec part{&message, sizeof(message)};
    msghdr header{};
    header.msg_iov = &part;
    header.msg_iovlen = 1;
    header.msg_control = control.data();
    header.msg_controllen = control.size();
    const auto received = ::recvmsg(socket_fd, &header, MSG_CMSG_CLOEXEC);
    if (received < 0)
        throw std::runtime_error("Cannot receive TUN fd: " + std::string(std::strerror(errno)));
    cmsghdr* item = CMSG_FIRSTHDR(&header);
    int fd = -1;
    if (item && item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_RIGHTS &&
        item->cmsg_len >= CMSG_LEN(sizeof(int)))
        std::memcpy(&fd, CMSG_DATA(item), sizeof(fd));
    const bool valid = received == static_cast<ssize_t>(sizeof(message)) &&
        !(header.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) &&
        message.magic == protocol_magic && message.version == protocol_version &&
        message.reserved == 0 && message.mtu != 0 &&
        std::memchr(message.interface_name, '\0', sizeof(message.interface_name)) &&
        item && item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_RIGHTS &&
        item->cmsg_len == CMSG_LEN(sizeof(int)) && !CMSG_NXTHDR(&header, item);
    if (!valid) {
        if (fd >= 0) ::close(fd);
        throw std::runtime_error("Invalid TUN fd socket message (expected one fd and protocol v1 metadata)");
    }
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        const std::string error = std::strerror(errno);
        ::close(fd);
        throw std::runtime_error("Cannot configure received TUN fd: " + error);
    }
    return {fd, message.interface_name, message.mtu};
}

inline void send(int socket_fd, int fd, const std::string& interface_name, std::size_t mtu) {
    if (interface_name.empty() || interface_name.size() >= IFNAMSIZ || mtu > UINT32_MAX)
        throw std::runtime_error("Invalid TUN fd metadata");
    Message message{};
    message.mtu = static_cast<std::uint32_t>(mtu);
    std::memcpy(message.interface_name, interface_name.c_str(), interface_name.size() + 1);
    std::array<char, CMSG_SPACE(sizeof(int))> control{};
    iovec part{&message, sizeof(message)};
    msghdr header{};
    header.msg_iov = &part;
    header.msg_iovlen = 1;
    header.msg_control = control.data();
    header.msg_controllen = control.size();
    cmsghdr* item = CMSG_FIRSTHDR(&header);
    item->cmsg_level = SOL_SOCKET;
    item->cmsg_type = SCM_RIGHTS;
    item->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(item), &fd, sizeof(fd));
    if (::sendmsg(socket_fd, &header, MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(message)))
        throw std::runtime_error("Cannot send TUN fd: " + std::string(std::strerror(errno)));
}

} // namespace tuntom::tun_fd_socket
