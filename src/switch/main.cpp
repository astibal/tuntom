#include "../routed_control.hpp"
#include "../control_ports.hpp"
#include "../ipc/retry_queue.hpp"
#include "../common.hpp"
#include "../runtime_recovery.hpp"
#include "../switch_admission.hpp"
#include "../ipc/switch_protocol.hpp"
#include "../switch_routes.hpp"
#include "../switch_ruleset.hpp"
#include "../switch_ecmp.hpp"
#include "../divert/switch.hpp"
#include "../via/switch.hpp"
#include "../relay/registry.hpp"
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

struct Connection {
    int fd = -1;
    std::string port;
    tuntom::AcceptBackoff::Time registration_deadline {};
    tuntom::SwitchPortRoutes routes;
    std::uint64_t identity = 0;
    tuntom::RulesProgram program;
    bool exit_role = false;
    tuntom::relay::Registry relay;
    tuntom::ipc::RetryQueue retry;
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
    std::uint64_t ecmp_packets = 0;
    std::uint64_t default_back = 0;
    std::uint64_t exit_deliveries = 0;
    std::uint64_t malformed_frames = 0;
    std::uint64_t send_errors = 0;
    std::uint64_t send_backpressure_drops = 0;
    std::uint64_t policy_drops = 0, rewrite_drops = 0;
    std::uint64_t divert_forwarded = 0, divert_invalid_drops = 0, divert_overflow_drops = 0;
};

void usage(const char* program) {
    std::cerr
        << "Usage:\n"
        << "  " << program << " --socket <unix-path>\n"
        << "      [--route <in-port>:<label>=<out-port>:<label> ...]\n"
        << "      [--exit-port <port-id> ...]\n"
        << "      [--control-socket <unix-path>]\n"
        << tuntom::control_auth::help
        << "      [--rules-file <format-1-or-2-config>]\n"
        << "      [--divert-file <config>]  Opt-in local divert, initially disabled\n"
        << "      [--max-ports <1..65535>] [--max-pending <1..65535>]\n"
        << "      [--default-back=off|on]\n\n"
        << "Route ports accept a trailing *; multiple matching outputs use ECMP.\n"
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
        if (connection.fd >= 0 and connection.port == port_id) return &connection;
    }
    return nullptr;
}

