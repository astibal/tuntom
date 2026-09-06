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

class ProtocolV5 final : public Protocol {
public:
    ProtocolV5(
        std::uint16_t tunnel_id,
        const ascon::key_type& master_key,
        bool server_mode)
        : tunnel_id_(tunnel_id),
          tx_key_(ascon::derive_direction_key(master_key, tunnel_id, not server_mode)),
          rx_key_(ascon::derive_direction_key(master_key, tunnel_id, server_mode)) {
    }

    ProtocolV5(std::uint16_t tunnel_id, const ascon::key_type& tx,
               const ascon::key_type& rx, bool encrypt = false)
        : tunnel_id_(tunnel_id), tx_key_(tx), rx_key_(rx), encrypt_(encrypt),
          tx_aead_(tx), rx_aead_(rx) {}

    ~ProtocolV5() override {
        secure_zero(tx_key_.data(), tx_key_.size());
        secure_zero(rx_key_.data(), rx_key_.size());
    }

    std::uint8_t version() const override {
        return protocol_version_v5;
    }

    std::vector<std::uint8_t> encode(const Packet& packet) const override {
        std::vector<std::uint8_t> output;
        std::vector<std::uint8_t> mac_input;
        encode_into(packet, output, mac_input);
        return output;
    }

    static bool fragmented(const Packet& packet) {
        return packet.type == PacketType::data and
            (packet.fragment_offset != 0 or packet.original_length != packet.payload.size());
    }

    // Type-specific metadata follows the common type/flags + sequence prefix.
    static std::size_t metadata_size(PacketType type, bool fragment = false) {
        switch (type) {
        case PacketType::hello: case PacketType::keepalive: return 9;
        case PacketType::data: return fragment ? 21 : 9;
        case PacketType::ping: case PacketType::pong:
        case PacketType::confirm: case PacketType::confirm_ack: return 17;
        case PacketType::mtu_probe: case PacketType::mtu_reply: return 19;
        case PacketType::init: case PacketType::response: return 18;
        default: return 0;
        }
    }

    static std::size_t header_size(const Packet& packet) {
        return metadata_size(packet.type, fragmented(packet)) + 16;
    }

    void encode_into(const Packet& packet, std::vector<std::uint8_t>& output,
                     std::vector<std::uint8_t>& mac_input) const {
        const bool fragment = fragmented(packet);
        const auto meta = metadata_size(packet.type, fragment);
        if (meta == 0 or packet.fragment_offset > max_ip_packet_size or
            packet.original_length > max_ip_packet_size or
            packet.payload.size() > max_ip_packet_size)
            throw std::runtime_error("Invalid V5 packet metadata");
        const bool handshake = packet.type == PacketType::init or packet.type == PacketType::response;
        if (handshake and encrypt_) throw std::runtime_error("INIT/RESPONSE must use handshake authentication");
        const auto header = meta + 16;
        output.resize(header + packet.payload.size());
        output[0] = static_cast<std::uint8_t>(packet.type) |
            (encrypt_ ? 0x80 : 0) | (fragment ? 0x40 : 0);
        store_be64(output.data() + 1, packet.sequence);
        if (handshake) {
            output[9] = protocol_version_v5;
            store_be64(output.data() + 10, packet.message_id);
        } else if (meta > 9) {
            store_be64(output.data() + 9, packet.message_id);
            if (fragment) {
                store_be16(output.data() + 17, static_cast<std::uint16_t>(packet.fragment_offset));
                store_be16(output.data() + 19, static_cast<std::uint16_t>(packet.original_length));
            } else if (meta == 19) {
                store_be16(output.data() + 17, static_cast<std::uint16_t>(packet.original_length));
            }
        }
        if (encrypt_) {
            std::array<std::uint8_t, 16> nonce {};
            store_be64(nonce.data() + 8, packet.sequence);
            tx_aead_.encrypt(nonce.data(), output.data(), meta,
                packet.payload.data(), packet.payload.size(), output.data() + header,
                output.data() + meta);
            return;
        }
        if (not packet.payload.empty())
            std::memcpy(output.data() + header, packet.payload.data(), packet.payload.size());
        mac_input.assign(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(meta));
        mac_input.insert(mac_input.end(), packet.payload.begin(), packet.payload.end());
        ascon::tag_type tag {};
        ascon::mac(tx_key_, tunnel_id_, mac_input.data(), mac_input.size(), tag);
        std::memcpy(output.data() + meta, tag.data(), tag.size());
    }

