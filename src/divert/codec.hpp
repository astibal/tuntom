#pragma once

#include "../ipc/switch_protocol.hpp"
#include "../flow_dump.hpp"

namespace tuntom::divert {

// The IPC limit is unchanged. Body length is carried by the cookie label;
// neither its position nor its length is fixed on the wire.
struct Stack {
    std::array<std::uint64_t, switch_max_labels> values{};
    std::size_t size = 0;
    bool push(std::uint64_t value) {
        if (size == values.size()) return false;
        values[size++] = value;
        return true;
    }
    bool operator==(const Stack& other) const {
        return size == other.size && std::equal(values.begin(), values.begin() + size, other.values.begin());
    }
};
inline Stack labels_of(const SwitchFrameView& frame) {
    Stack result;
    for (std::size_t i = 0; i < frame.label_count; ++i) result.push(frame.label(i));
    return result;
}
inline std::uint64_t text_label(const std::string& text) {
    if (text.size() > 8) throw std::runtime_error("label string exceeds eight bytes");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
        value |= std::uint64_t(static_cast<unsigned char>(text[i])) << (56 - 8 * i);
    return value;
}
enum Action : std::uint64_t { offered = 0, onward = 1, to_client = 2, bypass = 3 };
struct Envelope {
    Stack base, body;
    bool present = false;
    void write_flow(std::ostream& out, const char* prefix) const {
        out << ' ' << prefix << "_labels=";
        FlowDump::labels(out, base.values.data(), base.size);
        out << ' ' << prefix << "_divert_body=";
        FlowDump::labels(out, body.values.data(), body.size);
    }
    std::uint64_t origin() const { return body.values[0]; }
    Action action() const { return static_cast<Action>(body.values[1]); }
    bool same_context(const Envelope& other) const { return origin() == other.origin() && original() == other.original(); }
    Stack original() const {
        Stack result;
        for (std::size_t i = 2; i < body.size; ++i) result.push(body.values[i]);
        return result;
    }
};
class Codec {
    std::uint64_t prefix_;
    static constexpr std::uint64_t marker = 0x4456525400000000ULL;
public:
    explicit Codec(const std::string& cookie) : prefix_(text_label(cookie)) {
        if (cookie.size() != 3) throw std::runtime_error("divert cookie requires three ASCII letters");
        for (char c : cookie)
            if (!(('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z')))
                throw std::runtime_error("divert cookie requires three ASCII letters");
    }
    bool attach(const Stack& base, const Stack& body, Stack& output) const {
        if (!base.size || body.size < 3 || !body.values[0] || body.values[1] > bypass ||
            base.size + body.size + 2 > switch_max_labels) return false;
        Stack result = base;
        result.push(prefix_ | (std::uint64_t(body.size + 48) << 32));
        result.push(marker);
        for (std::size_t i = 0; i < body.size; ++i) result.push(body.values[i]);
        output = result;
        return true;
    }
    bool offer(const Stack& base, std::uint64_t origin, Stack& output) const {
        Stack body;
        body.push(origin); body.push(offered);
        for (std::size_t i = 0; i < base.size; ++i)
            if (!body.push(base.values[i])) return false;
        return attach(base, body, output);
    }
    bool split(const Stack& labels, Envelope& output) const {
        Envelope result;
        for (std::size_t i = 0; i < labels.size;) {
            const auto value = labels.values[i];
            const bool own = (value & 0xffffff0000000000ULL) == prefix_ &&
                !(value & 0xffffffffULL) && i + 1 < labels.size && labels.values[i + 1] == marker;
            if (!own) { result.base.push(value); ++i; continue; }
            const auto length = static_cast<unsigned>((value >> 32) & 255);
            if (result.present || length < 51 || length > 126) return false;
            const auto count = length - 48;
            if (count > labels.size - i - 2) return false;
            for (std::size_t j = 0; j < count; ++j) result.body.push(labels.values[i + 2 + j]);
            if (!result.origin() || result.body.values[1] > bypass) return false;
            result.present = true;
            i += count + 2;
        }
        if (!result.base.size) return false;
        output = result;
        return true;
    }
};

// Rebuild the label view without copying the IP payload or allocating memory.
inline SwitchFrameView view_of(const Stack& labels, const SwitchFrameView& input, SwitchFrameHeader& header) {
    encode_switch_header(header, SwitchOpcode::switch_packet, labels.values.data(), labels.size, input.payload_size);
    auto result = input;
    result.labels = header.data() + switch_base_header_size;
    result.label_count = labels.size;
    return result;
}
} // namespace tuntom::divert
