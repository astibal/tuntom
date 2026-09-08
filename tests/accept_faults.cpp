// Faults are enabled only by marker files in disposable regression processes.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <new>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static bool enabled(const char* variable) {
    const char* marker = std::getenv(variable);
    return marker and ::access(marker, F_OK) == 0;
}

extern "C" int accept4(int fd, sockaddr* address, socklen_t* length, int flags) {
    static auto original = reinterpret_cast<int (*)(int, sockaddr*, socklen_t*, int)>(::dlsym(RTLD_NEXT, "accept4"));
    static bool fired = false;
    if (enabled("TUNTOM_TEST_ACCEPT_MARKER")) {
        sockaddr_un local {};
        socklen_t size = sizeof(local);
        const char* target = std::getenv("TUNTOM_TEST_ACCEPT_TARGET");
        if (target and ::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &size) == 0 and
            std::strcmp(local.sun_path, target) == 0 and
            (not std::getenv("TUNTOM_TEST_ACCEPT_ONCE") or not fired)) {
            fired = true;
            errno = std::atoi(std::getenv("TUNTOM_TEST_ACCEPT_ERRNO"));
            return -1;
        }
    }
    return original(fd, address, length, flags);
}

extern "C" int poll(pollfd* fds, nfds_t count, int timeout) {
    static auto original = reinterpret_cast<int (*)(pollfd*, nfds_t, int)>(::dlsym(RTLD_NEXT, "poll"));
    if (count > 1 and enabled("TUNTOM_TEST_POLL_MARKER")) { errno = ENOMEM; return -1; }
    return original(fds, count, timeout);
}

void* operator new(std::size_t size) {
    // A maximum-length registration allocates a 63-character string + NUL.
    if (size == 64 and enabled("TUNTOM_TEST_ALLOC_MARKER")) throw std::bad_alloc();
    void* value = std::malloc(size ? size : 1);
    if (not value) throw std::bad_alloc();
    return value;
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
