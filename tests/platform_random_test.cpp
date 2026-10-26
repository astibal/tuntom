#include "../src/platform/random.hpp"
#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
enum class Mode { real, short_read, interrupted, unavailable, zero };
Mode mode = Mode::real;
unsigned calls = 0;

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
}

extern "C" ssize_t __real_getrandom(void*, size_t, unsigned);
extern "C" ssize_t __wrap_getrandom(void* output, size_t size, unsigned flags) {
    ++calls;
    if (flags != GRND_NONBLOCK) {
        errno = EINVAL;
        return -1;
    }
    if (mode == Mode::real) {
        return __real_getrandom(output, size, flags);
    }
    if (mode == Mode::short_read) {
        const auto count = size > 3 ? 3 : size;
        std::memset(output, 0x5a, count);
        return static_cast<ssize_t>(count);
    }
    if (mode == Mode::zero) {
        return 0;
    }
    errno = mode == Mode::interrupted ? EINTR : EAGAIN;
    return -1;
}

int main() {
    try {
        using tuntom::platform::random_bytes;
        std::array<std::uint8_t, 32> bytes{};
        require(random_bytes(nullptr, 0), "Empty request failed");
        require(!random_bytes(nullptr, 1), "Null output accepted");
        require(calls == 0, "Invalid/empty request reached OS");
        require(random_bytes(bytes.data(), bytes.size()), "System RNG failed");
        mode = Mode::short_read;
        calls = 0;
        bytes.fill(0);
        require(random_bytes(bytes.data(), bytes.size()), "Short reads failed");
        require(calls == 11, "Short reads did not advance correctly");
        for (const auto value : bytes) {
            require(value == 0x5a, "Incomplete random buffer");
        }
        for (auto failure : {Mode::interrupted, Mode::unavailable, Mode::zero}) {
            mode = failure;
            calls = 0;
            require(!random_bytes(bytes.data(), bytes.size()), "RNG failure accepted");
            require(calls == 1, "RNG failure retried inside packet loop");
        }
        std::cout << "PASS: platform entropy contract and Linux failure paths\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
