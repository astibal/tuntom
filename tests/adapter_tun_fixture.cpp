// Test-only linker wrappers: run the real adapter loop without root or a TUN.
// The parent supplies a SOCK_SEQPACKET pair endpoint as TUNTOM_TEST_TUN_FD.
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern "C" int __real_open(const char*, int, ...);
extern "C" int __real_ioctl(int, unsigned long, ...);

extern "C" int __wrap_open(const char* path, int flags, ...) {
    if (std::strcmp(path, "/dev/net/tun") == 0) {
        const char* inherited = std::getenv("TUNTOM_TEST_TUN_FD");
        if (not inherited) std::abort();
        const int fd = ::fcntl(std::atoi(inherited), F_DUPFD_CLOEXEC, 0);
        if (fd >= 0) ::fcntl(fd, F_SETFL, O_NONBLOCK);
        return fd;
    }
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        const auto mode = va_arg(args, unsigned int);
        va_end(args);
        return __real_open(path, flags, mode);
    }
    return __real_open(path, flags);
}

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    void* value = va_arg(args, void*);
    va_end(args);
    if (request == TUNSETIFF or request == SIOCSIFMTU or
        request == SIOCGIFFLAGS or request == SIOCSIFFLAGS) return 0;
    return __real_ioctl(fd, request, value);
}
