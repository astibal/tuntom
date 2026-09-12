// LD_PRELOAD fixture for disposable test children only. Never used by a target.
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <new>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
thread_local unsigned allocation_countdown = 0;
bool main_thread() { return ::syscall(SYS_gettid) == ::getpid(); }

unsigned allocation_ordinal() {
    const auto* path = std::getenv("TOMTOM_TEST_ALLOC_MARKER");
    if (!path) return 0;
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char input[32] {};
    const auto count = ::read(fd, input, sizeof(input) - 1);
    ::close(fd);
    return count > 0 ? static_cast<unsigned>(std::strtoul(input, nullptr, 10)) : 0;
}

void maybe_fail_allocation() {
    if (allocation_countdown) {
        const auto* marker = std::getenv("TOMTOM_TEST_ALLOC_MARKER");
        if (!marker || ::access(marker, F_OK) != 0) allocation_countdown = 0;
    }
    if (allocation_countdown && --allocation_countdown == 0) throw std::bad_alloc();
}

bool fail_call(const char* operation) {
    const auto* mode = std::getenv("TOMTOM_TEST_SYSCALL");
    const auto* marker = std::getenv("TOMTOM_TEST_SYSCALL_MARKER");
    if (!mode || !marker || std::strcmp(mode, operation) != 0 || ::access(marker, F_OK) != 0)
        return false;
    // Poll failures persist until the marker is removed. EINTR/send EAGAIN are
    // finite bursts; the fixture itself must not turn them into an endless loop.
    static std::atomic<unsigned> calls {0};
    return std::strcmp(operation, "poll") == 0 || std::strcmp(operation, "ppoll") == 0 ||
        calls.fetch_add(1, std::memory_order_relaxed) < 100;
}
} // namespace

void* operator new(std::size_t size) {
    maybe_fail_allocation();
    if (auto* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    maybe_fail_allocation();
    void* memory = nullptr;
    if (::posix_memalign(&memory, static_cast<std::size_t>(alignment), size ? size : 1) == 0)
        return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }

extern "C" ssize_t recv(int fd, void* buffer, size_t capacity, int flags) {
    static auto original = reinterpret_cast<ssize_t(*)(int, void*, size_t, int)>(::dlsym(RTLD_NEXT, "recv"));
    if (!main_thread() && fail_call("recv")) { errno = EINTR; return -1; }
    const auto result = original(fd, buffer, capacity, flags);
    // Arm only after main has consumed a registration for the fault-test port.
    // All worker allocations remain unaffected; the packet path should need none.
    if (main_thread() && result >= 13 && capacity >= 13 &&
        std::memcmp(buffer, "TTP\x01", 4) == 0 &&
        std::memcmp(static_cast<char*>(buffer) + 8, "fault", 5) == 0)
        allocation_countdown = allocation_ordinal();
    return result;
}

extern "C" ssize_t send(int fd, const void* buffer, size_t size, int flags) {
    static auto original = reinterpret_cast<ssize_t(*)(int, const void*, size_t, int)>(::dlsym(RTLD_NEXT, "send"));
    if (!main_thread() && fail_call("send")) { errno = EAGAIN; return -1; }
    return original(fd, buffer, size, flags);
}

extern "C" int poll(pollfd* fds, nfds_t count, int timeout) {
    static auto original = reinterpret_cast<int(*)(pollfd*, nfds_t, int)>(::dlsym(RTLD_NEXT, "poll"));
    if (main_thread() && count > 1 && fail_call("poll")) { errno = ENOMEM; return -1; }
    return original(fds, count, timeout);
}

extern "C" int ppoll(pollfd* fds, nfds_t count, const timespec* timeout, const sigset_t* mask) {
    static auto original = reinterpret_cast<int(*)(pollfd*, nfds_t, const timespec*, const sigset_t*)>(::dlsym(RTLD_NEXT, "ppoll"));
    if (fail_call("ppoll")) { errno = ENOMEM; return -1; }
    return original(fds, count, timeout, mask);
}
