#pragma once

#include "common.hpp"
#include "accept_backoff.hpp"
#include <poll.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
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

    using Clock = AcceptBackoff::Clock;
    using Time = Clock::time_point;
    static constexpr auto retry_interval = std::chrono::seconds(1);
    static constexpr auto error_pause = std::chrono::milliseconds(100);

    void open_server(std::uint16_t port) {
        server_ = true;
        family_ = AF_INET6;
        sockaddr_in6 address {};
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_any;
        address.sin6_port = htons(port);
        std::memcpy(&local_, &address, sizeof(address));
        local_length_ = sizeof(address);
        if (not create_socket()) startup_error();
        outer_ip_header_size_ = ipv6_header_size;
    }

    void open_client(const std::string& host, std::uint16_t port) {
        addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* result = nullptr;
        const std::string service = std::to_string(port);
        const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
        if (rc != 0) throw std::runtime_error("getaddrinfo() failed: " + std::string(gai_strerror(rc)));
        struct FreeAddresses {
            addrinfo* value;
            ~FreeAddresses() { ::freeaddrinfo(value); }
        } guard {result};
        server_ = false;
        for (const addrinfo* item = result; item; item = item->ai_next) {
            if ((item->ai_family != AF_INET and item->ai_family != AF_INET6) or
                item->ai_addrlen > sizeof(peer_)) continue;
            family_ = item->ai_family;
            local_ = {};
            local_.ss_family = static_cast<sa_family_t>(family_);
            local_length_ = family_ == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
            if (not create_socket()) continue;
            std::memcpy(&peer_, item->ai_addr, item->ai_addrlen);
            peer_length_ = static_cast<socklen_t>(item->ai_addrlen);
            peer_valid_ = true;
            outer_ip_header_size_ = family_ == AF_INET ? ipv4_header_min_size : ipv6_header_size;
            return;
        }
        startup_error();
    }

    int fd() const { return fd_; }
    int poll_fd() const { return Clock::now() >= resume_at_ ? fd_ : -1; }
    int poll_timeout_ms(Time now, int maximum) const {
        return now < resume_at_ ? deadline_timeout_ms(now, resume_at_, maximum) : maximum;
    }
    void maintain(Time now) {
        if (fd_ >= 0 or now < resume_at_) return;
        ++reopen_attempts_;
        if (create_socket()) {
            ++reopens_;
            resume_at_ = {};
        } else {
            last_error_ = errno;
            resume_at_ = now + retry_interval;
        }
    }
    void poll_events(short events) {
        if (fd_ < 0) return;
        if (events & (POLLHUP | POLLNVAL)) {
            retire(events & POLLNVAL ? EBADF : EIO);
        } else if (events & POLLERR) {
            int error = 0;
            socklen_t size = sizeof(error);
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &size) < 0) error = errno;
            // Even an unexplained/repeating error must not keep poll hot.
            io_error(error ? error : EIO);
        }
    }
    void write_stats(std::ostream& out) const {
        out << "udp_endpoint_available=" << (fd_ >= 0 ? 1 : 0) << '\n'
            << "udp_endpoint_errors=" << errors_ << '\n'
            << "udp_endpoint_last_errno=" << last_error_ << '\n'
            << "udp_reopen_attempts=" << reopen_attempts_ << '\n'
            << "udp_reopens=" << reopens_ << '\n';
    }

    ssize_t receive(std::uint8_t* buffer, std::size_t size,
                    sockaddr_storage& source, socklen_t& source_length) {
        source = {};
        source_length = sizeof(source);
        if (poll_fd() < 0) { errno = EAGAIN; return -1; }
        const auto received = ::recvfrom(fd_, buffer, size, 0,
            reinterpret_cast<sockaddr*>(&source), &source_length);
        if (received < 0) record_io_error();
        return received;
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
        if (fd_ < 0) { errno = ENETDOWN; return -1; }
        const auto sent = ::sendto(fd_, buffer, size, 0,
            reinterpret_cast<const sockaddr*>(&destination), length);
        if (sent < 0) record_io_error();
        return sent;
    }

    ssize_t send(const std::uint8_t* buffer, std::size_t size) {
        if (not peer_valid_) return 0;
        return send_to(buffer, size, peer_, peer_length_);
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

    void retire(int error) {
        ++errors_;
        last_error_ = error;
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        resume_at_ = Clock::now() + retry_interval;
    }
    void io_error(int error) {
        if (error == EAGAIN or error == EWOULDBLOCK or error == EINTR) return;
        switch (error) {
        case ECONNREFUSED: case EHOSTUNREACH: case ENETUNREACH: case ENETDOWN:
        case EMSGSIZE: case ENOBUFS: case ENOMEM: case EACCES: case EPERM:
            ++errors_;
            last_error_ = error;
            resume_at_ = Clock::now() + error_pause;
            break;
        default:
            retire(error);
        }
    }
    void record_io_error() {
        const int error = errno;
        io_error(error);
        errno = error;
    }
    [[noreturn]] void startup_error() {
        throw std::runtime_error("Cannot prepare UDP socket: " + std::string(std::strerror(errno)));
    }
    bool create_socket() {
        const int candidate = ::socket(family_, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (candidate < 0) return false;
        const auto fail = [&] {
            const int error = errno;
            ::close(candidate);
            errno = error;
            return false;
        };
        if (server_) {
            int reuse = 1, v6_only = 0;
            if (::setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 or
                ::setsockopt(candidate, IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, sizeof(v6_only)) < 0)
                return fail();
        }
#ifdef IP_MTU_DISCOVER
        if (family_ == AF_INET) {
            int mode = IP_PMTUDISC_DO;
            if (::setsockopt(candidate, IPPROTO_IP, IP_MTU_DISCOVER, &mode, sizeof(mode)) < 0)
                return fail();
        }
#endif
#ifdef IPV6_MTU_DISCOVER
        if (family_ == AF_INET6) {
            int mode = IPV6_PMTUDISC_DO;
            if (::setsockopt(candidate, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &mode, sizeof(mode)) < 0)
                return fail();
        }
#endif
        if (::bind(candidate, reinterpret_cast<const sockaddr*>(&local_), local_length_) < 0)
            return fail();
        sockaddr_storage bound {};
        socklen_t length = sizeof(bound);
        if (::getsockname(candidate, reinterpret_cast<sockaddr*>(&bound), &length) < 0)
            return fail();
        // Explicit client bind caches the ephemeral port before the first send.
        local_ = bound;
        local_length_ = length;
        fd_ = candidate;
        return true;
    }

    bool server_ = false;
    int family_ = AF_INET6;
    sockaddr_storage local_ {};
    socklen_t local_length_ = 0;
    Time resume_at_ {};
    std::uint64_t errors_ = 0, reopen_attempts_ = 0, reopens_ = 0;
    int last_error_ = 0;
    int fd_ = -1;
    sockaddr_storage peer_ {};
    socklen_t peer_length_ = 0;
    bool peer_valid_ = false;
    std::size_t outer_ip_header_size_ = ipv6_header_size;
};

} // namespace tuntom
