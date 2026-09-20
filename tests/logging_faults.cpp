// Test-only failures in the logger thread. Never linked into a daemon.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                                void* (*entry)(void*), void* context) {
    static auto original = reinterpret_cast<decltype(&pthread_create)>(::dlsym(RTLD_NEXT, "pthread_create"));
    const char* mode = std::getenv("TUNTOM_TEST_LOG_FAULT");
    if (mode and std::strcmp(mode, "create") == 0) return EAGAIN;
    return original(thread, attr, entry, context);
}

extern "C" ssize_t write(int fd, const void* data, size_t size) {
    static auto original = reinterpret_cast<decltype(&write)>(::dlsym(RTLD_NEXT, "write"));
    const char* marker = std::getenv("TUNTOM_TEST_LOG_STALL");
    if (fd == STDERR_FILENO and marker) {
        const char* entered = std::getenv("TUNTOM_TEST_LOG_ENTERED");
        const int signal = ::open(entered, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        if (signal >= 0) ::close(signal);
        while (::access(marker, F_OK) == 0) ::usleep(10000);
    }
    return original(fd, data, size);
}
