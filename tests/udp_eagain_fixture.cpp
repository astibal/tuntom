// Force one local DATA/IPC EAGAIN per process; all other sends reach real UDP.
// Running the real tunnel verifies POLLOUT-driven delivery without peer retries.
#include <cerrno>
#include <cstdint>
#include <sys/socket.h>
extern "C" ssize_t __real_sendto(int, const void*, size_t, int, const sockaddr*, socklen_t);
extern "C" ssize_t __wrap_sendto(int fd, const void* data, size_t size, int flags,
                                 const sockaddr* destination, socklen_t length) {
    static bool blocked = false;
    if (size && !blocked) {
        const auto type = static_cast<const std::uint8_t*>(data)[0] & 0x3f;
        if (type == 3 || type == 12) {
            blocked = true;
            errno = EAGAIN;
            return -1;
        }
    }
    return __real_sendto(fd, data, size, flags, destination, length);
}
