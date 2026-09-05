#pragma once

#include "packet.hpp"
#include "wire.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <arpa/inet.h>

namespace tuntom {

class Protocol {
public:
    virtual ~Protocol() = default;

    virtual std::uint8_t version() const = 0;
    virtual std::vector<std::uint8_t> encode(const Packet& packet) const = 0;
    virtual bool decode(
        const std::uint8_t* data,
        std::size_t size,
        Packet& packet) const = 0;
};

class ProtocolV1 final : public Protocol {
public:
    std::uint8_t version() const override {
        return protocol_version_v1;
    }

    std::vector<std::uint8_t> encode(const Packet& packet) const override {
        std::vector<std::uint8_t> output(
            protocol_header_v1_size + packet.payload.size());

        store_be32(output.data() + 0, protocol_magic);
        store_be16(output.data() + 4, packet.tunnel_id);
        output[6] = protocol_version_v1;
        output[7] = static_cast<std::uint8_t>(packet.type);

        if (not packet.payload.empty()) {
            std::memcpy(
                output.data() + protocol_header_v1_size,
                packet.payload.data(),
                packet.payload.size());
        }

        return output;
    }

    bool decode(
        const std::uint8_t* data,
        std::size_t size,
        Packet& packet) const override {

        if (size < protocol_header_v1_size) {
            return false;
        }

        if (
            load_be32(data + 0) != protocol_magic or
            data[6] != protocol_version_v1) {

            return false;
        }

        const auto type = static_cast<PacketType>(data[7]);
        if (
            type != PacketType::hello and
            type != PacketType::keepalive and
            type != PacketType::data) {

            return false;
        }

        packet.type = type;
        packet.tunnel_id = load_be16(data + 4);
        packet.protocol_version = protocol_version_v1;
        packet.sequence = 0;
        packet.message_id = 0;
        packet.fragment_offset = 0;
        packet.original_length = static_cast<std::uint32_t>(
            size - protocol_header_v1_size);
        packet.payload.assign(data + protocol_header_v1_size, data + size);
        return true;
    }
};

class ProtocolV2 final : public Protocol {
public:
    ProtocolV2(
        std::uint16_t tunnel_id,
        const ascon::key_type& master_key)
        : tunnel_id_(tunnel_id),
          node_key_(ascon::derive_node_key(master_key, tunnel_id)) {
    }

    std::uint8_t version() const override {
        return protocol_version_v2;
    }

    std::vector<std::uint8_t> encode(const Packet& packet) const override {
        std::vector<std::uint8_t> output(
            protocol_header_v2_size + packet.payload.size());

        store_be32(output.data() + 0, protocol_magic);
        store_be16(output.data() + 4, packet.tunnel_id);
        output[6] = protocol_version_v2;
        output[7] = static_cast<std::uint8_t>(packet.type);
        store_be64(output.data() + 8, packet.sequence);

        if (not packet.payload.empty()) {
            std::memcpy(
                output.data() + protocol_header_v2_size,
                packet.payload.data(),
                packet.payload.size());
        }

        std::vector<std::uint8_t> mac_input(16 + packet.payload.size());
        std::memcpy(mac_input.data(), output.data(), 16);

        if (not packet.payload.empty()) {
            std::memcpy(
                mac_input.data() + 16,
                packet.payload.data(),
                packet.payload.size());
        }

        ascon::tag_type tag {};
        ascon::mac(
            node_key_,
            tunnel_id_,
            mac_input.data(),
            mac_input.size(),
            tag);

        std::memcpy(output.data() + 16, tag.data(), tag.size());

        return output;
    }

