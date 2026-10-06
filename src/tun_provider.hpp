#pragma once

#include "common.hpp"
#include "tun_device.hpp"
#include "tun_fd_socket.hpp"

#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tuntom::tun_provider {

struct Request {
    std::string interface_name;
    std::size_t mtu = default_tun_mtu;
    bool multiqueue = false;
    bool bring_up = false;
};

inline std::string namespace_path(const std::string& value) {
    if (value.rfind("pid:", 0) == 0) {
        const auto pid = value.substr(4);
        if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("network namespace pid must be pid:<number>");
        return "/proc/" + pid + "/ns/net";
    }
    if (value.find('/') != std::string::npos) return value;
    return "/run/netns/" + value;
}

inline void enter_namespace(const std::string& target) {
    if (target.empty()) return;
    const auto path = namespace_path(target);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("Cannot open network namespace " + path + ": " + std::strerror(errno));
    if (::setns(fd, CLONE_NEWNET) < 0) {
        const std::string error = std::strerror(errno);
        ::close(fd);
        throw std::runtime_error("setns(" + path + ") failed: " + error);
    }
    ::close(fd);
}

inline void create_and_send(
    int socket_fd, const std::string& interface_name, std::size_t mtu,
    const std::string& netns = {}, bool bring_up = false) {
    enter_namespace(netns);
    TunDevice tun(interface_name, mtu);
    if (bring_up) tun.set_up();
    tun_fd_socket::send(socket_fd, tun.fd(), tun.interface_name(), mtu);
}

inline void create_and_send_many(
    int socket_fd, const std::vector<Request>& requests, const std::string& netns) {
    enter_namespace(netns);
    std::vector<std::unique_ptr<TunDevice>> tuns;
    tuns.reserve(requests.size());
    for (const auto& request : requests) {
        auto tun = std::make_unique<TunDevice>(
            request.interface_name, request.mtu, request.multiqueue);
        if (request.bring_up) tun->set_up();
        tuns.push_back(std::move(tun));
    }
    for (std::size_t index = 0; index < requests.size(); ++index)
        tun_fd_socket::send(socket_fd, tuns[index]->fd(),
                            tuns[index]->interface_name(), requests[index].mtu);
}

inline std::vector<tun_fd_socket::Received> create_many_in_child(
    const std::vector<Request>& requests, const std::string& netns) {
    if (requests.empty()) throw std::runtime_error("TUN provider requires at least one interface");
    int pair[2]{};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) < 0)
        throw std::runtime_error("Cannot create TUN provider socketpair: " + std::string(std::strerror(errno)));
    const pid_t child = ::fork();
    if (child < 0) {
        const std::string error = std::strerror(errno);
        ::close(pair[0]); ::close(pair[1]);
        throw std::runtime_error("Cannot fork TUN provider: " + error);
    }
    if (child == 0) {
        ::close(pair[0]);
        try {
            create_and_send_many(pair[1], requests, netns);
            ::close(pair[1]);
            _exit(0);
        } catch (const std::exception& error) {
            const std::string text = "TUN provider: " + std::string(error.what()) + "\n";
            [[maybe_unused]] const ssize_t written =
                ::write(STDERR_FILENO, text.data(), text.size());
            ::close(pair[1]);
            _exit(1);
        }
    }
    ::close(pair[1]);
    const auto wait_child = [child] {
        int status = 0; pid_t result;
        do { result = ::waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
        if (result < 0)
            throw std::runtime_error("Cannot wait for TUN provider: " + std::string(std::strerror(errno)));
        return status;
    };
    std::vector<tun_fd_socket::Received> received;
    try {
        received.reserve(requests.size());
        for (std::size_t index = 0; index < requests.size(); ++index)
            received.push_back(tun_fd_socket::receive(pair[0]));
    } catch (...) {
        for (const auto& item : received) if (item.fd >= 0) ::close(item.fd);
        ::close(pair[0]);
        try { (void)wait_child(); } catch (...) {}
        throw;
    }
    ::close(pair[0]);
    const int status = wait_child();
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        for (const auto& item : received) if (item.fd >= 0) ::close(item.fd);
        throw std::runtime_error("TUN provider child failed");
    }
    return received;
}

inline tun_fd_socket::Received create_in_child(
    const std::string& interface_name, std::size_t mtu,
    const std::string& netns, bool bring_up) {
    auto received = create_many_in_child(
        {{interface_name, mtu, false, bring_up}}, netns);
    return std::move(received.front());
}

} // namespace tuntom::tun_provider
