#pragma once

#include "accept_backoff.hpp"
#include "control_protocol.hpp"
#include "control_dispatcher.hpp"
#include <optional>
#include "flow_dump.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <map>
#include <functional>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace tuntom {

// One epoll FD integrates listener and bounded, nonblocking transactions into
// existing daemon loops. Slow clients never wait on the packet-processing thread.
class ControlSocket {
public:
    struct RemotePending {
        std::string id;
        std::function<std::optional<ControlResponse>()> poll;
    };
    using RemoteSubmit = std::function<RemotePending(ControlRequest, unsigned, unsigned)>;
    void set_routed(RemoteSubmit submit) { routed_submit_ = std::move(submit); }
private:
    RemoteSubmit routed_submit_;
    struct Client {
        std::string command, body, response, route;
        std::size_t expected = 0, sent = 0;
        bool header = false, responding = false, framed = false, response_header = false;
        bool success = true, rejected = false, remote = false, routed = false;
        unsigned retries = 5, wait_ms = 250;
        std::function<std::optional<ControlResponse>()> pending;
        AcceptBackoff::Time deadline;
    };
    int fd_ = -1, poller_ = -1;
    bool bound_ = false, listening_ = false;
    std::string path_;
    AcceptBackoff accept_backoff_;
    std::map<int, Client> clients_;
    static constexpr std::size_t max_clients = 8;

