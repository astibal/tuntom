#include "../ipc/switch_protocol.hpp"
#include "../control_socket.hpp"
#include "../throughput_stats.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <chrono>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using tuntom::SwitchFrameView;
using tuntom::SwitchOpcode;

volatile std::sig_atomic_t stop_requested = 0;
void request_stop(int) { stop_requested = 1; }

struct RouteKey {
    std::string port;
    std::uint64_t label = 0;
    bool operator==(const RouteKey& other) const {
        return port == other.port and label == other.label;
    }
};

struct RouteKeyHash {
    std::size_t operator()(const RouteKey& key) const {
        const auto label_hash = std::hash<std::uint64_t> {}(key.label);
        const auto port_hash = std::hash<std::string> {}(key.port);
        return label_hash ^ (port_hash + 0x9e3779b9U + (label_hash << 6) +
                             (label_hash >> 2));
    }
};

struct RouteTarget {
    std::string port;
    std::uint64_t label = 0;
};

struct Connection {
    int fd = -1;
    std::string port_id;
};

struct SwitchStats {
    std::uint64_t connections_accepted = 0;
    std::uint64_t registrations_ok = 0;
    std::uint64_t registrations_invalid = 0;
    std::uint64_t frames_rx = 0;
    std::uint64_t bytes_rx = 0;
    std::uint64_t frames_tx = 0;
    std::uint64_t bytes_tx = 0;
    std::uint64_t route_hits = 0;
    std::uint64_t route_misses = 0;
    std::uint64_t target_disconnected = 0;
    std::uint64_t default_back = 0;
    std::uint64_t exit_deliveries = 0;
    std::uint64_t malformed_frames = 0;
    std::uint64_t send_errors = 0;
};

void usage(const char* program) {
    std::cerr
        << "Usage:\n"
        << "  " << program << " --socket <unix-path>\n"
        << "      [--route <in-port>:<label>=<out-port>:<label> ...]\n"
        << "      [--exit-port <port-id> ...]\n"
        << "      [--control-socket <unix-path>]\n"
        << "      [--default-back=off|on]\n\n"
        << "Each client registers a stable port ID after connecting.\n";
}

std::uint64_t parse_label(const std::string& text) {
    if (text.empty() or text[0] == '-')
        throw std::runtime_error("Invalid label: " + text);
    std::size_t used = 0;
    const auto label = std::stoull(text, &used, 0);
    if (used != text.size()) throw std::runtime_error("Invalid label: " + text);
    return static_cast<std::uint64_t>(label);
}

std::pair<std::string, std::string> parse_assignment(
    const std::string& text, const char* what) {
    const auto separator = text.find('=');
    if (separator == std::string::npos or separator == 0 or
        separator + 1 == text.size())
        throw std::runtime_error(std::string("Invalid ") + what + ": " + text);
    return {text.substr(0, separator), text.substr(separator + 1)};
}

std::pair<std::string, std::uint64_t> parse_endpoint(const std::string& text) {
    const auto separator = text.rfind(':');
    if (separator == std::string::npos or separator == 0 or
        separator + 1 == text.size())
        throw std::runtime_error("Invalid route endpoint: " + text);
    return {text.substr(0, separator), parse_label(text.substr(separator + 1))};
}

int create_listener(const std::string& path) {
    if (path.empty() or path.size() >= sizeof(sockaddr_un::sun_path))
        throw std::runtime_error("Invalid Unix socket path");
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        throw std::runtime_error("socket() failed: " + std::string(std::strerror(errno)));
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        const std::string error = std::strerror(errno);
        ::close(fd);
        throw std::runtime_error("bind(" + path + ") failed: " + error);
    }
    if (::chmod(path.c_str(), 0660) < 0 or ::listen(fd, 64) < 0) {
        const std::string error = std::strerror(errno);
        ::close(fd);
        ::unlink(path.c_str());
        throw std::runtime_error("Cannot prepare switch socket: " + error);
    }
    return fd;
}

Connection* find_connection(
    std::vector<Connection>& connections, const std::string& port_id) {
    for (auto& connection : connections) {
        if (connection.fd >= 0 and connection.port_id == port_id) return &connection;
    }
    return nullptr;
}

