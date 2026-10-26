#pragma once

#include "info_codec.hpp"

// Address discovery is an OS service, not part of the portable wire codec.
// Windows transport currently consumes the codec directly; do not silently
// substitute empty addresses for an unimplemented discovery backend.
#if defined(__linux__)
#include "platform/linux/info.hpp"
#else
#error "INFO address discovery is not implemented for this platform; use info_codec.hpp for wire data"
#endif
