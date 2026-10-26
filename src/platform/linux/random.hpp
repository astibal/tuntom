#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/random.h>

namespace tuntom::platform {

inline bool random_bytes(std::uint8_t* output, std::size_t size) noexcept {
    if (size != 0 && output == nullptr) {
        return false;
    }
    while (size != 0) {
        const auto count = ::getrandom(output, size, GRND_NONBLOCK);
        // Do not spin on EINTR/EAGAIN in the packet loop. Session retry policy
        // schedules a later attempt; partially filled output is not usable.
        if (count <= 0) {
            return false;
        }
        output += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}

} // namespace tuntom::platform
