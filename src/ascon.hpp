#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tuntom {

namespace ascon {

inline constexpr std::size_t key_size = 16;
inline constexpr std::size_t tag_size = 16;

using key_type = std::array<std::uint8_t, key_size>;
using tag_type = std::array<std::uint8_t, tag_size>;

inline std::uint64_t rotate_right(std::uint64_t value, unsigned shift) {
    return (value >> shift) | (value << (64U - shift));
}

inline std::uint64_t load_be64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8U) | data[i];
    }
    return value;
}

inline void store_be64(std::uint8_t* data, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        data[i] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8U;
    }
}

inline void permute(std::array<std::uint64_t, 5>& state, int rounds) {
    static constexpr std::array<std::uint8_t, 12> round_constants {
        0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5,
        0x96, 0x87, 0x78, 0x69, 0x5a, 0x4b,
    };

    const int first_round = 12 - rounds;
    for (int round = first_round; round < 12; ++round) {
        state[2] ^= round_constants[round];

        state[0] ^= state[4];
        state[4] ^= state[3];
        state[2] ^= state[1];

        // These are bitwise operations on 64 parallel S-boxes.
        const std::uint64_t t0 = ~state[0] & state[1];
        const std::uint64_t t1 = ~state[1] & state[2];
        const std::uint64_t t2 = ~state[2] & state[3];
        const std::uint64_t t3 = ~state[3] & state[4];
        const std::uint64_t t4 = ~state[4] & state[0];

        state[0] ^= t1;
        state[1] ^= t2;
        state[2] ^= t3;
        state[3] ^= t4;
        state[4] ^= t0;

        state[1] ^= state[0];
        state[0] ^= state[4];
        state[3] ^= state[2];
        state[2] = ~state[2];

        state[0] ^= rotate_right(state[0], 19) ^ rotate_right(state[0], 28);
        state[1] ^= rotate_right(state[1], 61) ^ rotate_right(state[1], 39);
        state[2] ^= rotate_right(state[2], 1) ^ rotate_right(state[2], 6);
        state[3] ^= rotate_right(state[3], 10) ^ rotate_right(state[3], 17);
        state[4] ^= rotate_right(state[4], 7) ^ rotate_right(state[4], 41);
    }
}

inline void mac(
    const key_type& key,
    std::uint16_t tunnel_id,
    const std::uint8_t* data,
    std::size_t size,
    tag_type& tag) {

    const std::uint64_t k0 = load_be64(key.data());
    const std::uint64_t k1 = load_be64(key.data() + 8);

    // Compact Ascon-p[12]/p[8]-based keyed sponge MAC.
    // Kept self-contained for the single-file deployment model.
    std::array<std::uint64_t, 5> state {
        0x54554e544f4d4d41ULL, // "TUNTOMMA" domain separator
        k0,
        k1,
        static_cast<std::uint64_t>(tunnel_id),
        ~static_cast<std::uint64_t>(tunnel_id),
    };

    permute(state, 12);
    state[3] ^= k0;
    state[4] ^= k1;

    while (size >= 8) {
        state[0] ^= load_be64(data);
        permute(state, 8);
        data += 8;
        size -= 8;
    }

    std::array<std::uint8_t, 8> final_block {};
    if (size > 0) {
        std::memcpy(final_block.data(), data, size);
    }
    final_block[size] = 0x80;

    state[0] ^= load_be64(final_block.data());
    state[4] ^= 1;

    state[1] ^= k0;
    state[2] ^= k1;
    permute(state, 12);
    state[3] ^= k0;
    state[4] ^= k1;

    store_be64(tag.data(), state[3]);
    store_be64(tag.data() + 8, state[4]);
}

inline key_type derive_node_key(const key_type& master_key, std::uint16_t tunnel_id) {
    static constexpr std::array<std::uint8_t, 15> label {
        'T', 'U', 'N', 'T', 'O', 'M', '-', 'N', 'O', 'D', 'E', '-', 'K', 'E', 'Y'
    };

    tag_type derived {};
    mac(master_key, tunnel_id, label.data(), label.size(), derived);

    key_type node_key {};
    std::copy(derived.begin(), derived.end(), node_key.begin());
    return node_key;
}

// Distinct domains bind the key to both the wire version and traffic direction.
inline key_type derive_direction_key(
    const key_type& master_key,
    std::uint16_t tunnel_id,
    bool client_to_server) {
    static constexpr char c2s[] = "TUNTOM-V4-CLIENT-TO-SERVER";
    static constexpr char s2c[] = "TUNTOM-V4-SERVER-TO-CLIENT";
    const char* label = client_to_server ? c2s : s2c;
    key_type key {};
    mac(master_key, tunnel_id,
        reinterpret_cast<const std::uint8_t*>(label),
        std::strlen(label), key);
    return key;
}

inline bool constant_time_equal(const std::uint8_t* a, const std::uint8_t* b, std::size_t size) {
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < size; ++i) {
        difference |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    }
    return difference == 0;
}

} // namespace ascon

} // namespace tuntom
