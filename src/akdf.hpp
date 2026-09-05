#pragma once
#include "ascon.hpp"
#include "secret.hpp"
#include <vector>

namespace tuntom::akdf {
// Project-specific AKDF-v1, NOT HKDF or a standard Ascon KDF.
// AMAC is assumed to extract the DH entropy under a subsequently exposed PSK;
// this assumption has not been proven. All outputs have at most 128-bit strength.
// Fixed suite-2 INIT/RESPONSE sizes make the transcript unambiguous.
inline void extract(ascon::key_type& prk, const ascon::key_type& psk,
                    std::uint16_t id, const std::array<std::uint8_t, 32>& dh) {
    static constexpr char label[] = "TUNTOM-AKDF-v1-EXTRACT";
    std::array<std::uint8_t, sizeof(label) + 32> input {};
    WipeGuard wipe(input.data(), input.size());
    std::memcpy(input.data(), label, sizeof(label)); // includes NUL
    std::copy(dh.begin(), dh.end(), input.data() + sizeof(label));
    ascon::mac(psk, id, input.data(), input.size(), prk);
}
// Single-block HKDF-like expansion: label\0 || len(context)[8] || context || 01.
// Separate labels instead of a variable-length multi-block output API.
inline void expand(ascon::key_type& output, const ascon::key_type& prk,
                   std::uint16_t id, const char* label,
                   const std::vector<std::uint8_t>& transcript) {
    const auto n = std::strlen(label) + 1;
    std::vector<std::uint8_t> input(n + 8 + transcript.size() + 1);
    std::memcpy(input.data(), label, n);
    ascon::store_be64(input.data() + n, transcript.size());
    std::copy(transcript.begin(), transcript.end(), input.data() + n + 8);
    input.back() = 1;
    ascon::mac(prk, id, input.data(), input.size(), output);
}
} // namespace tuntom::akdf
