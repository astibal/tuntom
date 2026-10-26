#pragma once

// Keep Win32 macros out of the portable session headers.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h may already have been included by the embedding application.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <bcrypt.h>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace tuntom::platform {

inline bool random_bytes(std::uint8_t* output, std::size_t size) noexcept {
    if (size == 0) {
        return true;
    }
    if (output == nullptr || size > (std::numeric_limits<ULONG>::max)()) {
        return false;
    }
    // System CSPRNG only; link bcrypt. Failure must abort the handshake attempt.
    return BCryptGenRandom(nullptr, output, static_cast<ULONG>(size),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

} // namespace tuntom::platform