    bool decode(const std::uint8_t* data, std::size_t size, Packet& packet) const override {
        std::vector<std::uint8_t> scratch;
        return decode_with_scratch(data, size, packet, scratch);
    }

    bool decode_with_scratch(const std::uint8_t* data, std::size_t size,
                             Packet& packet, std::vector<std::uint8_t>& mac_input) const {
        if (size < protocol_header_v5_size) return false;
        if (bool(data[0] & 0x80) != encrypt_ or (data[0] & 0x30)) return false;
        const auto type = static_cast<PacketType>(data[0] & 0x0f);
        const bool fragment = (data[0] & 0x40) != 0;
        if (fragment and type != PacketType::data) return false;
        const auto meta = metadata_size(type, fragment);
        const auto header = meta + 16;
        if (meta == 0 or size < header or size - header > max_ip_packet_size) return false;
        const bool handshake = type == PacketType::init or type == PacketType::response;
        if (handshake and (encrypt_ or data[9] != protocol_version_v5)) return false;
        if (encrypt_) {
            std::array<std::uint8_t, 16> nonce {};
            std::memcpy(nonce.data() + 8, data + 1, 8);
            mac_input.resize(size - header);
            if (not rx_aead_.decrypt(nonce.data(), data, meta, data + header,
                    mac_input.size(), data + meta, mac_input.data())) return false;
            packet.payload.swap(mac_input);
        } else {
            mac_input.assign(data, data + meta);
            mac_input.insert(mac_input.end(), data + header, data + size);
            ascon::tag_type tag {};
            ascon::mac(rx_key_, tunnel_id_, mac_input.data(), mac_input.size(), tag);
            if (not ascon::constant_time_equal(data + meta, tag.data(), tag.size())) return false;
            packet.payload.assign(data + header, data + size);
        }
        packet.type = type;
        packet.tunnel_id = tunnel_id_; // Socket/configuration context, never transmitted.
        packet.protocol_version = protocol_version_v5;
        packet.sequence = load_be64(data + 1);
        packet.message_id = 0;
        packet.fragment_offset = 0;
        packet.original_length = 0;
        if (handshake) packet.message_id = load_be64(data + 10);
        else if (meta > 9) {
            packet.message_id = load_be64(data + 9);
            if (fragment) {
                packet.fragment_offset = load_be16(data + 17);
                packet.original_length = load_be16(data + 19);
                // Reject a redundant fragment extension: one canonical layout.
                if (packet.fragment_offset == 0 and packet.original_length == packet.payload.size()) return false;
            } else if (meta == 19) packet.original_length = load_be16(data + 17);
        } else if (type == PacketType::data) {
            // A complete datagram needs no transmitted reassembly identity.
            packet.message_id = packet.sequence;
            packet.original_length = static_cast<std::uint32_t>(packet.payload.size());
        }

        if (type == PacketType::init or type == PacketType::response) {
            const std::size_t expected = type == PacketType::init ? 44 : 68;
            if (packet.sequence != 0 or packet.message_id == 0 or
                packet.fragment_offset != 0 or packet.original_length != 0 or
                packet.payload.size() < expected) return false;
            const auto suite = load_be16(packet.payload.data() + expected - 4);
            const auto dh_size = load_be16(packet.payload.data() + expected - 2);
            if (suite > 2 or dh_size != (suite == 2 ? 32 : 0) or
                packet.payload.size() != expected + dh_size) return false;
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
