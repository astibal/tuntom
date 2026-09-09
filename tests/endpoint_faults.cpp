// Runtime endpoint faults in disposable local test processes only.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static const char* mode() {
    const char* marker = std::getenv("TUNTOM_TEST_FAULT_MARKER");
    const char* value = std::getenv("TUNTOM_TEST_FAULT");
    return marker and value and ::access(marker, F_OK) == 0 ? value : "";
}
static bool udp(int fd) {
    int type = 0;
    socklen_t length = sizeof(type);
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) < 0 or type != SOCK_DGRAM) return false;
    sockaddr_storage address {};
    length = sizeof(address);
    return ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0 and
        (address.ss_family == AF_INET or address.ss_family == AF_INET6);
}
static bool tun(int fd) {
    const char* inherited = std::getenv("TUNTOM_TEST_TUN_FD");
    if (not inherited or fd < 0) return false;
    struct stat a {}, b {};
    return ::fstat(fd, &a) == 0 and ::fstat(std::atoi(inherited), &b) == 0 and
        a.st_ino == b.st_ino and a.st_dev == b.st_dev;
}
static bool listener(int fd, bool control_target) {
    int listening = 0;
    socklen_t length = sizeof(listening);
    if (::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &length) < 0 or not listening) return false;
    sockaddr_un address {};
    length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) < 0 or
        address.sun_family != AF_UNIX) return false;
    const char* name = std::strrchr(address.sun_path, '/');
    name = name ? name + 1 : address.sun_path;
    return control_target ? (std::strcmp(name, "control") == 0 or std::strstr(name, ".ctl"))
                          : std::strcmp(name, "switch.sock") == 0;
}
extern "C" int poll(pollfd* fds, nfds_t count, int timeout) {
    static auto original = reinterpret_cast<int (*)(pollfd*, nfds_t, int)>(::dlsym(RTLD_NEXT, "poll"));
    const char* fault = mode();
    const bool target_udp = std::strncmp(fault, "udp_", 4) == 0;
    const bool target_tun = std::strncmp(fault, "tun_", 4) == 0 and std::strcmp(fault, "tun_down") != 0;
    const bool target_control = std::strncmp(fault, "control_", 8) == 0;
    const bool target_listener = std::strncmp(fault, "listener_", 9) == 0;
    for (nfds_t i = 0; i < count; ++i) {
        if (fds[i].fd < 0) continue;
        if (not ((target_udp and udp(fds[i].fd)) or (target_tun and tun(fds[i].fd)) or
                 (target_control and listener(fds[i].fd, true)) or
                 (target_listener and listener(fds[i].fd, false)))) continue;
        const int rc = original(fds, count, 0);
        if (rc < 0) return rc;
        short event = POLLERR;
        if (std::strstr(fault, "hup") or std::strstr(fault, "socket") or std::strstr(fault, "bind")) event = POLLHUP;
        if (std::strstr(fault, "nval")) event = POLLNVAL;
        if (std::strstr(fault, "read")) event = POLLIN;
        if (std::strstr(fault, "send")) return original(fds, count, timeout);
        const bool was_ready = fds[i].revents != 0;
        fds[i].revents = event;
        return rc + (was_ready ? 0 : 1);
    }
    return original(fds, count, timeout);
}
extern "C" int getsockopt(int fd, int level, int option, void* value, socklen_t* length) {
    static auto original = reinterpret_cast<int (*)(int, int, int, void*, socklen_t*)>(::dlsym(RTLD_NEXT, "getsockopt"));
    if (option == SO_ERROR and std::strcmp(mode(), "udp_refused") == 0) {
        *static_cast<int*>(value) = ECONNREFUSED;
        return 0;
    }
    return original(fd, level, option, value, length);
}
extern "C" int socket(int family, int type, int protocol) {
    static auto original = reinterpret_cast<int (*)(int, int, int)>(::dlsym(RTLD_NEXT, "socket"));
    if ((type & 0xf) == SOCK_DGRAM and std::strcmp(mode(), "udp_socket") == 0) {
        errno = EMFILE; return -1;
    }
    return original(family, type, protocol);
}
extern "C" int bind(int fd, const sockaddr* address, socklen_t length) {
    static auto original = reinterpret_cast<int (*)(int, const sockaddr*, socklen_t)>(::dlsym(RTLD_NEXT, "bind"));
    if (udp(fd) and std::strcmp(mode(), "udp_bind") == 0) { errno = EADDRINUSE; return -1; }
    return original(fd, address, length);
}
extern "C" ssize_t recvfrom(int fd, void* data, size_t size, int flags, sockaddr* address, socklen_t* length) {
    static auto original = reinterpret_cast<ssize_t (*)(int, void*, size_t, int, sockaddr*, socklen_t*)>(::dlsym(RTLD_NEXT, "recvfrom"));
    if (std::strcmp(mode(), "udp_read") == 0) { errno = EIO; return -1; }
    return original(fd, data, size, flags, address, length);
}
extern "C" ssize_t sendto(int fd, const void* data, size_t size, int flags, const sockaddr* address, socklen_t length) {
    static auto original = reinterpret_cast<ssize_t (*)(int, const void*, size_t, int, const sockaddr*, socklen_t)>(::dlsym(RTLD_NEXT, "sendto"));
    if (std::strcmp(mode(), "udp_send") == 0) { errno = EIO; return -1; }
    return original(fd, data, size, flags, address, length);
}
extern "C" ssize_t read(int fd, void* data, size_t size) {
    static auto original = reinterpret_cast<ssize_t (*)(int, void*, size_t)>(::dlsym(RTLD_NEXT, "read"));
    if (tun(fd) and std::strcmp(mode(), "tun_read") == 0) { errno = ENODEV; return -1; }
    return original(fd, data, size);
}
extern "C" ssize_t write(int fd, const void* data, size_t size) {
    static auto original = reinterpret_cast<ssize_t (*)(int, const void*, size_t)>(::dlsym(RTLD_NEXT, "write"));
    // Linux TUN returns EIO on write while IFF_UP is clear; the fd stays valid.
    if (tun(fd) and std::strcmp(mode(), "tun_down") == 0) { errno = EIO; return -1; }
    return original(fd, data, size);
}
