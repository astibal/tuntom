#pragma once

#include "wire.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace tuntom {

inline constexpr std::uint8_t switch_protocol_version = 1;
inline constexpr std::size_t switch_base_header_size = 8;
inline constexpr std::size_t switch_label_size = 8;
inline constexpr std::size_t switch_max_labels = 8;

enum class SwitchOpcode : std::uint8_t {
    switch_packet = 1,
    exit_packet = 2,
};

struct SwitchFrameView {
    SwitchOpcode opcode = SwitchOpcode::switch_packet;
    const std::uint8_t* labels = nullptr;
    std::size_t label_count = 0;
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;

    std::uint64_t label(std::size_t index) const {
        if (index >= label_count) {
            throw std::out_of_range("Switch label index out of range");
        }
        return load_be64(labels + index * switch_label_size);
    }
};

inline bool decode_switch_frame(
    const std::uint8_t* data,
    std::size_t size,
    SwitchFrameView& frame) {

    if (size < switch_base_header_size or
        data[0] != switch_protocol_version or
        data[2] != 0) {
        return false;
    }

    const auto opcode = static_cast<SwitchOpcode>(data[1]);
    if (opcode != SwitchOpcode::switch_packet and
        opcode != SwitchOpcode::exit_packet) {
        return false;
    }

    const std::size_t label_count = data[3];
    if (label_count == 0 or label_count > switch_max_labels) {
        return false;
    }

    const std::size_t total_length = load_be32(data + 4);
    const std::size_t header_size =
        switch_base_header_size + label_count * switch_label_size;

    if (total_length != size or header_size > size) {
        return false;
    }

    frame.opcode = opcode;
    frame.labels = data + switch_base_header_size;
    frame.label_count = label_count;
    frame.payload = data + header_size;
    frame.payload_size = size - header_size;
    return frame.payload_size != 0;
}

inline std::vector<std::uint8_t> encode_switch_frame(
    SwitchOpcode opcode,
    const std::vector<std::uint64_t>& labels,
    const std::uint8_t* payload,
    std::size_t payload_size) {

    if ((opcode != SwitchOpcode::switch_packet and
         opcode != SwitchOpcode::exit_packet) or
        labels.empty() or labels.size() > switch_max_labels or
        payload_size == 0) {
        throw std::runtime_error("Invalid switch frame");
    }

    const std::size_t header_size =
        switch_base_header_size + labels.size() * switch_label_size;
    const std::size_t total_size = header_size + payload_size;
    if (total_size > UINT32_MAX) {
        throw std::runtime_error("Switch frame is too large");
    }

    std::vector<std::uint8_t> output(total_size);
    output[0] = switch_protocol_version;
    output[1] = static_cast<std::uint8_t>(opcode);
    output[2] = 0;
    output[3] = static_cast<std::uint8_t>(labels.size());
    store_be32(output.data() + 4, static_cast<std::uint32_t>(total_size));
    for (std::size_t index = 0; index < labels.size(); ++index) {
        store_be64(
            output.data() + switch_base_header_size + index * switch_label_size,
            labels[index]);
    }
    std::copy(payload, payload + payload_size, output.begin() +
        static_cast<std::ptrdiff_t>(header_size));
    return output;
}

inline void replace_top_switch_label(
    std::vector<std::uint8_t>& frame,
    std::uint64_t label) {

    SwitchFrameView decoded;
    if (not decode_switch_frame(frame.data(), frame.size(), decoded)) {
        throw std::runtime_error("Cannot relabel invalid switch frame");
    }
    store_be64(frame.data() + switch_base_header_size, label);
}

inline void set_switch_opcode(
    std::vector<std::uint8_t>& frame,
    SwitchOpcode opcode) {

    if (opcode != SwitchOpcode::switch_packet and
        opcode != SwitchOpcode::exit_packet) {
        throw std::runtime_error("Invalid switch opcode");
    }
    SwitchFrameView decoded;
    if (not decode_switch_frame(frame.data(), frame.size(), decoded)) {
        throw std::runtime_error("Cannot change opcode of invalid switch frame");
    }
    frame[1] = static_cast<std::uint8_t>(opcode);
}

} // namespace tuntom
