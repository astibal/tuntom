#include "../control_protocol.hpp"
#include "../switch_ruleset.hpp"
#include <cerrno>
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
        throw std::runtime_error("incomplete or invalid control response; for load, check active serial before retrying");
    buffer.resize(static_cast<std::size_t>(size));
    return buffer;
}
}
int main(int argc, char **argv) {
    try {
        int first = 1;
        std::string path = "/run/tuntom/switch.control";
        if (argc > 1 && std::string(argv[1]) != "rules") { path = argv[1]; ++first; }
        const bool stats = argc == first + 2 && std::string(argv[first]) == "show" && std::string(argv[first + 1]) == "stats";
        std::string operation, body;
        if (!stats && argc >= first + 2 && std::string(argv[first]) == "rules") {
            operation = argv[first + 1];
            if ((operation == "check" || operation == "load") && argc == first + 3) {
                if (std::string(argv[first + 2]) == "-") {
                    std::array<char, tuntom::control_chunk_size> bytes{};
                    while (std::cin.read(bytes.data(), bytes.size()) || std::cin.gcount()) {
                        body.append(bytes.data(), static_cast<std::size_t>(std::cin.gcount()));
                        if (body.size() > tuntom::control_max_body) throw std::runtime_error("ruleset exceeds 1 MiB");
                    }
                    if (!std::cin.eof()) throw std::runtime_error("cannot read stdin");
                } else body = tuntom::read_rules_file(argv[first + 2]);
            } else if (operation != "show" || argc != first + 2) operation.clear();
        }
        if (!stats && operation.empty()) {
            std::cerr << "Usage: " << argv[0] << " <control-socket> show stats\n"
                      << "       " << argv[0] << " [control-socket] rules show\n"
                      << "       " << argv[0] << " [control-socket] rules check|load FILE|-\n";
            return 1;
        }
        if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) throw std::runtime_error("invalid control socket path");
        Socket socket{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)};
        if (socket.fd < 0) throw std::runtime_error(std::strerror(errno));
        timeval timeout{10, 0};
        if (::setsockopt(socket.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0 ||
            ::setsockopt(socket.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
            throw std::runtime_error("cannot set control timeout");
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (::connect(socket.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
            throw std::runtime_error("connect(" + path + ") failed: " + std::strerror(errno));
        const std::string command = stats ? "show stats" : "rules " + operation + " " + std::to_string(body.size());
        send_record(socket.fd, command.data(), command.size());
        for (std::size_t offset = 0; offset < body.size(); offset += tuntom::control_chunk_size)
            send_record(socket.fd, body.data() + offset, std::min(tuntom::control_chunk_size, body.size() - offset));
        if (stats) {
            const auto response = receive(socket.fd, 65536);
            if (response.compare(0, 6, "error=") == 0) throw std::runtime_error(response);
            std::cout << response;
        } else {
            auto header = receive(socket.fd, 256);
            if (!header.empty() && header.back() == '\n') header.pop_back();
            const auto space = header.find(' ');
            if (space == std::string::npos || (header.substr(0, space) != "OK" && header.substr(0, space) != "ERROR"))
                throw std::runtime_error("server does not support framed rules responses");
            const bool success = header.substr(0, space) == "OK";
            const auto length = tuntom::control_length(header.substr(space + 1));
            std::string response;
            while (response.size() < length) {
                auto chunk = receive(socket.fd, tuntom::control_chunk_size);
                if (chunk.size() > length - response.size()) throw std::runtime_error("oversized control response");
                response += chunk;
            }
            if (!success) throw std::runtime_error(response);
            std::cout << response;
        }
        std::cout.flush();
        if (!std::cout) throw std::runtime_error("cannot write output");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
