#include "switch_protocol.hpp"
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using tuntom::SwitchFrameView;
using tuntom::SwitchOpcode;

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int) {
    stop_requested = 1;
}

struct RouteKey {
    std::size_t port = 0;
    std::uint64_t label = 0;

    bool operator==(const RouteKey& other) const {
        return port == other.port and label == other.label;
    }
};

struct RouteKeyHash {
    std::size_t operator()(const RouteKey& key) const {
        const auto label_hash = std::hash<std::uint64_t> {}(key.label);
        const auto port_hash = std::hash<std::size_t> {}(key.port);
        return label_hash ^ (port_hash + 0x9e3779b9U + (label_hash << 6) +
                             (label_hash >> 2));
    }
};

struct RouteTarget {
    std::size_t port = 0;
    std::uint64_t label = 0;
};

struct Port {
    std::string name;
    std::string path;
    int listener = -1;
    int connection = -1;
};

struct RouteSpec {
    std::string input_port;
    std::uint64_t input_label = 0;
    std::string output_port;
    std::uint64_t output_label = 0;
};

void usage(const char* program) {
    std::cerr
        << "Usage:\n"
        << "  " << program << " --port <name>=<unix-path> [--port ...]\n"
        << "      [--route <in>:<label>=<out>:<label> ...]\n"
        << "      [--default-back=off|on]\n\n"
        << "Data frames use the tuntom switch protocol over Unix SOCK_SEQPACKET.\n";
}

std::uint64_t parse_label(const std::string& text) {
    if (text.empty() or text[0] == '-') {
        throw std::runtime_error("Invalid label: " + text);
    }
    std::size_t used = 0;
    const auto label = std::stoull(text, &used, 0);
    if (used != text.size()) throw std::runtime_error("Invalid label: " + text);
    return static_cast<std::uint64_t>(label);
}

std::pair<std::string, std::string> parse_assignment(
    const std::string& text,
    const char* what) {

    const auto separator = text.find('=');
    if (separator == std::string::npos or separator == 0 or
        separator + 1 == text.size()) {
        throw std::runtime_error(std::string("Invalid ") + what + ": " + text);
    }
    return {text.substr(0, separator), text.substr(separator + 1)};
}

std::pair<std::string, std::uint64_t> parse_endpoint(const std::string& text) {
    const auto separator = text.rfind(':');
    if (separator == std::string::npos or separator == 0 or
        separator + 1 == text.size()) {
        throw std::runtime_error("Invalid route endpoint: " + text);
    }
    return {text.substr(0, separator), parse_label(text.substr(separator + 1))};
}

RouteSpec parse_route(const std::string& text) {
    const auto assignment = parse_assignment(text, "route");
    const auto input = parse_endpoint(assignment.first);
    const auto output = parse_endpoint(assignment.second);
    return {input.first, input.second, output.first, output.second};
}

int create_listener(const std::string& path) {
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        throw std::runtime_error("Unix socket path is too long: " + path);
    }

    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw std::runtime_error("socket() failed: " + std::string(std::strerror(errno)));
    }

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        const std::string error = std::strerror(errno);
        ::close(fd);
        throw std::runtime_error("bind(" + path + ") failed: " + error);
    }
    if (::chmod(path.c_str(), 0660) < 0) {
        const std::string error = std::strerror(errno);
        ::close(fd);
        ::unlink(path.c_str());
        throw std::runtime_error("chmod(" + path + ") failed: " + error);
    }
    if (::listen(fd, 4) < 0) {
        const std::string error = std::strerror(errno);
        ::close(fd);
        ::unlink(path.c_str());
        throw std::runtime_error("listen(" + path + ") failed: " + error);
    }
    return fd;
}

std::size_t find_port(
    const std::vector<Port>& ports,
    const std::string& name) {

    for (std::size_t index = 0; index < ports.size(); ++index) {
        if (ports[index].name == name) return index;
    }
    throw std::runtime_error("Unknown switch port: " + name);
}

