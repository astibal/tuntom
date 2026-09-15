#include "common.hpp"
#include "switch_ruleset.hpp"
#include <csignal>
#include <iostream>
#include <map>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stopping = 0, enabled = 0;
void stop(int) { stopping = 1; }
void enable(int) { enabled = 1; }
struct Peer { int fd; std::string port; };
}

int main(int argc, char** argv) {
    using namespace divert_lab;
    using tuntom::SwitchOpcode;
    int listener = -1;
    std::string path;
    std::vector<Peer> peers;
    try {
        std::string rules_path, trace_path, cookie;
        std::map<std::string, std::uint64_t> ids;
        std::map<std::uint64_t, std::string> names;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (i + 1 == argc) throw std::runtime_error("missing argument");
            const std::string value = argv[++i];
            if (option == "--socket") path = value;
            else if (option == "--rules") rules_path = value;
            else if (option == "--trace") trace_path = value;
            else if (option == "--cookie") cookie = value;
            else if (option == "--port") {
                auto eq = value.find('=');
                if (eq == std::string::npos) throw std::runtime_error("port expects name=stable-id");
                const auto name = value.substr(0, eq);
                std::size_t used = 0;
                const auto id = std::stoull(value.substr(eq + 1), &used);
                if (!id || used != value.size() - eq - 1 || ids.count(name) || names.count(id))
                    throw std::runtime_error("duplicate or invalid stable port ID");
                ids[name] = id; names[id] = name;
            } else throw std::runtime_error("unknown option: " + option);
        }
        for (const auto* name : {"edge", "exit", "divert-in", "divert-out"})
            if (!ids.count(name)) throw std::runtime_error("missing port identity");
        Codec codec(cookie);
        Trace trace(trace_path);
        const auto rules = tuntom::parse_switch_ruleset(tuntom::read_rules_file(rules_path));
        std::map<std::string, tuntom::RulesProgram> programs;
        for (const auto& item : ids) programs.emplace(item.first, tuntom::RulesProgram(*rules, item.first));
        if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) throw std::runtime_error("invalid socket path");
        listener = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (listener < 0 || ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
            ::listen(listener, 16)) throw std::runtime_error("cannot create switch listener");
        std::signal(SIGTERM, stop); std::signal(SIGINT, stop); std::signal(SIGUSR1, enable);
        std::signal(SIGPIPE, SIG_IGN);

        auto deliver = [&](const std::string& target, SwitchOpcode opcode, const Labels& labels,
                           const tuntom::SwitchFrameView& frame, const std::string& event,
                           const std::string& from, const std::string& logical = "") {
            for (const auto& peer : peers) if (peer.fd >= 0 && peer.port == target) {
                auto bytes = tuntom::encode_switch_frame(opcode, labels, frame.payload, frame.payload_size);
                if (::send(peer.fd, bytes.data(), bytes.size(), MSG_NOSIGNAL | MSG_DONTWAIT) != static_cast<ssize_t>(bytes.size()))
                    trace.event("send_drop", from, target, labels);
                else trace.event(event, from, target, labels, frame.payload, frame.payload_size, logical);
                return;
            }
            trace.event("disconnected_drop", from, target, labels);
        };
        auto normal = [&](const std::string& physical, const std::string& logical, const Labels& base,
                          const Labels& body, const tuntom::SwitchFrameView& input) {
            auto bytes = tuntom::encode_switch_frame(SwitchOpcode::switch_packet, base, input.payload, input.payload_size);
            tuntom::SwitchFrameView view;
            if (!tuntom::decode_switch_frame(bytes.data(), bytes.size(), view)) throw std::runtime_error("invalid normal frame");
            const auto& program = programs.at(logical);
            const auto* rule = program.mapping(view);
            if (!rule || rule->type == tuntom::RuleStatement::Type::policy) {
                trace.event("rule_drop", physical, "", base, input.payload, input.payload_size, logical); return;
            }
            std::array<std::uint64_t, tuntom::switch_max_labels> output{};
            std::size_t count = 0;
            if (!rule->stack.apply(view, output, count)) throw std::runtime_error("rule rewrite failed");
            std::string target;
            for (const auto& item : ids) {
                if (tuntom::route_port_matches(rule->output.port, item.first) &&
                    program.allowed(rule, view, item.first, output.data(), count)) {
                    if (!target.empty()) throw std::runtime_error("lab requires single-output rules");
                    target = item.first;
                }
            }
            if (target.empty()) { trace.event("rule_drop", physical, "", base); return; }
            Labels labels(output.begin(), output.begin() + count);
            if (!body.empty()) labels = codec.attach(labels, body);
            const auto opcode = rules->role(target, tuntom::RuleStatement::Type::exit) ? SwitchOpcode::exit_packet : SwitchOpcode::switch_packet;
            deliver(target, opcode, labels, input, "normal", physical, logical);
        };

        trace.event("ready", "switch", "");
        bool announced = false;
        while (!stopping) {
            if (enabled && !announced) { trace.event("enabled", "switch", ""); announced = true; }
            std::vector<pollfd> descriptors{{listener, POLLIN, 0}};
            for (auto& peer : peers) descriptors.push_back({peer.fd, POLLIN, 0});
            if (::poll(descriptors.data(), descriptors.size(), 100) < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("poll failed");
            }
            for (std::size_t i = 0; i + 1 < descriptors.size(); ++i) {
                if (!descriptors[i + 1].revents || peers[i].fd < 0) continue;
                auto& peer = peers[i];
                std::array<std::uint8_t, 66000> buffer{};
                const auto n = ::recv(peer.fd, buffer.data(), buffer.size(), MSG_TRUNC);
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                if (n <= 0 || static_cast<std::size_t>(n) > buffer.size()) {
                    ::close(peer.fd); peer.fd = -1; continue;
                }
                if (peer.port.empty()) {
                    std::string port;
                    if (!tuntom::decode_switch_registration(buffer.data(), n, port) || !ids.count(port)) {
                        ::close(peer.fd); peer.fd = -1; continue;
                    }
                    for (auto& old : peers) if (&old != &peer && old.fd >= 0 && old.port == port) {
                        ::close(old.fd); old.fd = -1;
                    }
                    peer.port = port;
                    trace.event("registered", port, "", {ids.at(port)});
                    continue;
                }
                try {
                    tuntom::SwitchFrameView frame;
                    if (!tuntom::decode_switch_frame(buffer.data(), n, frame) || frame.opcode != SwitchOpcode::switch_packet)
                        throw std::runtime_error("invalid IPC frame");
                    const auto labels = labels_of(frame);
                    auto env = codec.split(labels);
                    if (peer.port == "divert-in" || peer.port == "divert-out") {
                        if (!env.present || !names.count(env.origin()) || names.at(env.origin()) != "edge")
                            throw std::runtime_error("unknown origin");
                        const auto& origin = names.at(env.origin());
                        if (peer.port == "divert-in" && env.action() == bypass)
                            normal(peer.port, origin, env.base, {}, frame);
                        else if (peer.port == "divert-in" && env.action() == to_client)
                            deliver(origin, SwitchOpcode::switch_packet, env.original(), frame, "client_return", peer.port);
                        else if (peer.port == "divert-out" && env.action() == onward)
                            normal(peer.port, origin, env.base, env.body, frame);
                        else throw std::runtime_error("unexpected action for adapter side");
                    } else if (peer.port == "exit" && env.present) {
                        if (env.action() != onward || !names.count(env.origin()) || names.at(env.origin()) != "edge")
                            throw std::runtime_error("invalid service return");
                        deliver("divert-out", SwitchOpcode::exit_packet, labels, frame, "server_return", peer.port);
                    } else if (peer.port == "edge" && enabled) {
                        if (env.present) throw std::runtime_error("duplicate local divert envelope");
                        deliver("divert-in", SwitchOpcode::exit_packet, codec.offer(labels, ids.at(peer.port)), frame, "divert", peer.port);
                    } else {
                        if (env.present) throw std::runtime_error("unexpected service envelope");
                        normal(peer.port, peer.port, labels, {}, frame);
                    }
                } catch (const std::exception& error) { trace.event("invalid_drop", peer.port, error.what()); }
            }
            if (descriptors[0].revents & POLLIN) {
                const auto fd = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (fd >= 0) peers.push_back({fd, ""});
            }
        }
        for (auto& peer : peers) if (peer.fd >= 0) ::close(peer.fd);
        ::close(listener); ::unlink(path.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "divert lab switch: " << error.what() << '\n';
        for (auto& peer : peers) if (peer.fd >= 0) ::close(peer.fd);
        if (listener >= 0) { ::close(listener); ::unlink(path.c_str()); }
        return 1;
    }
}
