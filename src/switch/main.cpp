#include "../common.hpp"
#include "../runtime_recovery.hpp"
#include "../switch_admission.hpp"
#include "../ipc/switch_protocol.hpp"
#include "../adaptive_polling.hpp"
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
#include <sys/prctl.h>
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
    tuntom::AcceptBackoff::Time registration_deadline {};
};

struct SwitchStats {
    std::uint64_t connections_accepted = 0;
    std::uint64_t registrations_ok = 0;
    std::uint64_t registrations_invalid = 0;
    std::uint64_t registrations_timed_out = 0;
    std::uint64_t registrations_capacity_rejected = 0;
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
    std::uint64_t send_backpressure_drops = 0;
};

void usage(const char* program) {
    std::cerr
        << "Usage:\n"
        << "  " << program << " --socket <unix-path>\n"
        << "      [--route <in-port>:<label>=<out-port>:<label> ...]\n"
        << "      [--exit-port <port-id> ...]\n"
        << "      [--control-socket <unix-path>]\n"
        << "      [--max-ports <1..65535>] [--max-pending <1..65535>]\n"
        << "      [--default-back=off|on]\n\n"
        << "Each client registers a stable port ID within 5 seconds.\n"
        << "Default limits: 256 ports, 16 pending registrations; reduced to fit FD capacity.\n";
}

