#pragma once

#include "switch_protocol.hpp"
#include <cstring>

namespace tuntom::ipc {
// V2 is a connection/transport protocol. The inner label frame stays V1.
inline constexpr std::uint8_t version = 2;
inline constexpr std::uint32_t mmap_ref = 1, mmap_batch = 2, known_caps = 3;
inline constexpr std::uint32_t pool_abi = 1, max_slots = 128, max_batch = 16;
inline constexpr std::uint32_t min_frame = 17, max_frame = 8 + 8 * 8 + 65535;
inline constexpr std::size_t max_record = 24 + 16 * max_batch;
enum class Type : std::uint8_t { hello = 1, welcome = 2, setup = 3,
    ready = 4, active = 5, ref = 16, batch = 17, error = 127 };
enum class Mode { automatic, legacy, inline_only };
inline Mode parse_mode(const std::string &text) {
    if (text == "auto") return Mode::automatic;
    if (text == "v1") return Mode::legacy;
    if (text == "inline") return Mode::inline_only;
    throw std::runtime_error("IPC mode must be auto, v1 or inline");
}
struct Options {
    Mode mode = Mode::automatic;
    std::uint32_t slots = 128, batch = 8, frame_limit = max_frame, frame_capacity = 16384;
};
struct Parameters {
    std::uint64_t epoch = 0;
    std::uint32_t caps = 0, abi = 0, slots = 0, frame_limit = max_frame, capacity = 0;
    std::uint64_t stride = 0, mapping_size = 0;
    std::uint32_t batch = 1;
    void inline_only() { caps = abi = slots = capacity = 0; stride = mapping_size = 0; batch = 1; }
};
struct Record {
    std::array<std::uint8_t, max_record> bytes{};
    std::size_t size = 0;
};
inline Record header(Type type, std::size_t size) {
    Record r;
    r.size = size;
    r.bytes[0] = 'T'; r.bytes[1] = 'T'; r.bytes[2] = 'X'; r.bytes[3] = version;
    r.bytes[4] = static_cast<std::uint8_t>(type);
    store_be16(r.bytes.data() + 6, static_cast<std::uint16_t>(size));
    return r;
}
inline bool is_header(const std::uint8_t *p, std::size_t n, Type type, std::size_t exact) {
    return n == exact && n >= 8 && p[0] == 'T' && p[1] == 'T' && p[2] == 'X' &&
        p[3] == version && p[4] == static_cast<std::uint8_t>(type) && p[5] == 0 &&
        load_be16(p + 6) == n;
}
inline bool valid_frame_limit(std::uint32_t value) { return value >= min_frame && value <= max_frame; }
inline bool valid_caps(std::uint32_t caps) {
    return !(caps & ~known_caps) && (!(caps & mmap_batch) || (caps & mmap_ref));
}
inline Record hello(const Options &o) {
    auto r = header(Type::hello, 32);
    const bool mmap = o.mode == Mode::automatic;
    store_be32(r.bytes.data() + 8, mmap ? known_caps : 0);
    store_be32(r.bytes.data() + 12, mmap ? pool_abi : 0);
    store_be32(r.bytes.data() + 16, mmap ? o.slots : 0);
    store_be32(r.bytes.data() + 20, o.frame_limit);
    store_be32(r.bytes.data() + 24, mmap ? o.batch : 1);
    return r;
}
inline bool decode_hello(const std::uint8_t *p, std::size_t n, Parameters &v) {
    if (!is_header(p, n, Type::hello, 32) || load_be32(p + 28)) return false;
    v.caps = load_be32(p + 8); v.abi = load_be32(p + 12); v.slots = load_be32(p + 16);
    v.frame_limit = load_be32(p + 20); v.batch = load_be32(p + 24);
    if (!valid_frame_limit(v.frame_limit) || !v.batch || v.batch > max_batch) return false;
    if (v.caps & mmap_ref) return v.slots != 0;
    return !v.abi && !v.slots && !(v.caps & mmap_batch) && v.batch == 1;
}
inline Record welcome(const Parameters &v) {
    auto r = header(Type::welcome, 40); auto *p = r.bytes.data();
    store_be64(p + 8, v.epoch); store_be32(p + 16, v.caps); store_be32(p + 20, v.abi);
    store_be32(p + 24, v.slots); store_be32(p + 28, v.frame_limit); store_be32(p + 32, v.batch);
    return r;
}
inline bool decode_welcome(const std::uint8_t *p, std::size_t n, Parameters &v) {
    if (!is_header(p, n, Type::welcome, 40) || load_be32(p + 36)) return false;
    v.epoch = load_be64(p + 8); v.caps = load_be32(p + 16); v.abi = load_be32(p + 20);
    v.slots = load_be32(p + 24); v.frame_limit = load_be32(p + 28); v.batch = load_be32(p + 32);
    if (!v.epoch || !valid_caps(v.caps) || !valid_frame_limit(v.frame_limit)) return false;
    if (!(v.caps & mmap_ref)) return !v.abi && !v.slots && v.batch == 1;
    return v.abi == pool_abi && v.slots && v.slots <= max_slots && v.batch &&
        v.batch <= max_batch && ((v.caps & mmap_batch) || v.batch == 1);
}
inline Record setup(const Parameters &v) {
    auto r = header(Type::setup, 56); auto *p = r.bytes.data();
    store_be64(p + 8, v.epoch); store_be32(p + 16, v.caps); store_be32(p + 20, v.abi);
    store_be32(p + 24, v.slots); store_be32(p + 28, v.capacity);
    store_be64(p + 32, v.stride); store_be64(p + 40, v.mapping_size); store_be32(p + 48, v.batch);
    return r;
}
inline bool decode_setup(const std::uint8_t *p, std::size_t n, const Parameters &offered, Parameters &v) {
    if (!is_header(p, n, Type::setup, 56) || load_be32(p + 52)) return false;
    v = offered;
    v.epoch = load_be64(p + 8); v.caps = load_be32(p + 16); v.abi = load_be32(p + 20);
    v.slots = load_be32(p + 24); v.capacity = load_be32(p + 28);
    v.stride = load_be64(p + 32); v.mapping_size = load_be64(p + 40); v.batch = load_be32(p + 48);
    if (v.epoch != offered.epoch || (v.caps & ~offered.caps) || !valid_caps(v.caps)) return false;
    if (!v.caps) return !v.abi && !v.slots && !v.capacity && !v.stride && !v.mapping_size && v.batch == 1;
    const std::uint64_t stride = (64ULL + v.capacity + 63) & ~63ULL;
    return v.abi == pool_abi && v.slots == offered.slots && v.batch == offered.batch &&
        valid_frame_limit(v.capacity) && v.capacity <= offered.frame_limit && v.stride == stride &&
        v.mapping_size == 4096 + v.slots * stride;
}
inline Record state(Type type, const Parameters &v, std::uint32_t status = 0) {
    auto r = header(type, 24);
    store_be64(r.bytes.data() + 8, v.epoch); store_be32(r.bytes.data() + 16, v.caps);
    store_be32(r.bytes.data() + 20, status);
    return r;
}
inline bool decode_state(const std::uint8_t *p, std::size_t n, Type type,
                         const Parameters &v, std::uint32_t &caps, std::uint32_t &status) {
    if (!is_header(p, n, type, 24) || load_be64(p + 8) != v.epoch) return false;
    caps = load_be32(p + 16); status = load_be32(p + 20);
    return valid_caps(caps) && !(caps & ~v.caps);
}
struct Ref { std::uint32_t slot = 0, length = 0; std::uint64_t token = 0; };
inline Record references(const Parameters &v, std::uint32_t pool, const Ref *refs, std::size_t count) {
    const bool batch = v.caps & mmap_batch;
    auto r = header(batch ? Type::batch : Type::ref, batch ? 24 + count * 16 : 40);
    auto *p = r.bytes.data();
    store_be64(p + 8, v.epoch); store_be32(p + 16, pool);
    if (!batch) {
        store_be32(p + 20, refs[0].slot); store_be64(p + 24, refs[0].token);
        store_be32(p + 32, refs[0].length);
    } else {
        store_be16(p + 20, static_cast<std::uint16_t>(count));
        for (std::size_t i = 0; i < count; ++i) {
            store_be32(p + 24 + i * 16, refs[i].slot);
            store_be32(p + 28 + i * 16, refs[i].length);
            store_be64(p + 32 + i * 16, refs[i].token);
        }
    }
    return r;
}
inline bool decode_references(const std::uint8_t *p, std::size_t n, const Parameters &v,
                              std::uint32_t pool, std::array<Ref, max_batch> &refs, std::size_t &count) {
    if (!(v.caps & mmap_ref) || n < 24 || load_be64(p + 8) != v.epoch || load_be32(p + 16) != pool)
        return false;
    if (is_header(p, n, Type::ref, 40)) {
        if (load_be32(p + 36)) return false;
        count = 1; refs[0] = {load_be32(p + 20), load_be32(p + 32), load_be64(p + 24)};
    } else {
        count = load_be16(p + 20);
        if (!(v.caps & mmap_batch) || !count || count > v.batch || count > max_batch ||
            load_be16(p + 22) || !is_header(p, n, Type::batch, 24 + count * 16)) return false;
        for (std::size_t i = 0; i < count; ++i)
            refs[i] = {load_be32(p + 24 + i * 16), load_be32(p + 28 + i * 16), load_be64(p + 32 + i * 16)};
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto &r = refs[i];
        if (r.slot >= v.slots || r.length < min_frame || r.length > v.capacity ||
            !(r.token & 1) || r.token == UINT64_MAX) return false;
        for (std::size_t j = 0; j < i; ++j) if (refs[j].slot == r.slot) return false;
    }
    return true;
}
} // namespace tuntom::ipc