void account_output(const tuntom::ipc::RetryQueue::Outcome& out, SwitchStats& stats) {
    stats.frames_tx += out.frames; stats.bytes_tx += out.bytes;
    stats.send_backpressure_drops += out.backpressure;
    stats.send_errors += out.drops - out.backpressure;
}
bool send_frame(Connection& connection, const std::uint8_t* frame, std::size_t size, SwitchStats& stats) {
    const auto out = connection.retry.submit(frame,size,tuntom::ipc::RetryQueue::Clock::now(),
        [&](const std::uint8_t* data,std::size_t n) { return ::send(connection.fd,data,n,MSG_DONTWAIT|MSG_NOSIGNAL); });
    account_output(out,stats);
    if (out.error) {
        account_output(connection.retry.discard(),stats);
        ::close(connection.fd); connection.fd = -1;
    }
    return !out.drops;
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
        tuntom::SwitchRoutes routes;
        std::shared_ptr<const tuntom::SwitchRuleset> ruleset;
        std::string rules_file;
        std::string divert_file;
        std::shared_ptr<tuntom::divert::Config> divert_config;
        std::unique_ptr<tuntom::divert::SwitchPath> divert_path;
        std::unique_ptr<tuntom::via::State> via_state;
        std::unique_ptr<tuntom::via::SwitchPath> via_path;
        bool via_guard = false;
        std::unordered_set<std::string> exit_ports;
        SwitchStats stats;
        tuntom::ThroughputStats throughput({"switch_rx", "switch_tx"});
        const auto started_at = std::chrono::steady_clock::now();
        throughput.update(started_at, {
            {stats.frames_rx, stats.bytes_rx},
            {stats.frames_tx, stats.bytes_tx}});
        bool default_back = false;
        tuntom::SwitchCapacity configured_capacity;
        tuntom::control_auth::Config control_auth_config;
        tuntom::RoutedControl routed_control("switch");

        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            if(tuntom::control_auth::option(control_auth_config,option,index,argc,argv))continue;
            if (option == "--socket") {
                if (++index >= argc) throw std::runtime_error("--socket requires a value");
                if (not socket_path.empty()) throw std::runtime_error("Duplicate --socket");
                socket_path = argv[index];
            } else if (option == "--control-socket") {
                if (++index >= argc) throw std::runtime_error("--control-socket requires a value");
                control_path = argv[index];
            } else if (option == "--route") {
                if (++index >= argc) throw std::runtime_error("--route requires a value");
                tuntom::add_switch_route(routes, argv[index]);
            } else if (option == "--rules-file") {
                if (++index >= argc || !rules_file.empty()) throw std::runtime_error("--rules-file requires one path");
                rules_file = argv[index];
            } else if (option == "--divert-file") {
                if (++index >= argc || !divert_file.empty()) throw std::runtime_error("--divert-file requires one path");
                divert_file = argv[index];
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
        if (!rules_file.empty()) {
            if (!routes.empty() || !exit_ports.empty() || default_back)
                throw std::runtime_error("--rules-file cannot be combined with legacy routing options");
            ruleset = tuntom::parse_switch_ruleset(tuntom::read_rules_file(rules_file));
        } else if (routes.empty() && exit_ports.empty() && !default_back) {
            ruleset = tuntom::parse_switch_ruleset("format 1\nserial 0\n");
        }

        if (!divert_file.empty()) {
            if (rules_file.empty() || control_path.empty())
                throw std::runtime_error("--divert-file requires --rules-file and --control-socket");
            divert_config = tuntom::divert::read_config(divert_file);
        }
        const auto make_divert_path = [&](std::shared_ptr<const tuntom::SwitchRuleset> selected, const std::string& added = "") {
            if (divert_config && divert_config->via && !tuntom::via::enabled(selected))
                throw std::runtime_error("VIA divert requires rules format 3");
            if (!divert_config || divert_config->via) return std::unique_ptr<tuntom::divert::SwitchPath>{};
            std::vector<std::string> names;
            for (const auto& c : connections)
                if (c.fd >= 0 && !c.port.empty() && c.port != added) names.push_back(c.port);
            if (!added.empty()) names.push_back(added);
            return std::make_unique<tuntom::divert::SwitchPath>(divert_config, std::move(selected), std::move(names));
        };
        const auto make_via_path = [&](std::shared_ptr<const tuntom::SwitchRuleset> selected, const std::string& added = "") {
            if (!tuntom::via::enabled(selected)) return std::unique_ptr<tuntom::via::SwitchPath>{};
            if (!tuntom::via::enabled(ruleset)) {
                for (const auto& c : connections) {
                    if (c.fd < 0 || c.port.empty()) continue;
                    if (!tuntom::via::accepted(*selected, c.port)) throw std::runtime_error("existing port conflicts with VIA registration: " + c.port);
                    for (const auto& other : connections)
                        if (&c != &other && other.fd >= 0 && !other.port.empty() &&
                            !tuntom::via::compatible(*selected, c.port, c.fd, other.port, other.fd))
                            throw std::runtime_error("existing ports conflict with VIA instance ownership");
                }
            }
            if (!via_state) via_state = std::make_unique<tuntom::via::State>();
            std::vector<std::string> names;
            std::map<std::string,std::string> bindings;
            for (const auto& c : connections) if (c.fd >= 0 && !c.port.empty() && c.port != added) {
                names.push_back(c.port);
                if (c.relay.live()) for (const auto& item : c.relay.directory.channels) {
                    if (!tuntom::via::accepted(*selected,item.second.name,c.port)) continue;
                    names.push_back(item.second.name); bindings.emplace(item.second.name,c.port);
                }
            }
            if (!added.empty()) names.push_back(added);
            return std::make_unique<tuntom::via::SwitchPath>(*via_state, std::move(selected), divert_config, std::move(names), bindings);
        };
        divert_path = make_divert_path(ruleset);
        via_path = make_via_path(ruleset);
        via_guard = static_cast<bool>(via_path);
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
            std::numeric_limits<std::uint16_t>::max() + tuntom::relay::header_size);
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
                if (connection.port.empty()) ++counts.second;
                else ++counts.first;
            }
            return counts;
        };
        const auto expire_registrations = [&](tuntom::AcceptBackoff::Time now) {
            for (auto& connection : connections) {
                if (connection.fd >= 0 and connection.port.empty() and
                    now >= connection.registration_deadline) {
                    account_output(connection.retry.discard(),stats); ::close(connection.fd);
                    connection.fd = -1;
                    ++stats.registrations_timed_out;
                }
            }
        };

        const auto refresh_control_edges = [&] {
            std::set<std::string> names;
            for (const auto& c : connections) {
                if (c.fd < 0 || c.port.empty()) continue;
                names.insert(c.port);
                const auto cookie = tuntom::control_socket_generation(c.fd);
                routed_control.router().edge(c.port, cookie, false, tuntom::control_route::max_frame,
                    [&,name=c.port,cookie](const auto& bytes) {
                        auto* target = find_connection(connections,name);
                        return target && tuntom::control_socket_generation(target->fd)==cookie &&
                            send_frame(*target,bytes.data(),bytes.size(),stats);
                    });
            }
            routed_control.router().retain(names);
        };

        const auto try_handle_connection = [&](Connection& connection) {
            // A ready record does not extend the registration's fixed lifetime.
            if (connection.port.empty() and std::chrono::steady_clock::now() >=
                connection.registration_deadline) {
                account_output(connection.retry.discard(),stats); ::close(connection.fd);
                connection.fd = -1;
                ++stats.registrations_timed_out;
                return true;
            }
            const ssize_t received = ::recv(
                connection.fd, buffer.data(), buffer.size(), MSG_TRUNC);
            if (received < 0) {
                if (errno == EAGAIN or errno == EWOULDBLOCK or errno == EINTR)
                    return false;
                account_output(connection.retry.discard(),stats); ::close(connection.fd);
                connection.fd = -1;
                return true;
            }
            if (received == 0 or static_cast<std::size_t>(received) > buffer.size()) {
                account_output(connection.retry.discard(),stats); ::close(connection.fd);
                connection.fd = -1;
                return true;
            }
            std::size_t size = static_cast<std::size_t>(received);

            if (connection.port.empty()) {
                try {
                    std::string registered_id;
                    if (not tuntom::decode_switch_registration(
                            buffer.data(), size, registered_id)) {
                        ++stats.registrations_invalid;
                        account_output(connection.retry.discard(),stats); ::close(connection.fd);
                        connection.fd = -1;
                        return true;
                    }
                    if (tuntom::via::enabled(ruleset)) {
                        bool valid = tuntom::via::accepted(*ruleset, registered_id);
                        for (const auto& c : connections)
                            if (c.fd >= 0 && !c.port.empty() && !tuntom::via::compatible(*ruleset, registered_id, connection.fd, c.port, c.fd)) valid = false;
                        if (!valid) {
                            ++stats.registrations_invalid; account_output(connection.retry.discard(),stats); ::close(connection.fd); connection.fd = -1; return true;
                        }
                    }
                    auto* old = find_connection(connections, registered_id);
                    if (not old and connection_counts().first >= capacity.ports) {
                        ++stats.registrations_capacity_rejected;
                        account_output(connection.retry.discard(),stats); ::close(connection.fd);
                        connection.fd = -1;
                        return true;
                    }
                    // All allocating work precedes the replacement. Swapping
                    // std::string with its default allocator cannot throw.
                    auto resolved = tuntom::routes_for_port(routes, registered_id);
                    auto program = ruleset ? tuntom::RulesProgram(*ruleset, registered_id) : tuntom::RulesProgram();
                    auto next_divert = make_divert_path(ruleset, registered_id);
                    auto next_via = make_via_path(ruleset, registered_id);
                    connection.exit_role = ruleset && ruleset->role(registered_id, tuntom::RuleStatement::Type::exit);
                    connection.identity = tuntom::ecmp_port_identity(registered_id);
                    connection.program = std::move(program);
                    connection.routes.swap(resolved);
                    connection.port.swap(registered_id);
                    divert_path.swap(next_divert);
                    via_path.swap(next_via);
                    if (old and old != &connection) {
                        account_output(old->retry.discard(),stats); ::close(old->fd);
                        old->fd = -1;
                    }
                    ++stats.registrations_ok;
                    return true;
                } catch (const std::bad_alloc&) {
                    // The registration record was already consumed. Force a
                    // reconnect rather than leaving a half-registered peer.
                    account_output(connection.retry.discard(),stats); ::close(connection.fd);
                    connection.fd = -1;
                    throw;
                }
            }

            if (tuntom::control_route::marked(buffer.data(),size)) {
                refresh_control_edges();
                routed_control.router().receive(connection.port,buffer.data(),size,tuntom::ControlRouter::Clock::now());
                return true;
            }
            const std::string* ingress = &connection.port;
            if (tuntom::relay::marked(buffer.data(),size)) {
                tuntom::relay::View record;
                if (!ruleset || !tuntom::relay::decode(buffer.data(),size,record) ||
                    !tuntom::relay::configured(*ruleset,connection.port)) { ++stats.malformed_frames; return true; }
                if (record.type == tuntom::relay::Type::snapshot) {
                    std::vector<std::string> occupied;
                    for (const auto& c : connections) if (&c != &connection && c.fd >= 0) {
                        occupied.push_back(c.port);
                        if (c.relay.live()) for (const auto& item : c.relay.directory.channels) occupied.push_back(item.second.name);
                    }
                    auto candidate = connection.relay; bool changed = false;
                    if (!tuntom::relay::update(candidate,*ruleset,connection.port,record,occupied,changed)) {
                        ++stats.registrations_invalid; return true;
                    }
                    auto previous = std::move(connection.relay); connection.relay = std::move(candidate);
                    try { if (changed) via_path = make_via_path(ruleset); }
                    catch (...) { connection.relay = std::move(previous); throw; }
                    if (changed) account_output(connection.retry.discard(),stats);
                    const auto ack = tuntom::relay::encode(tuntom::relay::Type::acknowledged,record.channel,record.epoch);
                    (void)send_frame(connection,ack.data(),ack.size(),stats); return true;
                }
                const auto channel = connection.relay.directory.channels.find(record.channel);
                if (record.type != tuntom::relay::Type::data || !connection.relay.live() ||
                    connection.relay.directory.epoch != record.epoch || channel == connection.relay.directory.channels.end() ||
                    !tuntom::via::accepted(*ruleset,channel->second.name,connection.port)) { ++stats.divert_invalid_drops; return true; }
                ingress = &channel->second.name;
                size = record.size; std::memmove(buffer.data(),record.payload,size);
            } else if (ruleset && tuntom::relay::configured(*ruleset,connection.port)) {
                ++stats.divert_invalid_drops; return true;
            }
            SwitchFrameView frame;
            if (not tuntom::decode_switch_frame(buffer.data(), size, frame) or
                frame.opcode != SwitchOpcode::switch_packet || frame.payload_size > tuntom::max_ip_packet_size) {
                ++stats.malformed_frames;
                return true;
            }
            ++stats.frames_rx;
            stats.bytes_rx += size;

            if (via_guard && !via_path) {
                tuntom::via::Envelope retired;
                if (tuntom::via::reserved(connection.port) ||
                    !tuntom::via::Codec::split(tuntom::divert::labels_of(frame), retired) || retired.present) {
                    ++stats.divert_invalid_drops; return true;
                }
            }
            if (divert_path || via_path) {
                using tuntom::divert::Result;
                const auto locate = [&](const std::string& name, std::uint32_t& channel) -> Connection* {
                    if (auto* c = find_connection(connections,name)) return c;
                    for (auto& c : connections) if (c.fd >= 0 && c.relay.live())
                        for (const auto& item : c.relay.directory.channels) if (item.second.name == name) { channel = item.first; return &c; }
                    return nullptr;
                };
                const auto live = [&](const std::string& name) { std::uint32_t channel = 0; return locate(name,channel) != nullptr; };
                const auto decision = via_path ? via_path->route(*ingress, frame, live) : divert_path->route(connection.port, frame, live);
                if (decision.result != Result::normal) {
                    if (decision.result == Result::forward) {
                        auto output_size = size;
                        tuntom::rewrite_rules_frame(buffer.data(), output_size, frame,
                            decision.labels.values, decision.labels.size, decision.exit);
                        std::uint32_t channel = 0;
                        auto* target = locate(*decision.target,channel);
                        if (!target) { ++stats.target_disconnected; return true; }
                        if (channel && !tuntom::relay::wrap(buffer.data(),output_size,buffer.size(),channel,target->relay.directory.epoch)) {
                            ++stats.rewrite_drops; return true;
                        }
                        if (decision.exit) ++stats.exit_deliveries;
                        if (decision.multipath) ++stats.ecmp_packets;
                        if (send_frame(*target, buffer.data(), output_size, stats)) ++stats.divert_forwarded;
                    } else if (decision.result == Result::overflow) ++stats.divert_overflow_drops;
                    else if (decision.result == Result::malformed) ++stats.divert_invalid_drops;
                    else if (decision.result == Result::policy) ++stats.policy_drops;
                    else ++stats.target_disconnected;
                    return true;
                }
            }

            if (ingress != &connection.port) { ++stats.divert_invalid_drops; return true; }
            if (ruleset) {
                const auto *mapping = connection.program.mapping(frame);
                if (!mapping) { ++stats.route_misses; return true; }
                if (mapping->type == tuntom::RuleStatement::Type::policy) { ++stats.policy_drops; return true; }
                ++stats.route_hits;
                std::array<std::uint64_t, tuntom::switch_max_labels> labels{};
                std::size_t count = 0;
                if (!mapping->stack.apply(frame, labels, count) ||
                    tuntom::switch_base_header_size + count * tuntom::switch_label_size + frame.payload_size > buffer.size()) {
                    ++stats.rewrite_drops; return true;
                }
                Connection *target = nullptr;
                bool connected = false;
                tuntom::EcmpSelector selector(frame);
                for (auto &candidate : connections) {
                    if (candidate.fd < 0 || candidate.port.empty() ||
                        !tuntom::route_port_matches(mapping->output.port, candidate.port)) continue;
                    connected = true;
                    if (!connection.program.allowed(mapping, frame, candidate.port, labels.data(), count)) continue;
                    if (selector.consider(candidate.identity, candidate.port)) target = &candidate;
                }
                if (!target) {
                    if (connected) ++stats.policy_drops; else ++stats.target_disconnected;
                    return true;
                }
                if (selector.multipath()) ++stats.ecmp_packets;
                const bool exit = target->exit_role;
                auto output_size = size;
                tuntom::rewrite_rules_frame(buffer.data(), output_size, frame, labels, count, exit);
                if (exit) ++stats.exit_deliveries;
                send_frame(*target, buffer.data(), output_size, stats);
                return true;
            }

            const auto route = connection.routes.find(frame.label(0));
            if (route != connection.routes.end()) {
                ++stats.route_hits;
                Connection* target = nullptr;
                if (!tuntom::wildcard_port(route->second.port)) {
                    target = find_connection(connections, route->second.port);
                } else {
                    tuntom::EcmpSelector selector(frame);
                    for (auto& candidate : connections) {
                        if (candidate.fd < 0 || candidate.port.empty() ||
                            !tuntom::route_port_matches(route->second.port, candidate.port)) continue;
                        if (selector.consider(candidate.identity, candidate.port)) target = &candidate;
                    }
                    if (selector.multipath()) ++stats.ecmp_packets;
                }
                if (target == nullptr) {
                    ++stats.target_disconnected;
                    return true;
                }
                // Only the validated header changes; reuse the receive buffer.
                tuntom::store_be64(buffer.data() + tuntom::switch_base_header_size,
                                  route->second.label);
                if (exit_ports.count(target->port) != 0) {
                    buffer[1] = static_cast<std::uint8_t>(SwitchOpcode::exit_packet);
                    ++stats.exit_deliveries;
                }
                send_frame(*target, buffer.data(), size, stats);
            } else if (default_back) {
                ++stats.route_misses;
                ++stats.default_back;
                buffer[1] = static_cast<std::uint8_t>(SwitchOpcode::exit_packet);
                send_frame(connection, buffer.data(), size, stats);
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
        tuntom::ControlDispatcher control_dispatcher;
        control_dispatcher.stats = [&] {
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
                    << "ecmp_packets=" << stats.ecmp_packets << "\n"
                    << "default_back=" << stats.default_back << "\n"
                    << "exit_deliveries=" << stats.exit_deliveries << "\n"
                    << "malformed_frames=" << stats.malformed_frames << "\n"
                    << "send_errors=" << stats.send_errors << "\n"
                    << "send_backpressure_drops=" << stats.send_backpressure_drops << "\n";
                for (const auto& c : connections) if (!c.port.empty()) c.retry.stats(out,"port_" + c.port + "_retry_");
                out << "policy_drops=" << stats.policy_drops << "\nrewrite_drops=" << stats.rewrite_drops << '\n';
                if (ruleset) out << "ruleset_format=" << ruleset->format << "\nruleset_serial=" << ruleset->serial << '\n';
                if (divert_config) out << divert_config->command("divert.show");
                if (via_path) out << "via_enabled=1\n";
                if (divert_config || via_path) out << "divert_forwarded=" << stats.divert_forwarded
                    << "\ndivert_invalid_drops=" << stats.divert_invalid_drops
                    << "\ndivert_overflow_drops=" << stats.divert_overflow_drops << '\n';
                recovery.write_stats(out);
                admission.write_stats(out, snapshot_at);
                if (control) control->write_stats(out);
                tuntom::logger.write_stats(out);
                adaptive_polling.write_stats(out);
                throughput.write(out);
                return out.str();
            };
        control_dispatcher.rules = [&](const std::string &operation, const std::string &body) {
                if (operation.compare(0, 7, "divert.") == 0) {
                    if (!divert_config) throw std::runtime_error("divert is not configured");
                    if (operation == "divert.enable" && !divert_config->via &&
                        (!find_connection(connections, divert_config->input) || !find_connection(connections, divert_config->output)))
                        throw std::runtime_error("both divert adapter ports must be connected");
                    return divert_config->command(operation);
                }
                if (operation == "show") {
                    if (!ruleset) throw std::runtime_error("legacy CLI routes are active; load a versioned ruleset to enable export");
                    return ruleset->text();
                }
                auto next = tuntom::parse_switch_ruleset(body);
                const bool changed = tuntom::ruleset_changed(ruleset, *next);
                auto next_divert = make_divert_path(next);
                auto next_via = make_via_path(next);
                std::vector<tuntom::RulesProgram> programs;
                std::vector<bool> exits;
                programs.reserve(connections.size());
                exits.reserve(connections.size());
                for (const auto &connection : connections) {
                    programs.emplace_back(*next, connection.port);
                    exits.push_back(next->role(connection.port, tuntom::RuleStatement::Type::exit));
                }
                auto response = std::string(operation == "check" ? "checked" : changed ? "applied" : "unchanged") +
                    " serial=" + std::to_string(next->serial) + "\n";
                if (operation == "load" && changed) {
                    for (std::size_t i = 0; i < connections.size(); ++i) {
                        connections[i].program = std::move(programs[i]);
                        connections[i].exit_role = exits[i];
                    }
                    for (auto& c : connections) account_output(c.retry.discard(),stats);
                    ruleset.swap(next);
                    divert_path.swap(next_divert);
                    via_path.swap(next_via);
                    via_guard = via_guard || static_cast<bool>(via_path);
                    routes.clear(); exit_ports.clear(); default_back = false;
                }
                return response;
            };
        control_dispatcher.flows = [] { return tuntom::FlowDump("none").finish(); };
        control_dispatcher.ports = [&] {
                tuntom::ControlPorts ports;
                for (const auto& connection : connections) {
                    if (connection.fd < 0 || connection.port.empty()) continue;
                    ports.direct(connection.port);
                    if (connection.relay.live())
                        for (const auto& channel : connection.relay.directory.channels)
                            ports.relayed(channel.second.name, connection.port);
                }
                return ports.finish();
            };
        tuntom::control_auth::validate(control_auth_config);
        routed_control.configure_auth(control_auth_config);
        routed_control.configure(control_auth_config.allow_all,control_dispatcher);
        if (control) control->set_routed([&](tuntom::ControlRequest request,unsigned retries,unsigned wait) {
            refresh_control_edges();
            return routed_control.local_submit()(std::move(request),retries,wait);
        });
        const auto handle_control = [&] {
            if (control) control->handle_dispatch(control_dispatcher);
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
                if (routed_control.active()) refresh_control_edges();
                routed_control.tick();
                if (routed_control.active()) handle_control();
                const auto now = std::chrono::steady_clock::now();
                const bool pending_space = connection_counts().second < capacity.pending;
                descriptors.clear();
                descriptors.reserve(connections.size() + 2);
                descriptors.push_back({pending_space and admission.ready(now) ? listener : -1, POLLIN, 0});
                descriptors.push_back({control ? control->poll_fd() : -1, POLLIN, 0});
                for (const auto& connection : connections)
                    descriptors.push_back({connection.fd, static_cast<short>(POLLIN | (connection.retry.writable(now) ? POLLOUT : 0)), 0});

                int timeout = pending_space ? admission.poll_timeout_ms(now, 1000) : 1000;
                if (control) timeout = control->poll_timeout_ms(now, timeout);
                if (routed_control.active()) timeout = std::min(timeout,10);
                for (const auto& connection : connections) {
                    timeout = connection.retry.timeout(now, timeout);
                    if (connection.port.empty()) timeout = tuntom::deadline_timeout_ms(
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
                                tuntom::SwitchAdmission::registration_timeout, {}, 0, {}, false, {}, {}});
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
                        account_output(connection.retry.discard(),stats); ::close(connection.fd);
                        connection.fd = -1;
                        continue;
                    }
                    const auto output = connection.retry.flush(poll_finished,[&](const std::uint8_t* data,std::size_t n) {
                        return ::send(connection.fd,data,n,MSG_DONTWAIT|MSG_NOSIGNAL);
                    });
                    account_output(output,stats);
                    if (output.error) {
                        account_output(connection.retry.discard(),stats);
                        ::close(connection.fd); connection.fd = -1; continue;
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