std::size_t parse_capacity(const std::string& text) {
    if (text.empty() or text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Connection limit must be in range 1..65535");
    const auto value = std::stoull(text);
    if (value == 0 or value > 65535)
        throw std::runtime_error("Connection limit must be in range 1..65535");
    return static_cast<std::size_t>(value);
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
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
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
        if (sent < 0 and (errno == EAGAIN or errno == EWOULDBLOCK))
            ++stats.send_backpressure_drops;
        else
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
    // Deployment installs the executable as "main"; identify it in top/ps.
    // This is cosmetic, so failure must not prevent startup.
    (void)::prctl(PR_SET_NAME, "tuntom-switch", 0UL, 0UL, 0UL);
    tuntom::logger.ignore_sigpipe();
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
        tuntom::SwitchCapacity configured_capacity;

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
            } else if (option == "--max-ports" or option == "--max-pending") {
                if (++index >= argc) throw std::runtime_error(option + " requires a value");
                const auto value = parse_capacity(argv[index]);
                if (option == "--max-ports") configured_capacity.ports = value;
                else configured_capacity.pending = value;
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
        const auto capacity = configured_capacity.for_process();
        connections.reserve(capacity.ports + capacity.pending);
        tuntom::SwitchAdmission admission(std::chrono::steady_clock::now());
        struct sigaction action {};
        action.sa_handler = request_stop;
        ::sigemptyset(&action.sa_mask);
        ::sigaction(SIGINT, &action, nullptr);
        ::sigaction(SIGTERM, &action, nullptr);

        std::vector<std::uint8_t> buffer(
            tuntom::switch_base_header_size +
            tuntom::switch_max_labels * tuntom::switch_label_size +
            std::numeric_limits<std::uint16_t>::max());
        tuntom::AdaptivePolling adaptive_polling;
        std::size_t next_connection = 0;
        std::vector<pollfd> descriptors;
        std::vector<pollfd> backlog_descriptors;
        std::vector<bool> initially_ready;
        descriptors.reserve(capacity.ports + capacity.pending + 2);
        backlog_descriptors.reserve(capacity.ports + capacity.pending);
        initially_ready.reserve(capacity.ports + capacity.pending);

        const auto connection_counts = [&] {
            std::pair<std::size_t, std::size_t> counts {};
            for (const auto& connection : connections) {
                if (connection.fd < 0) continue;
                if (connection.port_id.empty()) ++counts.second;
                else ++counts.first;
            }
            return counts;
        };
        const auto expire_registrations = [&](tuntom::AcceptBackoff::Time now) {
            for (auto& connection : connections) {
                if (connection.fd >= 0 and connection.port_id.empty() and
                    now >= connection.registration_deadline) {
                    ::close(connection.fd);
                    connection.fd = -1;
                    ++stats.registrations_timed_out;
                }
            }
        };

        const auto try_handle_connection = [&](Connection& connection) {
            // A ready record does not extend the registration's fixed lifetime.
            if (connection.port_id.empty() and std::chrono::steady_clock::now() >=
                connection.registration_deadline) {
                ::close(connection.fd);
                connection.fd = -1;
                ++stats.registrations_timed_out;
                return true;
            }
            const ssize_t received = ::recv(
                connection.fd, buffer.data(), buffer.size(), MSG_TRUNC);
            if (received < 0) {
                if (errno == EAGAIN or errno == EWOULDBLOCK or errno == EINTR)
                    return false;
                ::close(connection.fd);
                connection.fd = -1;
                return true;
            }
            if (received == 0 or static_cast<std::size_t>(received) > buffer.size()) {
                ::close(connection.fd);
                connection.fd = -1;
                return true;
            }
            const std::size_t size = static_cast<std::size_t>(received);

            if (connection.port_id.empty()) {
                try {
                    std::string registered_id;
                    if (not tuntom::decode_switch_registration(
                            buffer.data(), size, registered_id)) {
                        ++stats.registrations_invalid;
                        ::close(connection.fd);
                        connection.fd = -1;
                        return true;
                    }
                    auto* old = find_connection(connections, registered_id);
                    if (not old and connection_counts().first >= capacity.ports) {
                        ++stats.registrations_capacity_rejected;
                        ::close(connection.fd);
                        connection.fd = -1;
                        return true;
                    }
                    // All allocating work precedes the replacement. Swapping
                    // std::string with its default allocator cannot throw.
                    connection.port_id.swap(registered_id);
                    if (old and old != &connection) {
                        ::close(old->fd);
                        old->fd = -1;
                    }
                    ++stats.registrations_ok;
                    return true;
                } catch (const std::bad_alloc&) {
                    // The registration record was already consumed. Force a
                    // reconnect rather than leaving a half-registered peer.
                    ::close(connection.fd);
                    connection.fd = -1;
                    throw;
                }
            }

            SwitchFrameView frame;
            if (not tuntom::decode_switch_frame(buffer.data(), size, frame) or
                frame.opcode != SwitchOpcode::switch_packet) {
                ++stats.malformed_frames;
                return true;
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
                    return true;
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
            return true;
        };

        const auto data_backlog_ready = [&] {
            backlog_descriptors.clear();
            backlog_descriptors.reserve(connections.size());
            for (const auto& connection : connections)
                backlog_descriptors.push_back({connection.fd, POLLIN, 0});
            if (backlog_descriptors.empty()) return false;
            const int ready = ::poll(
                backlog_descriptors.data(), backlog_descriptors.size(), 0);
            if (ready <= 0) return false;
            for (const auto& descriptor : backlog_descriptors) {
                if ((descriptor.revents & POLLIN) != 0) return true;
            }
            return false;
        };

        tuntom::RuntimeRecovery recovery;
        const auto handle_control = [&] {
            if (not control) return;
            control->handle([&] {
                const auto snapshot_at = std::chrono::steady_clock::now();
                throughput.update(snapshot_at, {
                    {stats.frames_rx, stats.bytes_rx},
                    {stats.frames_tx, stats.bytes_tx}});
                const auto counts = connection_counts();
                const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                    snapshot_at - started_at).count();
                std::ostringstream out;
                out.exceptions(std::ios::badbit);
                out << "format=txt\nformat_version=1\ncomponent=switch\n"
                    << "pid=" << ::getpid() << "\nuptime_seconds=" << uptime << "\n"
                    << "connections_current=" << counts.first << "\n"
                    << "connections_pending=" << counts.second << "\n"
                    << "connections_total=" << counts.first + counts.second << "\n"
                    << "connections_limit_ports_configured=" << configured_capacity.ports << "\n"
                    << "connections_limit_pending_configured=" << configured_capacity.pending << "\n"
                    << "connections_limit_ports=" << capacity.ports << "\n"
                    << "connections_limit_pending=" << capacity.pending << "\n"
                    << "connections_fd_reserve=" << tuntom::SwitchCapacity::fd_reserve << "\n"
                    << "connections_accepted=" << stats.connections_accepted << "\n"
                    << "registrations_ok=" << stats.registrations_ok << "\n"
                    << "registrations_invalid=" << stats.registrations_invalid << "\n"
                    << "registrations_timed_out=" << stats.registrations_timed_out << "\n"
                    << "registrations_capacity_rejected=" << stats.registrations_capacity_rejected << "\n"
                    << "frames_rx=" << stats.frames_rx << "\nbytes_rx=" << stats.bytes_rx << "\n"
                    << "frames_tx=" << stats.frames_tx << "\nbytes_tx=" << stats.bytes_tx << "\n"
                    << "route_hits=" << stats.route_hits << "\nroute_misses=" << stats.route_misses << "\n"
                    << "target_disconnected=" << stats.target_disconnected << "\n"
                    << "default_back=" << stats.default_back << "\n"
                    << "exit_deliveries=" << stats.exit_deliveries << "\n"
                    << "malformed_frames=" << stats.malformed_frames << "\n"
                    << "send_errors=" << stats.send_errors << "\n"
                    << "send_backpressure_drops=" << stats.send_backpressure_drops << "\n";
                recovery.write_stats(out);
                admission.write_stats(out, snapshot_at);
                control->write_stats(out);
                tuntom::logger.write_stats(out);
                adaptive_polling.write_stats(out);
                throughput.write(out);
                return out.str();
            });
        };

        tuntom::logger.start();
        tuntom::log_info("tuntom-switch ready");

        while (not stop_requested) {
            try {
                // Expire even during PF-02 recovery; this maintenance cannot allocate.
                expire_registrations(std::chrono::steady_clock::now());
                if (recovery.wait_for_retry(control ? control->poll_fd() : -1)) {
                    handle_control();
                    continue;
                }
                // Compact before polling; descriptor indexes stay stable until
                // all events from this poll have been serviced.
                connections.erase(std::remove_if(connections.begin(), connections.end(),
                    [](const Connection& connection) { return connection.fd < 0; }), connections.end());
                if (connections.empty()) next_connection = 0;
                else next_connection %= connections.size();
                const auto now = std::chrono::steady_clock::now();
                const bool pending_space = connection_counts().second < capacity.pending;
                descriptors.clear();
                descriptors.reserve(connections.size() + 2);
                descriptors.push_back({pending_space and admission.ready(now) ? listener : -1, POLLIN, 0});
                descriptors.push_back({control ? control->poll_fd() : -1, POLLIN, 0});
                for (const auto& connection : connections)
                    descriptors.push_back({connection.fd, POLLIN, 0});

                int timeout = pending_space ? admission.poll_timeout_ms(now, 1000) : 1000;
                if (control) timeout = control->poll_timeout_ms(now, timeout);
                for (const auto& connection : connections) {
                    if (connection.port_id.empty()) timeout = tuntom::deadline_timeout_ms(
                        now, connection.registration_deadline, timeout);
                }
                // Adaptive polling measures the syscall, not deadline bookkeeping.
                const auto poll_started = tuntom::AdaptivePolling::Clock::now();
                const int ready = ::poll(descriptors.data(), descriptors.size(), timeout);
                const auto poll_finished = tuntom::AdaptivePolling::Clock::now();
                if (ready < 0) {
                    if (errno != EINTR) recovery.poll_failed(errno);
                    continue;
                }
                adaptive_polling.observe_poll(poll_finished - poll_started);
                expire_registrations(poll_finished);

                if (control and (descriptors[1].revents & POLLIN)) handle_control();
                if (descriptors[0].revents & POLLIN) {
                    admission.attempted(poll_finished);
                    const int accepted = ::accept4(
                        listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                    if (accepted >= 0) {
                        try {
                            connections.push_back({accepted, {}, std::chrono::steady_clock::now() +
                                tuntom::SwitchAdmission::registration_timeout});
                        } catch (...) {
                            ::close(accepted);
                            throw;
                        }
                        ++stats.connections_accepted;
                    } else {
                        admission.failed(errno, std::chrono::steady_clock::now());
                    }
                }

                const std::size_t polled_connections = descriptors.size() - 2;
                initially_ready.assign(polled_connections, false);
                for (std::size_t index = 0; index < polled_connections; ++index) {
                    auto& connection = connections[index];
                    const auto events = descriptors[index + 2].revents;
                    if (connection.fd < 0) continue;
                    if (events & (POLLHUP | POLLERR | POLLNVAL)) {
                        ::close(connection.fd);
                        connection.fd = -1;
                        continue;
                    }
                    initially_ready[index] = (events & POLLIN) != 0;
                }

                const unsigned rounds = adaptive_polling.batch_size();
                const auto slice_started = tuntom::AdaptivePolling::Clock::now();
                bool slice_limited = false;
                for (unsigned round = 0; round < rounds and polled_connections != 0; ++round) {
                    bool progress = false;
                    for (std::size_t offset = 0; offset < polled_connections; ++offset) {
                        const std::size_t index =
                            (next_connection + offset) % polled_connections;
                        if (round == 0 and not initially_ready[index]) continue;
                        if (connections[index].fd < 0) continue;
                        progress |= try_handle_connection(connections[index]);
                        if (tuntom::AdaptivePolling::Clock::now() - slice_started >=
                            tuntom::AdaptivePolling::processing_slice) {
                            next_connection = (index + 1) % polled_connections;
                            adaptive_polling.note_slice_limit();
                            slice_limited = true;
                            break;
                        }
                    }
                    if (slice_limited) break;
                    next_connection = (next_connection + 1) % polled_connections;
                    if (not progress) break;
                }

                if (adaptive_polling.should_check_backlog())
                    adaptive_polling.observe_backlog(
                        data_backlog_ready(), tuntom::AdaptivePolling::Clock::now());

                connections.erase(
                    std::remove_if(
                        connections.begin(), connections.end(),
                        [](const Connection& connection) { return connection.fd < 0; }),
                    connections.end());
                if (connections.empty()) next_connection = 0;
                else next_connection %= connections.size();
                throughput.update(std::chrono::steady_clock::now(), {
                    {stats.frames_rx, stats.bytes_rx},
                    {stats.frames_tx, stats.bytes_tx}});
            } catch (const std::bad_alloc&) {
                recovery.allocation_failed();
            }
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
        tuntom::log_fatal(error.what());
        return 1;
    }
}
