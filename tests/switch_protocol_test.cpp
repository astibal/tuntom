#include "../src/ipc/switch_protocol.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace tuntom;

void require(bool value, const char* why) {
    if (not value) throw std::runtime_error(why);
}

int main() {
    const auto registration = encode_switch_registration("edge-42");
    std::string port_id;
    require(decode_switch_registration(
        registration.data(), registration.size(), port_id), "registration roundtrip failed");
    require(port_id == "edge-42", "registration ID lost");

    const std::vector<std::uint8_t> payload {0x45, 0, 0, 20};
    auto wire = encode_switch_frame(
        SwitchOpcode::switch_packet,
        {17, 83},
        payload.data(),
        payload.size());

    require(wire.size() == 28, "unexpected encoded size");

    SwitchFrameView frame;
    require(decode_switch_frame(wire.data(), wire.size(), frame), "roundtrip failed");
    require(frame.opcode == SwitchOpcode::switch_packet, "wrong opcode");
    require(frame.label_count == 2, "wrong label count");
    require(frame.label(0) == 17 and frame.label(1) == 83, "wrong labels");
    require(std::vector<std::uint8_t>(frame.payload, frame.payload + frame.payload_size) == payload,
            "wrong payload");

    replace_top_switch_label(wire, 99);
    set_switch_opcode(wire, SwitchOpcode::exit_packet);
    require(decode_switch_frame(wire.data(), wire.size(), frame), "modified frame invalid");
    require(frame.opcode == SwitchOpcode::exit_packet and frame.label(0) == 99 and
            frame.label(1) == 83, "label stack mutation failed");

    for (std::size_t size = 0; size < wire.size(); ++size) {
        require(not decode_switch_frame(wire.data(), size, frame), "truncated frame accepted");
    }

    for (std::size_t offset : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U}) {
        auto bad = wire;
        bad[offset] ^= 0x80;
        require(not decode_switch_frame(bad.data(), bad.size(), frame), "bad header accepted");
    }

    bool rejected = false;
    try {
        encode_switch_frame(SwitchOpcode::switch_packet, {}, payload.data(), payload.size());
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "empty label stack accepted");

    auto bad_registration = registration;
    bad_registration[5] = 1;
    require(not decode_switch_registration(
        bad_registration.data(), bad_registration.size(), port_id),
        "reserved registration byte accepted");

    std::cout << "PASS: switch frame codec, label stacks, mutation and malformed input\n";
}