    bool decode(
        const std::uint8_t* data,
        std::size_t size,
        Packet& packet) const override {

        if (size < protocol_header_v2_size) {
            return false;
        }

        if (
            load_be32(data + 0) != protocol_magic or
            data[6] != protocol_version_v2) {

            return false;
        }

        const std::uint16_t tunnel_id = load_be16(data + 4);
        if (tunnel_id != tunnel_id_) {
            return false;
        }

        const auto type = static_cast<PacketType>(data[7]);
        if (
            type != PacketType::hello and
            type != PacketType::keepalive and
            type != PacketType::data) {

            return false;
        }

        std::vector<std::uint8_t> mac_input(
            16 + size - protocol_header_v2_size);

        std::memcpy(mac_input.data(), data, 16);

        if (size > protocol_header_v2_size) {
            std::memcpy(
                mac_input.data() + 16,
                data + protocol_header_v2_size,
                size - protocol_header_v2_size);
        }

        ascon::tag_type expected_tag {};
        ascon::mac(
            node_key_,
            tunnel_id_,
            mac_input.data(),
            mac_input.size(),
            expected_tag);

        if (
            not ascon::constant_time_equal(
                data + 16,
                expected_tag.data(),
                expected_tag.size())) {

            return false;
        }

        packet.type = type;
        packet.tunnel_id = tunnel_id;
        packet.protocol_version = protocol_version_v2;
        packet.sequence = load_be64(data + 8);
        packet.message_id = 0;
        packet.fragment_offset = 0;
        packet.original_length = static_cast<std::uint32_t>(
            size - protocol_header_v2_size);
        packet.payload.assign(data + protocol_header_v2_size, data + size);
        return true;
    }

private:
    std::uint16_t tunnel_id_ = 0;
    ascon::key_type node_key_ {};
};

class ProtocolV4 final : public Protocol {
public:
    ProtocolV4(
        std::uint16_t tunnel_id,
        const ascon::key_type& master_key,
        bool server_mode)
        : tunnel_id_(tunnel_id),
          tx_key_(ascon::derive_direction_key(master_key, tunnel_id, not server_mode)),
          rx_key_(ascon::derive_direction_key(master_key, tunnel_id, server_mode)) {
    }

    ProtocolV4(std::uint16_t tunnel_id, const ascon::key_type& tx,
               const ascon::key_type& rx, bool encrypt = false)
        : tunnel_id_(tunnel_id), tx_key_(tx), rx_key_(rx), encrypt_(encrypt),
          tx_aead_(tx), rx_aead_(rx) {}

    std::uint8_t version() const override {
        return protocol_version_v4;
    }

    std::vector<std::uint8_t> encode(const Packet& packet) const override {
        std::vector<std::uint8_t> output;
        std::vector<std::uint8_t> mac_input;
        encode_into(packet, output, mac_input);
        return output;
    }

    void encode_into(
        const Packet& packet,
        std::vector<std::uint8_t>& output,
        std::vector<std::uint8_t>& mac_input) const {

        output.resize(
            protocol_header_v4_size + packet.payload.size());

        // Bytes 0..31 are authenticated metadata.
        store_be32(output.data() + 0, protocol_magic);
        store_be16(output.data() + 4, packet.tunnel_id);
        output[6] = protocol_version_v4;
        output[7] = static_cast<std::uint8_t>(packet.type) | (encrypt_ ? 0x80 : 0);
        store_be64(output.data() + 8, packet.sequence);
        store_be64(output.data() + 16, packet.message_id);
        store_be32(output.data() + 24, packet.fragment_offset);
        store_be32(output.data() + 28, packet.original_length);

        if (encrypt_) {
            std::array<std::uint8_t, 16> nonce {};
            store_be64(nonce.data() + 8, packet.sequence);
            tx_aead_.encrypt(nonce.data(), output.data(), 32,
                packet.payload.data(), packet.payload.size(),
                output.data() + protocol_header_v4_size, output.data() + 32);
            return;
        }

        if (not packet.payload.empty()) {
            std::memcpy(
                output.data() + protocol_header_v4_size,
                packet.payload.data(),
                packet.payload.size());
        }

        // The tag itself occupies bytes 32..47 and is not part of the MAC
        // input. The complete V4 metadata (0..31) plus payload is covered.
        mac_input.resize(32 + packet.payload.size());
        std::memcpy(mac_input.data(), output.data(), 32);

        if (not packet.payload.empty()) {
            std::memcpy(
                mac_input.data() + 32,
                packet.payload.data(),
                packet.payload.size());
        }

        ascon::tag_type tag {};
        ascon::mac(
            tx_key_,
            tunnel_id_,
            mac_input.data(),
            mac_input.size(),
            tag);

        std::memcpy(output.data() + 32, tag.data(), tag.size());

        if (log_enabled(LogLevel::debug)) {
            std::cerr
                << "ENCODE v4 seq=" << packet.sequence
                << " msg=" << packet.message_id
                << " offset=" << packet.fragment_offset
                << " original=" << packet.original_length
                << " payload=" << packet.payload.size()
                << " output=" << output.size()
                << "\n";
        }
    }

    bool decode(
        const std::uint8_t* data,
        std::size_t size,
        Packet& packet) const override {

        std::vector<std::uint8_t> mac_input;
        return decode_with_scratch(
            data,
            size,
            packet,
            mac_input);
    }

