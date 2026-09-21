#pragma once
#include "control_route.hpp"
#include "control_protocol.hpp"
#include "control_ports.hpp"
#include "switch_ruleset.hpp"
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
struct Socket { int fd; ~Socket() { if (fd >= 0) ::close(fd); } };
void send_record(int fd, const char *data, std::size_t size) {
    ssize_t sent;
    do { sent = ::send(fd, data, size, MSG_NOSIGNAL); } while (sent < 0 && errno == EINTR);
    if (sent != static_cast<ssize_t>(size)) throw std::runtime_error("cannot send control request");
}
std::string receive(int fd, std::size_t maximum) {
    std::string buffer(maximum, '\0');
    ssize_t size;
    do { size = ::recv(fd, buffer.data(), buffer.size(), MSG_TRUNC); } while (size < 0 && errno == EINTR);
    if (size <= 0 || static_cast<std::size_t>(size) > maximum)
        throw std::runtime_error("incomplete or invalid control response; for load, check active serial or classifier generation before retrying");
    buffer.resize(static_cast<std::size_t>(size));
    return buffer;
}
std::string discover_switch_socket() {
    namespace fs = std::filesystem;
    const char* configured = std::getenv("TUNTOM_RUN_DIR");
    const fs::path directory = configured && *configured ? configured : "/run/tuntom";
    std::vector<std::string> candidates;
    std::error_code error;
    fs::directory_iterator entries(directory, error);
    if (error && error != std::errc::no_such_file_or_directory)
        throw std::runtime_error("cannot inspect " + directory.string() + ": " + error.message());
    for (const auto& entry : entries) {
        if (entry.path().filename().string().find("switch") == std::string::npos) continue;
        const auto status = entry.status(error);
        if (error == std::errc::no_such_file_or_directory) { error.clear(); continue; }
        if (error) throw std::runtime_error("cannot inspect " + entry.path().string() + ": " + error.message());
        if (fs::is_socket(status)) candidates.push_back(entry.path().string());
    }
    std::sort(candidates.begin(), candidates.end());
    if (candidates.size() == 1) return candidates.front();
    std::string message = candidates.empty() ? "no switch socket found in " : "multiple switch sockets found in ";
    message += directory.string() + "; specify --socket PATH";
    for (const auto& candidate : candidates) message += "\n  " + candidate;
    throw std::runtime_error(message);
}
int connect_control(const std::string& path, bool remote) {
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) throw std::runtime_error("invalid control socket path");
    Socket socket{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)};
    if (socket.fd < 0) throw std::runtime_error(std::strerror(errno));
    timeval timeout{remote ? 7200 : 10, 0};
    if (::setsockopt(socket.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0 ||
        ::setsockopt(socket.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
        throw std::runtime_error("cannot set control timeout");
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(socket.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
        throw std::runtime_error("connect(" + path + ") failed: " + std::strerror(errno));
    const int result = socket.fd; socket.fd = -1; return result;
}

}
inline int tuntom_control_main(int argc, char **argv) {
    try {
        int first = 1;
        std::string path = "/run/tuntom/switch.control";
        const bool remote = argc > 1 && std::string(argv[1]) == "remote";
        const bool switch_mode = argc > 1 && std::string(argv[1]) == "switch";
        if (remote || switch_mode) ++first;
        bool explicit_socket = false;
        bool port_list = false, port_tree = false;
        bool separator = false, peer = false, remote_options = false;
        std::string target_port, peer_port;
        unsigned remote_retries = 5, remote_wait = 250;
        while (first < argc) {
            const std::string arg = argv[first];
            if (switch_mode && (arg == "--port-list" || arg == "--port-tree")) {
                if (port_list) throw std::runtime_error("choose exactly one of --port-list and --port-tree");
                port_list = true; port_tree = arg == "--port-tree"; ++first; continue;
            }
            if ((remote || switch_mode) && arg == "--peer") { peer = true; ++first; continue; }
            if ((remote || switch_mode) && (arg == "--port" || arg == "--peer-port")) {
                if (++first >= argc) throw std::runtime_error(arg + " requires a value");
                if (arg == "--port") target_port = argv[first++];
                else { peer_port = argv[first++]; peer = true; }
                continue;
            }
            if ((remote || switch_mode) && arg == "---") { separator = true; ++first; break; }
            if (arg == "--socket" || arg == "--remote-retries" || arg == "--remote-wait") {
                if (++first >= argc) throw std::runtime_error(arg + " requires a value");
                std::string value = argv[first++];
                if (arg != "--socket") remote_options = true;
                if (arg == "--socket") { path = value; explicit_socket = true; }
                else if (arg == "--remote-retries") remote_retries = static_cast<unsigned>(tuntom::control_length(value, 100));
                else {
                    unsigned scale = 1000;
                    if (value.size() > 2 && value.substr(value.size() - 2) == "ms") { value.resize(value.size() - 2); scale = 1; }
                    else if (!value.empty() && value.back() == 's') value.pop_back();
                    remote_wait = static_cast<unsigned>(tuntom::control_length(value, 60000 / scale)) * scale;
                    if (remote_wait < 10) throw std::runtime_error("remote wait must be at least 10 ms");
                }
            } else if (!remote && (arg == "show" || arg == "rules" || arg == "classifier" || arg == "divert" || arg == "request")) break;
            else if (arg.empty() || arg[0] == '-') throw std::runtime_error("unknown local option: " + arg);
            else { path = arg; explicit_socket = true; ++first; }
        }
        if (remote && !separator) throw std::runtime_error("remote requires --- before the command");
        if (port_list && (remote_options || first != argc || peer || !target_port.empty())) throw std::runtime_error("port listing accepts only local socket options");
        if (switch_mode && !port_list && !separator) throw std::runtime_error("switch commands require ---");
        if (remote && !target_port.empty()) throw std::runtime_error("--port requires switch mode");
        const bool discovery = (remote || switch_mode) && argc == first + 1 && std::string(argv[first]) == "discover";
        const bool routed = discovery || peer || !target_port.empty();
        const bool asynchronous = remote || routed;
        tuntom::control_route::Path route;
        if (!target_port.empty()) route.push_back(tuntom::control_route::Hop::port(target_port));
        if (remote || peer) route.push_back(tuntom::control_route::Hop::peer());
        if (!peer_port.empty()) route.push_back(tuntom::control_route::Hop::port(peer_port));
        const bool stats = argc == first + 2 && std::string(argv[first]) == "show" && std::string(argv[first + 1]) == "stats";
        const bool flows = argc == first + 2 && std::string(argv[first]) == "show" && std::string(argv[first + 1]) == "flows";
        const bool status = asynchronous && argc == first + 3 && std::string(argv[first]) == "request" && std::string(argv[first + 1]) == "status";
        std::string operation, body;
        const bool classifier = argc >= first + 2 && std::string(argv[first]) == "classifier";
        const bool divert = argc == first + 2 && std::string(argv[first]) == "divert";
        if (divert && (std::string(argv[first + 1]) == "enable" || std::string(argv[first + 1]) == "stop" ||
                       std::string(argv[first + 1]) == "show"))
            operation = argv[first + 1];
        if (!stats && argc >= first + 2 && (std::string(argv[first]) == "rules" || classifier)) {
            operation = argv[first + 1];
            if ((operation == "check" || operation == "load" || (classifier && operation == "load-flush")) && argc == first + 3) {
                if (std::string(argv[first + 2]) == "-") {
                    std::array<char, tuntom::control_chunk_size> bytes{};
                    while (std::cin.read(bytes.data(), bytes.size()) || std::cin.gcount()) {
                        body.append(bytes.data(), static_cast<std::size_t>(std::cin.gcount()));
                        if (body.size() > tuntom::control_max_body) throw std::runtime_error("ruleset exceeds 1 MiB");
                    }
                    if (!std::cin.eof()) throw std::runtime_error("cannot read stdin");
                } else body = tuntom::read_rules_file(argv[first + 2]);
            } else if ((operation != "show" && !(classifier && operation == "disable")) || argc != first + 2) operation.clear();
        }
        if (!discovery && !port_list && !stats && !flows && !status && operation.empty()) {
            std::cerr << "Switch: tuntomctl switch [--socket PATH | PATH] --port-list|--port-tree\n";
            std::cerr << "Routed: tuntomctl switch [SOCKET] [--port NAME] [--peer | --peer-port NAME] --- COMMAND|discover\n";
            std::cerr << "Remote: tuntomctl remote [--socket PATH] [--remote-retries N] [--remote-wait N[ms|s]] --- COMMAND\n";
            std::cerr << "Usage: " << argv[0] << " <control-socket> show stats|flows\n"
                      << "       " << argv[0] << " [control-socket] rules show\n"
                      << "       " << argv[0] << " [control-socket] rules check|load FILE|-\n";
            std::cerr << "       " << argv[0] << " <control-socket> divert enable|stop|show\n";
            std::cerr << "       " << argv[0] << " <control-socket> classifier check|load|load-flush FILE|-\n"
                      << "       " << argv[0] << " <control-socket> classifier show|disable\n";
            return 1;
        }
        if (switch_mode && !explicit_socket) path = discover_switch_socket();
        if (switch_mode) {
            Socket probe{connect_control(path, false)};
            constexpr char stats_command[] = "show stats";
            send_record(probe.fd, stats_command, sizeof(stats_command) - 1);
            const auto snapshot = "\n" + receive(probe.fd, 65536);
            if (snapshot.find("\ncomponent=switch\n") == std::string::npos)
                throw std::runtime_error("control socket is not a switch: " + path);
        }
        Socket socket{connect_control(path, asynchronous)};
        std::string command = discovery ? "discover" : port_list ? "show ports" : status ? "request status " + std::string(argv[first + 2]) : stats ? "show stats" : flows ? "show flows" : std::string(classifier ? "classifier " : divert ? "divert " : "rules ") + operation + " " + std::to_string(body.size());
        if (routed) command = "routed " + std::to_string(remote_retries) + " " + std::to_string(remote_wait) + " " + tuntom::control_route::path_text(route) + " " + command;
        else if (remote) command = "remote " + std::to_string(remote_retries) + " " + std::to_string(remote_wait) + " " + command;
        send_record(socket.fd, command.data(), command.size());
        for (std::size_t offset = 0; offset < body.size(); offset += tuntom::control_chunk_size)
            send_record(socket.fd, body.data() + offset, std::min(tuntom::control_chunk_size, body.size() - offset));
        if (asynchronous) {
            const auto accepted = receive(socket.fd, 256);
            if (accepted.compare(0, 8, "REQUEST ") != 0) throw std::runtime_error("remote request was not accepted locally: " + accepted);
            std::cerr << "request_id=" << accepted.substr(8);
        }
        if (stats && !asynchronous) {
            const auto response = receive(socket.fd, 65536);
            if (response.compare(0, 6, "error=") == 0) throw std::runtime_error(response);
            std::cout << response;
        } else {
            auto header = receive(socket.fd, 256);
            if (!header.empty() && header.back() == '\n') header.pop_back();
            const auto space = header.find(' ');
            if (space == std::string::npos || (header.substr(0, space) != "OK" && header.substr(0, space) != "ERROR" && header.substr(0, space) != "REJECTED"))
                throw std::runtime_error("server does not support framed control responses");
            const bool success = header.substr(0, space) == "OK";
            const auto length = tuntom::control_length(header.substr(space + 1), flows ? tuntom::control_max_flows : tuntom::control_max_body);
            std::string response;
            while (response.size() < length) {
                auto chunk = receive(socket.fd, tuntom::control_chunk_size);
                if (chunk.size() > length - response.size()) throw std::runtime_error("oversized control response");
                response += chunk;
            }
            if (!success) {
                std::cerr << "ERROR: " << response;
                return asynchronous && header.substr(0, space) == "REJECTED" ? 255 : 1;
            }
            std::cout << (port_tree ? tuntom::ControlPorts::tree(response) : response);
        }
        std::cout.flush();
        if (!std::cout) throw std::runtime_error("cannot write output");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