bool send_frame(int fd, const std::vector<std::uint8_t>& frame) {
    const ssize_t sent = ::send(fd, frame.data(), frame.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(frame.size());
}

void close_ports(std::vector<Port>& ports) {
    for (auto& port : ports) {
        if (port.connection >= 0) {
            ::close(port.connection);
            port.connection = -1;
        }
        if (port.listener >= 0) {
            ::close(port.listener);
            port.listener = -1;
            ::unlink(port.path.c_str());
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::vector<Port> ports;
    try {
        std::vector<RouteSpec> route_specs;
        bool default_back = false;

        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--port") {
                if (++index >= argc) throw std::runtime_error("--port requires a value");
                const auto assignment = parse_assignment(argv[index], "port");
                for (const auto& port : ports) {
                    if (port.name == assignment.first or port.path == assignment.second)
                        throw std::runtime_error("Duplicate switch port name or path");
                }
                ports.push_back({assignment.first, assignment.second, -1, -1});
            } else if (option == "--route") {
                if (++index >= argc) throw std::runtime_error("--route requires a value");
                route_specs.push_back(parse_route(argv[index]));
            } else if (option == "--default-back=on") {
                default_back = true;
            } else if (option == "--default-back=off") {
                default_back = false;
            } else if (option == "--help" or option == "-h") {
                usage(argv[0]);
                return 0;
            } else {
                throw std::runtime_error("Unknown option: " + option);
            }
        }

        if (ports.empty()) {
            usage(argv[0]);
            throw std::runtime_error("At least one --port is required");
        }

        std::unordered_map<RouteKey, RouteTarget, RouteKeyHash> routes;
        for (const auto& spec : route_specs) {
            const RouteKey key {find_port(ports, spec.input_port), spec.input_label};
            const RouteTarget target {find_port(ports, spec.output_port), spec.output_label};
            if (not routes.emplace(key, target).second)
                throw std::runtime_error("Duplicate switch route");
        }

        for (auto& port : ports) port.listener = create_listener(port.path);

        struct sigaction action {};
        action.sa_handler = request_stop;
        ::sigemptyset(&action.sa_mask);
        ::sigaction(SIGINT, &action, nullptr);
        ::sigaction(SIGTERM, &action, nullptr);

        std::vector<std::uint8_t> buffer(
            tuntom::switch_base_header_size +
            tuntom::switch_max_labels * tuntom::switch_label_size +
            std::numeric_limits<std::uint16_t>::max());

        while (not stop_requested) {
            std::vector<pollfd> descriptors;
            descriptors.reserve(ports.size() * 2);
            for (const auto& port : ports) {
                descriptors.push_back({port.listener, POLLIN, 0});
                descriptors.push_back({port.connection, POLLIN, 0});
            }

            const int ready = ::poll(descriptors.data(), descriptors.size(), 1000);
            if (ready < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("poll() failed: " +
                                         std::string(std::strerror(errno)));
            }

            for (std::size_t index = 0; index < ports.size(); ++index) {
                auto& port = ports[index];
                const auto& listener_event = descriptors[index * 2];
                const auto& connection_event = descriptors[index * 2 + 1];
                bool accepted_new_connection = false;

                if (listener_event.revents & POLLIN) {
                    const int accepted = ::accept4(
                        port.listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                    if (accepted >= 0) {
                        if (port.connection >= 0) ::close(port.connection);
                        port.connection = accepted;
                        accepted_new_connection = true;
                    }
                }

                if (port.connection < 0) continue;
                // connection_event belongs to the descriptor snapshot taken
                // before accept(), not to the newly accepted descriptor.
                if (accepted_new_connection) continue;
                if (connection_event.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    ::close(port.connection);
                    port.connection = -1;
                    continue;
                }
                if (not (connection_event.revents & POLLIN)) continue;

                const ssize_t received = ::recv(
                    port.connection, buffer.data(), buffer.size(), MSG_TRUNC);
                if (received <= 0 or static_cast<std::size_t>(received) > buffer.size()) {
                    ::close(port.connection);
                    port.connection = -1;
                    continue;
                }

                const std::size_t frame_size = static_cast<std::size_t>(received);
                SwitchFrameView frame;
                if (not tuntom::decode_switch_frame(buffer.data(), frame_size, frame) or
                    frame.opcode != SwitchOpcode::switch_packet) {
                    continue;
                }

                std::vector<std::uint8_t> output(buffer.begin(), buffer.begin() + received);
                const auto route = routes.find({index, frame.label(0)});
                if (route != routes.end()) {
                    const auto target = route->second;
                    if (ports[target.port].connection < 0) continue;
                    tuntom::replace_top_switch_label(output, target.label);
                    send_frame(ports[target.port].connection, output);
                } else if (default_back) {
                    tuntom::set_switch_opcode(output, SwitchOpcode::exit_packet);
                    send_frame(port.connection, output);
                }
            }
        }

        close_ports(ports);
        return 0;
    } catch (const std::exception& error) {
        close_ports(ports);
        std::cerr << "ERROR: " << error.what() << "\n";
        return 1;
    }
}