    void remove(int fd) {
        ::epoll_ctl(poller_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        clients_.erase(fd);
    }
    void maintain() {
        const auto now = AcceptBackoff::Clock::now();
        for (auto i = clients_.begin(); i != clients_.end();) {
            const int fd = i->first;
            const bool expired = now >= i->second.deadline;
            ++i;
            if (expired) remove(fd);
        }
        const bool enabled = clients_.size() < max_clients && accept_backoff_.ready(now);
        if (enabled != listening_) {
            epoll_event event{}; event.events = EPOLLIN; event.data.fd = fd_;
            if (::epoll_ctl(poller_, enabled ? EPOLL_CTL_ADD : EPOLL_CTL_DEL, fd_, &event) == 0)
                listening_ = enabled;
        }
    }
    void respond(int fd, Client &client, std::string response, bool success = true, bool rejected = false) {
        client.response = std::move(response);
        client.success = success;
        client.rejected = rejected;
        client.responding = true;
        client.deadline = AcceptBackoff::Clock::now() + std::chrono::seconds(5);
        epoll_event event{}; event.events = EPOLLOUT; event.data.fd = fd;
        if (::epoll_ctl(poller_, EPOLL_CTL_MOD, fd, &event) < 0)
            throw std::runtime_error("cannot prepare control response");
    }
    bool flush(int fd, Client &c) {
        for (unsigned turn = 0; turn < 4; ++turn) {
            const bool header = c.framed && !c.response_header;
            const std::string prefix = header ? std::string(c.success ? "OK " : c.rejected ? "REJECTED " : "ERROR ") +
                std::to_string(c.response.size()) + "\n" : "";
            const auto count = header ? prefix.size() :
                c.framed ? std::min(control_chunk_size, c.response.size() - c.sent) : c.response.size();
            if (!count) return true;
            const auto sent = ::send(fd, header ? prefix.data() : c.response.data() + c.sent,
                                     count, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return false;
            if (sent != static_cast<ssize_t>(count)) return true;
            if (header) c.response_header = true;
            else c.sent += count;
            if (c.sent == c.response.size() && (!c.framed || c.response_header)) return true;
        }
        return false;
    }
    [[noreturn]] void fail(const std::string &message) {
        const std::string error = std::strerror(errno);
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        if (poller_ >= 0) { ::close(poller_); poller_ = -1; }
        if (bound_) { ::unlink(path_.c_str()); bound_ = false; }
        throw std::runtime_error(message + ": " + error);
    }
  public:
    explicit ControlSocket(const std::string &path) : path_(path) {
        if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
            throw std::runtime_error("Invalid control socket path");
        fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd_ < 0) fail("control socket() failed");
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (::bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
            fail("control bind(" + path + ") failed");
        bound_ = true;
        if (::chmod(path.c_str(), 0660) < 0 || ::listen(fd_, 8) < 0) fail("Cannot prepare control socket");
        poller_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (poller_ < 0) fail("control epoll_create1() failed");
        epoll_event event{}; event.events = EPOLLIN; event.data.fd = fd_;
        if (::epoll_ctl(poller_, EPOLL_CTL_ADD, fd_, &event) < 0) fail("control epoll_ctl() failed");
        listening_ = true;
    }
    ~ControlSocket() {
        for (const auto &entry : clients_) ::close(entry.first);
        if (poller_ >= 0) ::close(poller_);
        if (fd_ >= 0) ::close(fd_);
        if (bound_) ::unlink(path_.c_str());
    }
    ControlSocket(const ControlSocket &) = delete;
    ControlSocket &operator=(const ControlSocket &) = delete;
    int fd() const { return fd_; }
    int poll_fd() { maintain(); return poller_; }
    int poll_timeout_ms(AcceptBackoff::Time now, int maximum) const {
        auto result = accept_backoff_.poll_timeout_ms(now, maximum);
        for (const auto &entry : clients_) result = deadline_timeout_ms(now, entry.second.deadline, result);
        return result;
    }
    void write_stats(std::ostream &out) const {
        accept_backoff_.write_stats(out, "control", AcceptBackoff::Clock::now());
    }
    template<class StatsProvider> void handle(StatsProvider stats) {
        handle(stats, [](const std::string &, const std::string &) -> std::string {
            throw std::runtime_error("rules commands are supported only by switches");
        });
    }
    template<class StatsProvider, class RulesProvider> void handle(StatsProvider stats, RulesProvider rules) {
        handle(stats, rules, [] { return FlowDump("none").finish(); });
    }
    template<class StatsProvider, class RulesProvider, class FlowsProvider>
    void handle(StatsProvider stats, RulesProvider rules, FlowsProvider flows,
                std::function<std::string(const std::string&, const std::string&)> classifier = {},
                ControlDispatcher::Provider ports = {}) {
        ControlDispatcher dispatcher;
        dispatcher.stats = stats; dispatcher.flows = flows;
        dispatcher.rules = rules; dispatcher.classifier = classifier; dispatcher.ports = ports;
        handle_dispatch(dispatcher);
    }
    void handle_dispatch(const ControlDispatcher& dispatcher, RemoteSubmit remote = {}) {
        maintain();
        for (auto& entry : clients_) {
            auto& c = entry.second;
            if (!c.pending) continue;
            auto result = c.pending();
            if (result) {
                c.pending = {};
                respond(entry.first, c, std::move(result->body), result->success, result->rejected);
            }
        }
        std::array<epoll_event, max_clients + 1> events{};
        const int count = ::epoll_wait(poller_, events.data(), static_cast<int>(events.size()), 0);
        for (int i = 0; i < count; ++i) {
            const int fd = events[static_cast<std::size_t>(i)].data.fd;
            if (fd == fd_) {
                if (!listening_) continue;
                const int accepted = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (accepted < 0) { accept_backoff_.failed(errno, AcceptBackoff::Clock::now()); continue; }
                try {
                    auto &c = clients_[accepted];
                    c.deadline = AcceptBackoff::Clock::now() + std::chrono::seconds(5);
                    epoll_event event{}; event.events = EPOLLIN; event.data.fd = accepted;
                    if (::epoll_ctl(poller_, EPOLL_CTL_ADD, accepted, &event) < 0) remove(accepted);
                } catch (...) { remove(accepted); throw; }
                continue;
            }
            auto found = clients_.find(fd);
            if (found == clients_.end()) continue;
            auto &c = found->second;
            try {
                if (c.pending) { if (events[static_cast<std::size_t>(i)].events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) remove(fd); continue; }
                if (c.responding) { if (flush(fd, c)) remove(fd); continue; }
                for (unsigned turn = 0; turn < 4 && !c.responding; ++turn) {
                    std::array<char, control_chunk_size> bytes{};
                    const auto size = ::recv(fd, bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_TRUNC);
                    if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
                    if (size <= 0) { remove(fd); break; }
                    if (static_cast<std::size_t>(size) > bytes.size()) throw std::runtime_error("control record too large");
                    if (!c.header) {
                        std::string command(bytes.data(), static_cast<std::size_t>(size));
                        while (!command.empty() && (command.back() == '\n' || command.back() == '\r')) command.pop_back();
                        c.header = true;
                        if (command.compare(0, 7, "remote ") == 0 || command.compare(0, 7, "routed ") == 0) {
                            c.routed = command.compare(0, 7, "routed ") == 0;
                            c.remote = c.framed = true;
                            const auto a = command.find(' ', 7), b = a == std::string::npos ? a : command.find(' ', a + 1);
                            if (b == std::string::npos) throw std::runtime_error("expected remote RETRIES WAIT_MS COMMAND");
                            c.retries = static_cast<unsigned>(control_length(command.substr(7, a - 7), 100));
                            c.wait_ms = static_cast<unsigned>(control_length(command.substr(a + 1, b - a - 1), 60000));
                            if (c.wait_ms < 10) throw std::runtime_error("remote wait must be at least 10 ms");
                            command = command.substr(b + 1);
                            if(c.routed) {
                                const auto space=command.find(' ');
                                if(space==std::string::npos)throw std::runtime_error("missing routed command");
                                c.route=command.substr(0,space);command=command.substr(space+1);
                            }
                        }
                        // Preserve the legacy framing even for malformed local commands.
                        if (!c.remote) {
                            c.framed = command == "show flows" || command == "show ports" || command.compare(0, 11, "classifier ") == 0 ||
                                command.compare(0, 6, "rules ") == 0 || command.compare(0, 7, "divert ") == 0;
                            if (!c.framed && command != "show stats") {
                                respond(fd, c, "error=unknown_command\n"); break;
                            }
                        }
                        const auto operation = (c.routed && command=="discover") || (c.remote && command.compare(0, 15, "request status ") == 0)
                            ? ControlOperation{} : ControlDispatcher::parse(command);
                        c.command = command; c.expected = operation.length;
                        c.framed = c.remote || operation.framed;
                    } else {
                        if (static_cast<std::size_t>(size) > c.expected - c.body.size())
                            throw std::runtime_error("control body exceeds declared length");
                        c.body.append(bytes.data(), static_cast<std::size_t>(size));
                    }
                    if (c.body.size() == c.expected) {
                        if (c.remote) {
                            const auto& submit=c.routed?routed_submit_:remote;
                            if (!submit) throw std::runtime_error("remote control unsupported by this component");
                            auto pending = submit({c.routed?c.route+" "+c.command:c.command, std::move(c.body)}, c.retries, c.wait_ms);
                            c.pending = std::move(pending.poll);
                            const auto accepted = "REQUEST " + pending.id + "\n";
                            if (::send(fd, accepted.data(), accepted.size(), MSG_DONTWAIT | MSG_NOSIGNAL) != static_cast<ssize_t>(accepted.size())) {
                                remove(fd); break;
                            }
                            // The daemon owns the request even if this client disconnects.
                            c.deadline = AcceptBackoff::Clock::now() + std::chrono::hours(2);
                            epoll_event event{}; event.events = EPOLLRDHUP; event.data.fd = fd;
                            ::epoll_ctl(poller_, EPOLL_CTL_MOD, fd, &event);
                            break;
                        }
                        const auto response = dispatcher.execute({c.command, c.body}, ControlAccess::all());
                        respond(fd, c, !c.framed && !response.success ? "error=" + response.body : response.body, response.success);

                    }
                }
            } catch (const std::bad_alloc &) {
                constexpr char error[] = "error=out_of_memory\n";
                ::send(fd, error, sizeof(error) - 1, MSG_DONTWAIT | MSG_NOSIGNAL);
                remove(fd); throw;
            } catch (const std::exception &e) {
                try { respond(fd, c, std::string(e.what()) + "\n", false); }
                catch (...) { remove(fd); throw; }
            }
        }
        maintain();
    }
};
} // namespace tuntom
