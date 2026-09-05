#pragma once
#include "secret.hpp"
#include "vendor/x25519.hpp"

namespace tuntom::x25519 {
using Bytes = std::array<std::uint8_t, 32>;
inline void public_key(Bytes& output, const Bytes& secret) {
    monocypher::crypto_x25519_public_key(output.data(), secret.data());
}
// RFC 7748 raw result; callers must feed it into AKDF, never AEAD directly.
inline bool shared(Bytes& output, const Bytes& secret, const Bytes& peer) {
    monocypher::crypto_x25519(output.data(), secret.data(), peer.data());
    const Bytes zero {};
    return monocypher::crypto_verify32(output.data(), zero.data()) != 0;
}
} // namespace tuntom::x25519
