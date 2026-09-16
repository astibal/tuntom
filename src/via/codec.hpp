#pragma once

#include "../divert/codec.hpp"

namespace tuntom::via {
using divert::Stack;
using divert::labels_of;
using divert::view_of;
enum Action : unsigned { offer = 0, onward = 1, bypass = 2, complete = 3 };
struct Envelope {
    Stack base, saved;
    std::uint32_t cookie = 0, chain = 0;
    std::uint64_t origin_id = 0;
    std::uint16_t step = 0;
    Action action = offer;
    bool reverse = false, present = false;
    std::uint64_t origin() const { return origin_id; }
    Stack original() const { return saved; }
    bool same_context(const Envelope& other) const {
        return cookie == other.cookie && chain == other.chain && step == other.step &&
            origin_id == other.origin_id && saved == other.saved;
    }
};
class Codec {
    static constexpr std::uint64_t magic = 0x5649410000ULL;
    static bool letter(unsigned c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
public:
    static bool attach(const Envelope& env, Stack& output) {
        if (!env.base.size || !env.saved.size || !env.origin_id || !env.chain || env.action > complete ||
            !letter(env.cookie >> 16) || !letter((env.cookie >> 8) & 255) || !letter(env.cookie & 255) ||
            env.base.size + 3 + env.saved.size > switch_max_labels) return false;
        output = env.base;
        output.push((std::uint64_t(env.cookie) << 40) | magic | 0x100 | (3 + env.saved.size));
        output.push((std::uint64_t(env.chain) << 32) | (std::uint64_t(env.step) << 16) |
            (env.saved.size << 8) | (unsigned(env.action) << 1) | unsigned(env.reverse));
        output.push(env.origin_id);
        for (std::size_t i = 0; i < env.saved.size; ++i) output.push(env.saved.values[i]);
        return true;
    }
    static bool split(const Stack& labels, Envelope& output) {
        Envelope result;
        for (std::size_t i = 0; i < labels.size;) {
            auto header = labels.values[i];
            if ((header & 0xffffff0000ULL) != magic) { result.base.push(header); ++i; continue; }
            const auto count = header & 255;
            result.cookie = static_cast<std::uint32_t>(header >> 40);
            if (result.present || ((header >> 8) & 255) != 1 || count < 4 || count > labels.size - i ||
                !letter(result.cookie >> 16) || !letter((result.cookie >> 8) & 255) || !letter(result.cookie & 255)) return false;
            const auto ctx = labels.values[i + 1];
            result.chain = static_cast<std::uint32_t>(ctx >> 32); result.step = (ctx >> 16) & 65535;
            const auto saved = (ctx >> 8) & 255, flags = ctx & 255;
            if (!result.chain || count != saved + 3 || flags & 0xf0 || ((flags >> 1) & 7) > complete) return false;
            result.reverse = flags & 1; result.action = static_cast<Action>((flags >> 1) & 7);
            result.origin_id = labels.values[i + 2];
            if (!result.origin_id) return false;
            for (std::size_t j = 0; j < saved; ++j) result.saved.push(labels.values[i + 3 + j]);
            result.present = true; i += count;
        }
        if (!result.base.size) return false;
        output = result;
        return true;
    }
};
} // namespace tuntom::via
