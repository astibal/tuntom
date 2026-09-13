// Injected only into disposable test children. A marker file contains one fault
// name; removing it restores normal operation without touching real services.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
static bool fault(const char *kind) {
    const char *path = std::getenv("TUNTOM_V2_FAULT_MARKER");
    if (!path) return false;
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char name[64]{}; const auto n = ::read(fd, name, sizeof(name) - 1); ::close(fd);
    return n > 0 && !std::strcmp(name, kind);
}
extern "C" int memfd_create(const char *name, unsigned flags) {
    static auto next = reinterpret_cast<int (*)(const char *, unsigned)>(::dlsym(RTLD_NEXT, "memfd_create"));
    if (fault("memfd")) { errno = EMFILE; return -1; }
    return next(name, flags);
}
extern "C" int fallocate(int fd, int mode, off_t start, off_t size) {
    static auto next = reinterpret_cast<int (*)(int, int, off_t, off_t)>(::dlsym(RTLD_NEXT, "fallocate"));
    if (fault("fallocate")) { errno = ENOSPC; return -1; }
    return next(fd, mode, start, size);
}
extern "C" void *mmap(void *address, size_t size, int protection, int flags, int fd, off_t offset) {
    static auto next = reinterpret_cast<void *(*)(void *, size_t, int, int, int, off_t)>(::dlsym(RTLD_NEXT, "mmap"));
    if (fd >= 0 && (flags & MAP_SHARED) && fault("mmap")) { errno = ENOMEM; return MAP_FAILED; }
    return next(address, size, protection, flags, fd, offset);
}
extern "C" int epoll_create1(int flags) {
    static auto next = reinterpret_cast<int (*)(int)>(::dlsym(RTLD_NEXT, "epoll_create1"));
    if (fault("epoll")) { errno = EMFILE; return -1; }
    return next(flags);
}
extern "C" ssize_t send(int fd, const void *data, size_t size, int flags) {
    static auto next = reinterpret_cast<ssize_t (*)(int, const void *, size_t, int)>(::dlsym(RTLD_NEXT, "send"));
    const auto *p = static_cast<const unsigned char *>(data);
    if (size == 24 && !std::memcmp(p, "TTX\x02\x05", 5) && fault("active")) { errno = EAGAIN; return -1; }
    return next(fd, data, size, flags);
}
