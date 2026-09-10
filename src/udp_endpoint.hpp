#pragma once

#include "common.hpp"
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tuntom {

class UdpEndpoint {
public:
    ~UdpEndpoint() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    void open_server(std::uint16_t port) {
        fd_ = ::socket(
            AF_INET6,
            SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
            0);
        if (fd_ < 0) {
            throw std::runtime_error(
                "socket() failed: " +
                std::string(std::strerror(errno)));
        }

        int reuse_address = 1;
        if (
            ::setsockopt(
                fd_,
                SOL_SOCKET,
                SO_REUSEADDR,
                &reuse_address,
                sizeof(reuse_address)) < 0) {

            const std::string error = std::strerror(errno);
            ::close(fd_);
            fd_ = -1;

            throw std::runtime_error(
                "setsockopt(SO_REUSEADDR) failed: " + error);
        }

        configure_pmtud_socket(AF_INET6);

        int v6_only = 0;
        ::setsockopt(
            fd_,
            IPPROTO_IPV6,
            IPV6_V6ONLY,
            &v6_only,
            sizeof(v6_only));

        sockaddr_in6 address {};
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_any;
        address.sin6_port = htons(port);

        if (
            ::bind(
                fd_,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) < 0) {

            throw std::runtime_error(
                "bind() failed: " +
                std::string(std::strerror(errno)));
        }

        // A dual-stack server socket is treated conservatively as IPv6 for
        // transport-MTU calculations. IPv4-mapped peers therefore merely
        // get 20 bytes of extra safety margin.
        outer_ip_header_size_ = ipv6_header_size;
    }

    void open_client(const std::string& host, std::uint16_t port) {
        addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;

        addrinfo* result = nullptr;
        const std::string service = std::to_string(port);

        const int rc =
            ::getaddrinfo(
                host.c_str(),
                service.c_str(),
                &hints,
                &result);

        if (rc != 0) {
            throw std::runtime_error(
                "getaddrinfo() failed: " +
                std::string(gai_strerror(rc)));
        }

        for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
            const int candidate =
                ::socket(
                    item->ai_family,
                    SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                    0);

            if (candidate < 0) {
                continue;
            }

            fd_ = candidate;
            configure_pmtud_socket(item->ai_family);
            std::memset(&peer_, 0, sizeof(peer_));
            std::memcpy(&peer_, item->ai_addr, item->ai_addrlen);
            peer_length_ = static_cast<socklen_t>(item->ai_addrlen);
            peer_valid_ = true;

            outer_ip_header_size_ =
                item->ai_family == AF_INET
                    ? ipv4_header_min_size
                    : ipv6_header_size;

            break;
        }

        ::freeaddrinfo(result);

        if (fd_ < 0 or not peer_valid_) {
            throw std::runtime_error("Cannot create UDP client socket");
        }
    }

    int fd() const {
        return fd_;
    }

    ssize_t receive(
        std::uint8_t* buffer,
        std::size_t size,
        sockaddr_storage& source,
        socklen_t& source_length) {

        source = {};
        source_length = sizeof(source);

        return ::recvfrom(
            fd_,
            buffer,
            size,
            0,
            reinterpret_cast<sockaddr*>(&source),
            &source_length);
    }

    bool set_peer(
        const sockaddr_storage& peer,
        socklen_t peer_length) {

        const bool changed =
            not peer_valid_ or
            not peer_matches(peer, peer_length);

        peer_ = peer;
        peer_length_ = peer_length;
        peer_valid_ = true;

        if (peer.ss_family == AF_INET) {
            outer_ip_header_size_ = ipv4_header_min_size;
        } else if (peer.ss_family == AF_INET6) {
            const auto* address =
                reinterpret_cast<const sockaddr_in6*>(&peer);

            outer_ip_header_size_ =
                IN6_IS_ADDR_V4MAPPED(&address->sin6_addr)
                    ? ipv4_header_min_size
                    : ipv6_header_size;
        }

        return changed;
    }

    ssize_t send_to(const std::uint8_t* buffer, std::size_t size,
                    const sockaddr_storage& destination, socklen_t length) {
        return ::sendto(fd_, buffer, size, 0,
            reinterpret_cast<const sockaddr*>(&destination), length);
    }

    ssize_t send(const std::uint8_t* buffer, std::size_t size) {
        if (not peer_valid_) {
            return 0;
        }

        return ::sendto(
            fd_,
            buffer,
            size,
            0,
            reinterpret_cast<const sockaddr*>(&peer_),
            peer_length_);
    }

    struct BatchResult {
        std::size_t packets = 0;
        std::size_t bytes = 0;
        int error = 0;
    };

    BatchResult send_batch(
        const std::vector<std::uint8_t>* buffers, std::size_t count) {
        if (count == 0) return {};
        if (count > max_fragments_per_packet) return {0, 0, EINVAL};
        if (not peer_valid_) return {0, 0, EDESTADDRREQ};
        if (count == 1) {
            const ssize_t sent = send(buffers[0].data(), buffers[0].size());
            if (sent < 0) return {0, 0, errno};
            return {1, static_cast<std::size_t>(sent), 0};
        }

        std::array<iovec, max_fragments_per_packet> vectors;
        std::array<mmsghdr, max_fragments_per_packet> messages;
        for (std::size_t i = 0; i < count; ++i) {
            vectors[i] = {const_cast<std::uint8_t*>(buffers[i].data()), buffers[i].size()};
            messages[i] = {};
            messages[i].msg_hdr.msg_name = &peer_;
            messages[i].msg_hdr.msg_namelen = peer_length_;
            messages[i].msg_hdr.msg_iov = &vectors[i];
            messages[i].msg_hdr.msg_iovlen = 1;
        }

        BatchResult result;
        while (result.packets < count) {
            const int sent = ::sendmmsg(
                fd_, messages.data() + result.packets,
                static_cast<unsigned>(count - result.packets), MSG_DONTWAIT);
            if (sent <= 0) {
                result.error = sent < 0 ? errno : EIO;
                break;
            }
            // A short batch hides the later error. Retry only the unsent
            // suffix, preserving both the wire bytes and successful counters.
            const std::size_t end = result.packets + static_cast<std::size_t>(sent);
            for (; result.packets < end; ++result.packets)
                result.bytes += messages[result.packets].msg_len;
        }
        return result;
    }

    std::size_t outer_ip_header_size() const {
        return outer_ip_header_size_;
    }

private:
    bool peer_matches(
        const sockaddr_storage& peer,
        socklen_t peer_length) const {

        if (
            peer_length_ != peer_length or
            peer_.ss_family != peer.ss_family) {

            return false;
        }

        if (peer.ss_family == AF_INET) {
            const auto* current =
                reinterpret_cast<const sockaddr_in*>(&peer_);
            const auto* candidate =
                reinterpret_cast<const sockaddr_in*>(&peer);

            return
                current->sin_port == candidate->sin_port and
                current->sin_addr.s_addr == candidate->sin_addr.s_addr;
        }

        if (peer.ss_family == AF_INET6) {
            const auto* current =
                reinterpret_cast<const sockaddr_in6*>(&peer_);
            const auto* candidate =
                reinterpret_cast<const sockaddr_in6*>(&peer);

            return
                current->sin6_port == candidate->sin6_port and
                current->sin6_scope_id == candidate->sin6_scope_id and
                std::memcmp(
                    &current->sin6_addr,
                    &candidate->sin6_addr,
                    sizeof(in6_addr)) == 0;
        }

        return std::memcmp(&peer_, &peer, peer_length) == 0;
    }

    void configure_pmtud_socket(int family) {
#ifdef IP_MTU_DISCOVER
        if (family == AF_INET) {
            int mode = IP_PMTUDISC_DO;
            if (
                ::setsockopt(
                    fd_,
                    IPPROTO_IP,
                    IP_MTU_DISCOVER,
                    &mode,
                    sizeof(mode)) < 0) {

                throw std::runtime_error(
                    "setsockopt(IP_MTU_DISCOVER) failed: " +
                    std::string(std::strerror(errno)));
            }
        }
#endif

#ifdef IPV6_MTU_DISCOVER
        if (family == AF_INET6) {
            int mode = IPV6_PMTUDISC_DO;
            if (
                ::setsockopt(
                    fd_,
                    IPPROTO_IPV6,
                    IPV6_MTU_DISCOVER,
                    &mode,
                    sizeof(mode)) < 0) {

                throw std::runtime_error(
                    "setsockopt(IPV6_MTU_DISCOVER) failed: " +
                    std::string(std::strerror(errno)));
            }
        }
#endif
    }

    int fd_ = -1;
    sockaddr_storage peer_ {};
    socklen_t peer_length_ = 0;
    bool peer_valid_ = false;
    std::size_t outer_ip_header_size_ = ipv6_header_size;
};

} // namespace tuntom
