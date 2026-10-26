#pragma once

// One platform boundary for session entropy. Both backends provide
// platform::random_bytes(output, size): true only after filling the entire
// buffer, false on any error, no weak fallback and no unbounded retry.
// Empty buffers succeed; null pointers for nonempty buffers are rejected.
#if defined(_WIN32)
#include "windows/random.hpp"
#elif defined(__linux__)
#include "linux/random.hpp"
#else
#error "Tuntom requires a secure random backend for this platform"
#endif
