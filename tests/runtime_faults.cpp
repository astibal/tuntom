// Loaded only into disposable test processes. The marker enables faults after
// startup; removing it simulates resource recovery without restarting a process.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <new>
#include <poll.h>
#include <sys/random.h>
#include <unistd.h>

static bool enabled(const char* kind) {
    const char* mode = std::getenv("TUNTOM_TEST_FAULT");
    const char* marker = std::getenv("TUNTOM_TEST_FAULT_MARKER");
    return mode and marker and std::strcmp(mode, kind) == 0 and ::access(marker, F_OK) == 0;
}

extern "C" int poll(pollfd* fds, nfds_t count, int timeout) {
    static auto original = reinterpret_cast<int (*)(pollfd*, nfds_t, int)>(::dlsym(RTLD_NEXT, "poll"));
    static bool fired = false;
    if (count > 1 and (enabled("poll_always") or (not fired and enabled("poll_once")))) {
        fired = true;
        errno = ENOMEM;
        return -1;
    }
    return original(fds, count, timeout);
}

extern "C" ssize_t getrandom(void* output, size_t size, unsigned flags) {
    static auto original = reinterpret_cast<ssize_t (*)(void*, size_t, unsigned)>(::dlsym(RTLD_NEXT, "getrandom"));
    if (enabled("random_always")) { errno = EIO; return -1; }
    return original(output, size, flags);
}

void* operator new(std::size_t size) {
    static bool fired = false;
    if (size == 1200 and (enabled("alloc_always") or (not fired and enabled("alloc_once")))) {
        fired = true;
        throw std::bad_alloc();
    }
    void* memory = std::malloc(size ? size : 1);
    if (not memory) throw std::bad_alloc();
    return memory;
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
