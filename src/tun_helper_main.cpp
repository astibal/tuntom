#include "common.hpp"
#include "tun_device.hpp"
#include "tun_fd_socket.hpp"
#include "tun_provider.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) ::close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    int get() const { return fd_; }
private:
    int fd_;
};

class SocketPath {
public:
    explicit SocketPath(std::string path) : path_(std::move(path)) {}
    ~SocketPath() { if (bound_) ::unlink(path_.c_str()); }
    void bound() { bound_ = true; }
private:
    std::string path_;
    bool bound_ = false;
};

std::size_t parse_mtu(const char* text) {
    std::size_t used = 0;
    const auto value = std::stoul(text, &used, 10);
    if (text[used] != '\0' || value < 576 || value > tuntom::max_ip_packet_size)
        throw std::runtime_error("--mtu must be in range 576..65535");
    return value;
}

void usage(const char* program) {
    std::cerr
        << "Usage: " << program << " <socket-path> <ifname> [options]\n"
        << "Options:\n"
        << "  --mtu <n>          TUN MTU advertised to tuntom (default 1500)\n"
        << "  --up               Bring the interface up before passing its fd\n"
        << "  --netns <target>   Create it in a named, path, or pid:<PID> network namespace\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
            usage(argv[0]);
            return 0;
        }
        if (argc < 3) { usage(argv[0]); return 1; }
        const std::string socket_path = argv[1];
        const std::string interface_name = argv[2];
        std::size_t mtu = tuntom::default_tun_mtu;
        bool bring_up = false;
        std::string netns;
        for (int index = 3; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--help" || option == "-h") { usage(argv[0]); return 0; }
            if (option == "--up") { bring_up = true; continue; }
            if (++index >= argc) throw std::runtime_error(option + " requires a value");
            if (option == "--mtu") mtu = parse_mtu(argv[index]);
            else if (option == "--netns") netns = argv[index];
            else throw std::runtime_error("Unknown option: " + option);
        }

        SocketPath cleanup(socket_path);
        Fd listener(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
        if (listener.get() < 0)
            throw std::runtime_error("Cannot create Unix socket: " + std::string(std::strerror(errno)));
        const auto endpoint = tuntom::tun_fd_socket::address(socket_path);
        if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) < 0)
            throw std::runtime_error("Cannot bind " + socket_path + ": " + std::strerror(errno));
        cleanup.bound();
        if (::chmod(socket_path.c_str(), 0600) < 0)
            throw std::runtime_error("Cannot chmod " + socket_path + ": " + std::strerror(errno));
        if (::listen(listener.get(), 1) < 0)
            throw std::runtime_error("Cannot listen on " + socket_path + ": " + std::strerror(errno));

        // Create the pathname listener first: it remains reachable from the
        // caller's namespace while only TUN creation moves into the target.
        std::cout << "TUN provider ready; waiting on " << socket_path << '\n';
        std::cout.flush();
        Fd client(::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC));
        if (client.get() < 0)
            throw std::runtime_error("accept failed: " + std::string(std::strerror(errno)));
        tuntom::tun_provider::create_and_send(client.get(), interface_name, mtu, netns, bring_up);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FATAL: " << error.what() << '\n';
        return 1;
    }
}
