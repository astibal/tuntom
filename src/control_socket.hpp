#pragma once

#include "accept_backoff.hpp"
#include <array>
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
    static constexpr std::size_t max_clients = 4;
    static constexpr auto request_timeout = std::chrono::milliseconds(10);
    static constexpr std::size_t max_response = 65536;

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
        for (auto& client : clients_) close_client(client);
        if (fd_ >= 0) ::close(fd_);
        if (bound_) ::unlink(path_.c_str());
    }

    int fd() const { return fd_; }
    int poll_fd() const {
        if (not accept_backoff_.ready(AcceptBackoff::Clock::now())) return -1;
        for (const auto& client : clients_) if (client.fd < 0) return fd_;
        return -1;
    }
    void poll_clients(pollfd* descriptors) const {
        for (std::size_t i = 0; i < clients_.size(); ++i)
            descriptors[i] = {clients_[i].fd,
                static_cast<short>(clients_[i].response.empty() ? POLLIN : POLLOUT), 0};
    }
    int poll_timeout_ms(AcceptBackoff::Time now, int maximum) const {
        maximum = accept_backoff_.poll_timeout_ms(now, maximum);
        for (const auto& client : clients_)
            if (client.fd >= 0) maximum = deadline_timeout_ms(now, client.deadline, maximum);
        return maximum;
    }
    void poll_events(short events) {
        if (events & (POLLERR | POLLHUP | POLLNVAL))
            accept_backoff_.failed(events & POLLNVAL ? EBADF : EIO, AcceptBackoff::Clock::now());
    }
    void write_stats(std::ostream& out) const {
        accept_backoff_.write_stats(out, "control", AcceptBackoff::Clock::now());
    }

    template<typename StatsProvider>
    void handle(StatsProvider provider, bool accept_ready = true) {
        // One admission per turn and a fixed client array bound work and FDs.
        if (accept_ready and poll_fd() >= 0) {
            const int fd = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd < 0) accept_backoff_.failed(errno, AcceptBackoff::Clock::now());
            else {
                for (auto& client : clients_) {
                    if (client.fd >= 0) continue;
                    client.fd = fd;
                    client.deadline = AcceptBackoff::Clock::now() + request_timeout;
                    break;
                }
            }
        }
        // Try ready I/O before timeout cleanup, as poll may resume late after
        // scheduler delays. A queued request must not be mistaken for an idle peer.
        for (auto& client : clients_) {
            if (client.fd < 0) continue;
            try {
                if (client.response.empty()) {
                    char request[64] {};
                    const ssize_t size = ::recv(client.fd, request, sizeof(request), MSG_TRUNC);
                    if (size < 0 and (errno == EAGAIN or errno == EWOULDBLOCK or errno == EINTR)) {
                        if (AcceptBackoff::Clock::now() >= client.deadline) close_client(client);
                        continue;
                    }
                    if (size <= 0) {
                        close_client(client);
                        continue;
                    }
                    if (static_cast<std::size_t>(size) > sizeof(request)) {
                        client.response = "error=unknown_command\n";
                    } else {
                        std::string command(request, static_cast<std::size_t>(size));
                        while (not command.empty() and
                               (command.back() == '\n' or command.back() == '\r'))
                            command.pop_back();
                        client.response = command == "show stats"
                            ? provider() : "error=unknown_command\n";
                        if (client.response.empty() or client.response.size() > max_response)
                            client.response = "error=invalid_response\n";
                    }
                }
                const ssize_t sent = ::send(client.fd, client.response.data(), client.response.size(),
                                            MSG_DONTWAIT | MSG_NOSIGNAL);
                if (sent < 0 and (errno == EAGAIN or errno == EWOULDBLOCK or errno == EINTR)) {
                    if (AcceptBackoff::Clock::now() >= client.deadline) close_client(client);
                    continue;
                }
                // SOCK_SEQPACKET sends a whole record or fails. Never send a suffix.
                close_client(client);
            } catch (const std::bad_alloc&) {
                constexpr char error[] = "error=out_of_memory\n";
                ::send(client.fd, error, sizeof(error) - 1, MSG_DONTWAIT | MSG_NOSIGNAL);
                close_client(client);
                throw;
            } catch (...) {
                close_client(client);
                throw;
            }
        }
    }

private:
    struct Client {
        int fd = -1;
        AcceptBackoff::Time deadline {};
        std::string response;
    };
    static void close_client(Client& client) {
        if (client.fd >= 0) ::close(client.fd);
        client.fd = -1;
        // Release capacity too, including after a provider returned an oversized value.
        std::string().swap(client.response);
    }
    std::array<Client, max_clients> clients_ {};

    [[noreturn]] void fail(const std::string& message) {
        const std::string error = std::strerror(errno);
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        if (bound_) ::unlink(path_.c_str());
        throw std::runtime_error(message + ": " + error);
    }

    int fd_ = -1;
    bool bound_ = false;
    std::string path_;
    AcceptBackoff accept_backoff_;
};

} // namespace tuntom