    bool decode_with_scratch(
        const std::uint8_t* data,
        std::size_t size,
        Packet& packet,
        std::vector<std::uint8_t>& mac_input) const {

        if (size < protocol_header_v4_size) {
            return false;
        }

        if (
            load_be32(data + 0) != protocol_magic or
            data[6] != protocol_version_v4) {

            return false;
        }

        const std::uint16_t tunnel_id = load_be16(data + 4);
        if (tunnel_id != tunnel_id_) {
            return false;
        }

        if (bool(data[7] & 0x80) != encrypt_) return false;
        const auto type = static_cast<PacketType>(data[7] & 0x7f);
        if (
            type != PacketType::hello and
            type != PacketType::keepalive and
            type != PacketType::data and
            type != PacketType::ping and
            type != PacketType::pong and
            type != PacketType::mtu_probe and
            type != PacketType::mtu_reply and
            type != PacketType::init and type != PacketType::response and
            type != PacketType::confirm and type != PacketType::confirm_ack) {

            return false;
        }

        if (encrypt_) {
            std::array<std::uint8_t, 16> nonce {};
            std::memcpy(nonce.data() + 8, data + 8, 8);
            mac_input.resize(size - protocol_header_v4_size);
            if (not rx_aead_.decrypt(nonce.data(), data, 32,
                    data + protocol_header_v4_size, mac_input.size(), data + 32,
                    mac_input.data())) return false;
            packet.payload.swap(mac_input);
        } else {
            mac_input.resize(
                32 + size - protocol_header_v4_size);

            std::memcpy(mac_input.data(), data, 32);

            if (size > protocol_header_v4_size) {
                std::memcpy(
                    mac_input.data() + 32,
                    data + protocol_header_v4_size,
                    size - protocol_header_v4_size);
            }

            ascon::tag_type expected_tag {};
            ascon::mac(
                rx_key_,
                tunnel_id_,
                mac_input.data(),
                mac_input.size(),
                expected_tag);

            if (
                not ascon::constant_time_equal(
                    data + 32,
                    expected_tag.data(),
                    expected_tag.size())) {

                return false;
            }
        }

        packet.type = type;
        packet.tunnel_id = tunnel_id;
        packet.protocol_version = protocol_version_v4;
        packet.sequence = load_be64(data + 8);
        packet.message_id = load_be64(data + 16);
        packet.fragment_offset = load_be32(data + 24);
        packet.original_length = load_be32(data + 28);
        if (not encrypt_) packet.payload.assign(data + protocol_header_v4_size, data + size);

        if (type == PacketType::init or type == PacketType::response) {
            const std::size_t expected = type == PacketType::init ? 44 : 68;
            if (packet.sequence != 0 or packet.message_id == 0 or
                packet.fragment_offset != 0 or packet.original_length != 0 or
                packet.payload.size() != expected) return false;
            // Supported suites have no DH share.
            if (load_be16(packet.payload.data() + expected - 4) > 1 or
                load_be16(packet.payload.data() + expected - 2) != 0) return false;
        } else if (type == PacketType::confirm or type == PacketType::confirm_ack) {
            if (packet.message_id == 0 or packet.fragment_offset != 0 or
                packet.original_length != 0 or not packet.payload.empty() or
                (packet.sequence & 0x0000ffffffffffffULL) != 0) return false;
        } else if (packet.type == PacketType::data) {
            if (
                packet.message_id == 0 or
                packet.original_length == 0 or
                packet.payload.empty()) {

                return false;
            }

            const std::uint64_t fragment_end =
                static_cast<std::uint64_t>(packet.fragment_offset) +
                packet.payload.size();

            if (fragment_end > packet.original_length) {
                return false;
            }
        } else if (
            packet.type == PacketType::ping or
            packet.type == PacketType::pong) {

            if (
                packet.message_id == 0 or
                packet.fragment_offset != 0 or
                packet.original_length != 0 or
                not packet.payload.empty()) {

                return false;
            }
        } else if (packet.type == PacketType::mtu_probe) {
            if (
                packet.message_id == 0 or
                packet.fragment_offset != 0 or
                packet.original_length < min_transport_mtu or
                packet.original_length > max_ip_packet_size) {

                return false;
            }
        } else if (packet.type == PacketType::mtu_reply) {
            if (
                packet.message_id == 0 or
                packet.fragment_offset != 0 or
                packet.original_length < min_transport_mtu or
                packet.original_length > max_ip_packet_size or
                not packet.payload.empty()) {

                return false;
            }
        } else {
            if (
                packet.message_id != 0 or
                packet.fragment_offset != 0 or
                packet.original_length != 0 or
                not packet.payload.empty()) {

                return false;
            }
        }

        return true;
    }

private:
    std::uint16_t tunnel_id_ = 0;
    ascon::key_type tx_key_ {};
    ascon::key_type rx_key_ {};
    bool encrypt_ = false;
    ascon::Aead128 tx_aead_ {tx_key_}, rx_aead_ {rx_key_};
};

} // namespace tuntom
