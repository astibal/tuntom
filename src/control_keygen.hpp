#pragma once

// Provisioning needs OS-specific private-file permissions. Do not substitute
// a generic file stream with inherited permissions for secret key creation.
#if defined(__linux__)
#include "platform/linux/control_keygen.hpp"
#else
#error "Control key provisioning is not implemented on this platform"
#endif