bool send_frame(int fd, const std::vector<std::uint8_t>& frame, SwitchStats& stats) {
    const ssize_t sent = ::send(
        fd, frame.data(), frame.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent != static_cast<ssize_t>(frame.size())) {
        ++stats.send_errors;
        return false;
    }
    ++stats.frames_tx;
    stats.bytes_tx += frame.size();
    return true;
}

void close_connections(std::vector<Connection>& connections) {
    for (auto& connection : connections) {
        if (connection.fd >= 0) ::close(connection.fd);
        connection.fd = -1;
    }
}

} // namespace

int main(int argc, char** argv) {
    int listener = -1;
    std::string socket_path;
    std::string control_path;
    std::vector<Connection> connections;
    try {
        std::unordered_map<RouteKey, RouteTarget, RouteKeyHash> routes;
        std::unordered_set<std::string> exit_ports;
        SwitchStats stats;
        tuntom::ThroughputStats throughput({"switch_rx", "switch_tx"});
        const auto started_at = std::chrono::steady_clock::now();
        throughput.update(started_at, {
            {stats.frames_rx, stats.bytes_rx},
            {stats.frames_tx, stats.bytes_tx}});
        bool default_back = false;

        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--socket") {
                if (++index >= argc) throw std::runtime_error("--socket requires a value");
                if (not socket_path.empty()) throw std::runtime_error("Duplicate --socket");
                socket_path = argv[index];
            } else if (option == "--control-socket") {
                if (++index >= argc) throw std::runtime_error("--control-socket requires a value");
                control_path = argv[index];
            } else if (option == "--route") {
                if (++index >= argc) throw std::runtime_error("--route requires a value");
                const auto assignment = parse_assignment(argv[index], "route");
                const auto input = parse_endpoint(assignment.first);
                const auto output = parse_endpoint(assignment.second);
                if (not routes.emplace(
                        RouteKey {input.first, input.second},
                        RouteTarget {output.first, output.second}).second)
                    throw std::runtime_error("Duplicate switch route");
            } else if (option == "--exit-port") {
                if (++index >= argc) throw std::runtime_error("--exit-port requires a value");
                const std::string port = argv[index];
                if (port.empty() or port.size() > tuntom::switch_max_port_id_size or
                    not exit_ports.insert(port).second)
                    throw std::runtime_error("Invalid or duplicate exit port: " + port);
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
        if (socket_path.empty()) {
            usage(argv[0]);
            throw std::runtime_error("--socket is required");
        }

        listener = create_listener(socket_path);
        std::unique_ptr<tuntom::ControlSocket> control;
        if (not control_path.empty())
            control = std::make_unique<tuntom::ControlSocket>(control_path);
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
            descriptors.reserve(connections.size() + 2);
            descriptors.push_back({listener, POLLIN, 0});
            descriptors.push_back({control ? control->fd() : -1, POLLIN, 0});
            for (const auto& connection : connections)
                descriptors.push_back({connection.fd, POLLIN, 0});

            const int ready = ::poll(descriptors.data(), descriptors.size(), 1000);
            if (ready < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(
                    "poll() failed: " + std::string(std::strerror(errno)));
            }

            if (descriptors[0].revents & POLLIN) {
                const int accepted = ::accept4(
                    listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (accepted >= 0) {
                    connections.push_back({accepted, {}});
                    ++stats.connections_accepted;
                }
            }

            if (control and (descriptors[1].revents & POLLIN)) {
                control->handle([&] {
                    const auto snapshot_at = std::chrono::steady_clock::now();
                    throughput.update(snapshot_at, {
                        {stats.frames_rx, stats.bytes_rx},
                        {stats.frames_tx, stats.bytes_tx}});
                    std::size_t connected = 0;
                    for (const auto& item : connections)
                        if (item.fd >= 0 and not item.port_id.empty()) ++connected;
                    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                        snapshot_at - started_at).count();
                    std::ostringstream out;
                    out << "format=txt\nformat_version=1\ncomponent=switch\n"
                        << "pid=" << ::getpid() << "\nuptime_seconds=" << uptime << "\n"
                        << "connections_current=" << connected << "\n"
                        << "connections_accepted=" << stats.connections_accepted << "\n"
                        << "registrations_ok=" << stats.registrations_ok << "\n"
                        << "registrations_invalid=" << stats.registrations_invalid << "\n"
                        << "frames_rx=" << stats.frames_rx << "\nbytes_rx=" << stats.bytes_rx << "\n"
                        << "frames_tx=" << stats.frames_tx << "\nbytes_tx=" << stats.bytes_tx << "\n"
                        << "route_hits=" << stats.route_hits << "\nroute_misses=" << stats.route_misses << "\n"
                        << "target_disconnected=" << stats.target_disconnected << "\n"
                        << "default_back=" << stats.default_back << "\n"
                        << "exit_deliveries=" << stats.exit_deliveries << "\n"
                        << "malformed_frames=" << stats.malformed_frames << "\n"
                        << "send_errors=" << stats.send_errors << "\n";
                    throughput.write(out);
                    return out.str();
                });
            }

            const std::size_t polled_connections = descriptors.size() - 2;
            for (std::size_t index = 0; index < polled_connections; ++index) {
                auto& connection = connections[index];
                const auto events = descriptors[index + 2].revents;
                if (connection.fd < 0) continue;
                if (events & (POLLHUP | POLLERR | POLLNVAL)) {
                    ::close(connection.fd);
                    connection.fd = -1;
                    continue;
                }
                if (not (events & POLLIN)) continue;

                const ssize_t received = ::recv(
                    connection.fd, buffer.data(), buffer.size(), MSG_TRUNC);
                if (received <= 0 or static_cast<std::size_t>(received) > buffer.size()) {
                    ::close(connection.fd);
                    connection.fd = -1;
                    continue;
                }
                const std::size_t size = static_cast<std::size_t>(received);

                if (connection.port_id.empty()) {
                    std::string registered_id;
                    if (not tuntom::decode_switch_registration(
                            buffer.data(), size, registered_id)) {
                        ++stats.registrations_invalid;
                        ::close(connection.fd);
                        connection.fd = -1;
                        continue;
                    }
                    if (auto* old = find_connection(connections, registered_id)) {
                        if (old != &connection) {
                            ::close(old->fd);
                            old->fd = -1;
                        }
                    }
                    connection.port_id = registered_id;
                    ++stats.registrations_ok;
                    continue;
                }

                SwitchFrameView frame;
                if (not tuntom::decode_switch_frame(buffer.data(), size, frame) or
                    frame.opcode != SwitchOpcode::switch_packet) {
                    ++stats.malformed_frames;
                    continue;
                }
                ++stats.frames_rx;
                stats.bytes_rx += size;

                std::vector<std::uint8_t> output(buffer.begin(), buffer.begin() + received);
                const auto route = routes.find({connection.port_id, frame.label(0)});
                if (route != routes.end()) {
                    ++stats.route_hits;
                    auto* target = find_connection(connections, route->second.port);
                    if (target == nullptr) {
                        ++stats.target_disconnected;
                        continue;
                    }
                    tuntom::replace_top_switch_label(output, route->second.label);
                    if (exit_ports.count(route->second.port) != 0) {
                        tuntom::set_switch_opcode(output, SwitchOpcode::exit_packet);
                        ++stats.exit_deliveries;
                    }
                    send_frame(target->fd, output, stats);
                } else if (default_back) {
                    ++stats.route_misses;
                    ++stats.default_back;
                    tuntom::set_switch_opcode(output, SwitchOpcode::exit_packet);
                    send_frame(connection.fd, output, stats);
                } else {
                    ++stats.route_misses;
                }
            }

            connections.erase(
                std::remove_if(
                    connections.begin(), connections.end(),
                    [](const Connection& connection) { return connection.fd < 0; }),
                connections.end());
            throughput.update(std::chrono::steady_clock::now(), {
                {stats.frames_rx, stats.bytes_rx},
                {stats.frames_tx, stats.bytes_tx}});
        }

        close_connections(connections);
        ::close(listener);
        listener = -1;
        ::unlink(socket_path.c_str());
        return 0;
    } catch (const std::exception& error) {
        close_connections(connections);
        if (listener >= 0) {
            ::close(listener);
            ::unlink(socket_path.c_str());
        }
        std::cerr << "ERROR: " << error.what() << "\n";
        return 1;
    }
}
