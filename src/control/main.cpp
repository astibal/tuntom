#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char** argv) {
    try {
        if (argc != 4 or std::string(argv[2]) != "show" or
            std::string(argv[3]) != "stats") {
            std::cerr << "Usage: " << argv[0] << " <control-socket> show stats\n";
            return 1;
        }
        const std::string path = argv[1];
        if (path.empty() or path.size() >= sizeof(sockaddr_un::sun_path))
            throw std::runtime_error("Invalid control socket path");
        const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd < 0) throw std::runtime_error(std::strerror(errno));
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            const std::string error = std::strerror(errno);
            ::close(fd);
            throw std::runtime_error("connect(" + path + ") failed: " + error);
        }
        const std::string command = "show stats";
        if (::send(fd, command.data(), command.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(command.size())) {
            ::close(fd);
            throw std::runtime_error("Cannot send control command");
        }
        std::vector<char> response(65536);
        const ssize_t size = ::recv(fd, response.data(), response.size(), MSG_TRUNC);
        ::close(fd);
        if (size < 0 or static_cast<std::size_t>(size) > response.size())
            throw std::runtime_error("Invalid control response");
        std::cout.write(response.data(), size);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << "\n";
        return 1;
    }
}
