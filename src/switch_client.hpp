#pragma once

#include "ipc/switch_protocol.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace tuntom {

class SwitchClient {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr auto connect_timeout = std::chrono::seconds(5);

    SwitchClient(const std::string& path, const std::string& port_id)
        : path_(path), registration_(encode_switch_registration(port_id)) {
        if (path.size() >= sizeof(sockaddr_un::sun_path)) {
            throw std::runtime_error("Switch socket path is too long");
        }
        // Validate and encode once: retries need no registration allocation.
    }

    ~SwitchClient() { disconnect(); }

    void start_connect(Time now) {
        disconnect();
        last_error_ = 0;
        connect_deadline_ = now + connect_timeout;

        // Nonblocking from creation: even connect() can wait indefinitely
        // when the Unix listener's accept queue is full.
        fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd_ < 0) {
            last_error_ = errno;
            return;
        }

        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
        if (::connect(
                fd_,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) < 0) {
            if (errno == EINPROGRESS) {
                state_ = State::connecting;
                return;
            }
            // AF_UNIX EAGAIN means no connection was queued. Do not poll it
            // as EINPROGRESS: close and let the caller's backoff start anew.
            fail(errno);
            return;
        }

        state_ = State::registering;
        send_registration();
    }

    void advance_connect(Time now, short revents) {
        if (not connecting()) return;
        // One fixed deadline covers connect and registration. Readiness and
        // temporary errors never renew it; each call does bounded work.
        if (now >= connect_deadline_) {
            fail(ETIMEDOUT);
            return;
        }
        if (revents & POLLNVAL) {
            fail(EBADF);
            return;
        }
        if (not (revents & (POLLOUT | POLLERR | POLLHUP))) return;
        if (state_ == State::connecting or (revents & (POLLERR | POLLHUP))) {
            int error = 0;
            socklen_t size = sizeof(error);
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
                if (errno != EINTR) fail(errno);
                return;
            }
            if (error != 0 or (revents & (POLLERR | POLLHUP))) {
                fail(error != 0 ? error : ECONNRESET);
                return;
            }
            state_ = State::registering;
        }
        if (revents & POLLOUT) send_registration();
    }

    void disconnect() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        state_ = State::disconnected;
    }

    int fd() const { return fd_; }
    bool connected() const { return state_ == State::connected; }
    bool connecting() const {
        return state_ == State::connecting or state_ == State::registering;
    }
    short poll_events() const { return connecting() ? POLLOUT : POLLIN; }
    int poll_timeout_ms(Time now, int maximum) const {
        if (not connecting()) return maximum;
        if (now >= connect_deadline_) return 0;
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(
            connect_deadline_ - now).count();
        return static_cast<int>(std::min<std::int64_t>(maximum, left));
    }
    int last_error() const { return last_error_; }

    ssize_t receive(std::uint8_t* buffer, std::size_t size) {
        if (not connected()) {
            errno = ENOTCONN;
            return -1;
        }
        return ::recv(fd_, buffer, size, MSG_DONTWAIT | MSG_TRUNC);
    }

    ssize_t send(const std::uint8_t* buffer, std::size_t size) {
        if (not connected()) {
            errno = ENOTCONN;
            return -1;
        }
        return ::send(fd_, buffer, size, MSG_DONTWAIT | MSG_NOSIGNAL);
    }

private:
    enum class State { disconnected, connecting, registering, connected };

    void fail(int error) {
        last_error_ = error;
        disconnect();
    }

    void send_registration() {
        const ssize_t sent = ::send(
            fd_, registration_.data(), registration_.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent == static_cast<ssize_t>(registration_.size())) {
            state_ = State::connected;
        } else if (sent < 0 and (errno == EAGAIN or errno == EWOULDBLOCK or errno == EINTR)) {
            // Retry the whole record on POLLOUT. SOCK_SEQPACKET preserves
            // message boundaries; no DATA may precede this registration.
        } else {
            fail(sent < 0 ? errno : EIO);
        }
    }

    int fd_ = -1;
    int last_error_ = 0;
    State state_ = State::disconnected;
    Time connect_deadline_ {};
    std::string path_;
    std::vector<std::uint8_t> registration_;
};

} // namespace tuntom
