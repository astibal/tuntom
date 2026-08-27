#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <netdb.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t protocol_magic = 0x5554554e; // "UTUN"
constexpr std::uint8_t protocol_version_v1 = 1;
constexpr std::uint8_t protocol_version_v2 = 2;
constexpr std::size_t protocol_header_v1_size = 8;
constexpr std::size_t protocol_header_v2_size = 32;
constexpr std::size_t buffer_size = 65536;
constexpr int keepalive_seconds = 5;

enum class PacketType : std::uint8_t {
    hello = 1,
    keepalive = 2,
    data = 3,
};

enum class Direction {
    tun_to_udp,
    udp_to_tun,
};

struct Packet {
    PacketType type = PacketType::data;
    std::uint16_t tunnel_id = 0;
    std::uint8_t protocol_version = protocol_version_v2;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> payload;
};

void dump_bytes(const std::string& prefix, const std::uint8_t* data, std::size_t size, std::size_t max_bytes = 28) {
    std::cerr << prefix << " " << size << " bytes:";

    const std::size_t count = std::min(size, max_bytes);
    for (std::size_t i = 0; i < count; ++i) {
        std::cerr << " " << std::hex << static_cast<unsigned>(data[i]);
    }

    std::cerr << std::dec << "\n";
}

namespace ascon {

constexpr std::size_t key_size = 16;
constexpr std::size_t tag_size = 16;

using key_type = std::array<std::uint8_t, key_size>;
using tag_type = std::array<std::uint8_t, tag_size>;

std::uint64_t rotate_right(std::uint64_t value, unsigned shift) {
    return (value >> shift) | (value << (64U - shift));
}

std::uint64_t load_be64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8U) | data[i];
    }
    return value;
}

void store_be64(std::uint8_t* data, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        data[i] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8U;
    }
}

void permute(std::array<std::uint64_t, 5>& state, int rounds) {
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

        const std::uint64_t t0 = not state[0] and state[1];
        const std::uint64_t t1 = not state[1] and state[2];
        const std::uint64_t t2 = not state[2] and state[3];
        const std::uint64_t t3 = not state[3] and state[4];
        const std::uint64_t t4 = not state[4] and state[0];

        state[0] ^= t1;
        state[1] ^= t2;
        state[2] ^= t3;
        state[3] ^= t4;
        state[4] ^= t0;

        state[1] ^= state[0];
        state[0] ^= state[4];
        state[3] ^= state[2];
        state[2] = not state[2];

        state[0] ^= rotate_right(state[0], 19) ^ rotate_right(state[0], 28);
        state[1] ^= rotate_right(state[1], 61) ^ rotate_right(state[1], 39);
        state[2] ^= rotate_right(state[2], 1) ^ rotate_right(state[2], 6);
        state[3] ^= rotate_right(state[3], 10) ^ rotate_right(state[3], 17);
        state[4] ^= rotate_right(state[4], 7) ^ rotate_right(state[4], 41);
    }
}

void mac(
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
        not static_cast<std::uint64_t>(tunnel_id),
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

key_type derive_node_key(const key_type& master_key, std::uint16_t tunnel_id) {
    static constexpr std::array<std::uint8_t, 15> label {
        'T', 'U', 'N', 'T', 'O', 'M', '-', 'N', 'O', 'D', 'E', '-', 'K', 'E', 'Y'
    };

    tag_type derived {};
    mac(master_key, tunnel_id, label.data(), label.size(), derived);

    key_type node_key {};
    std::copy(derived.begin(), derived.end(), node_key.begin());
    return node_key;
}

bool constant_time_equal(const std::uint8_t* a, const std::uint8_t* b, std::size_t size) {
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < size; ++i) {
        difference |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    }
    return difference == 0;
}

} // namespace ascon

std::uint64_t load_be64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8U) | data[i];
    }
    return value;
}

void store_be64(std::uint8_t* data, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        data[i] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8U;
    }
}

void store_be16(std::uint8_t* data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    data[1] = static_cast<std::uint8_t>(value & 0xffU);
}

std::uint16_t load_be16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
}

std::uint32_t load_be32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

void store_be32(std::uint8_t* data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    data[1] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    data[2] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    data[3] = static_cast<std::uint8_t>(value & 0xffU);
}

ascon::key_type parse_master_key() {
    const char* secret = std::getenv("TUNTOM_SECRET");
    if (secret == nullptr) {
        throw std::runtime_error("TUNTOM_SECRET is not set");
    }

    const std::string value(secret);
    if (value.size() != ascon::key_size * 2) {
        throw std::runtime_error("TUNTOM_SECRET must contain exactly 32 hex characters");
    }

    ascon::key_type key {};
    for (std::size_t i = 0; i < key.size(); ++i) {
        const std::string byte_string = value.substr(i * 2, 2);
        std::size_t parsed = 0;
        const unsigned long byte = std::stoul(byte_string, &parsed, 16);
        if (parsed != 2 or byte > 0xffUL) {
            throw std::runtime_error("TUNTOM_SECRET contains invalid hex");
        }
        key[i] = static_cast<std::uint8_t>(byte);
    }

    return key;
}

class SequenceGenerator {
public:
    std::uint64_t next() {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        std::uint64_t candidate = static_cast<std::uint64_t>(nanoseconds);

        if (candidate <= last_sequence_) {
            candidate = last_sequence_ + 1;
        }

        last_sequence_ = candidate;
        return candidate;
    }

private:
    std::uint64_t last_sequence_ = 0;
};

class ReplayWindow {
public:
    bool accept(std::uint64_t sequence) {
        if (sequence == 0) {
            return false;
        }

        if (highest_sequence_ == 0) {
            highest_sequence_ = sequence;
            bitmap_ = 1;
            return true;
        }

        if (sequence > highest_sequence_) {
            const std::uint64_t shift = sequence - highest_sequence_;
            if (shift >= 64) {
                bitmap_ = 1;
            } else {
                bitmap_ = (bitmap_ << shift) | 1ULL;
            }

            highest_sequence_ = sequence;
            return true;
        }

        const std::uint64_t distance = highest_sequence_ - sequence;
        if (distance >= 64) {
            return false;
        }

        const std::uint64_t mask = 1ULL << distance;
        if ((bitmap_ & mask) != 0) {
            return false;
        }

        bitmap_ |= mask;
        return true;
    }

private:
    std::uint64_t highest_sequence_ = 0;
    std::uint64_t bitmap_ = 0;
};

class Protocol {
public:
    virtual ~Protocol() = default;

    virtual std::uint8_t version() const = 0;
    virtual std::vector<std::uint8_t> encode(const Packet& packet) const = 0;
    virtual bool decode(const std::uint8_t* data, std::size_t size, Packet& packet) const = 0;
};

class ProtocolV1 final : public Protocol {
public:
    std::uint8_t version() const override {
        return protocol_version_v1;
    }

    std::vector<std::uint8_t> encode(const Packet& packet) const override {
        std::vector<std::uint8_t> output(protocol_header_v1_size + packet.payload.size());

        store_be32(output.data() + 0, protocol_magic);
        store_be16(output.data() + 4, packet.tunnel_id);
        output[6] = protocol_version_v1;
        output[7] = static_cast<std::uint8_t>(packet.type);

        if (not packet.payload.empty()) {
            std::memcpy(output.data() + protocol_header_v1_size, packet.payload.data(), packet.payload.size());
        }

        return output;
    }

    bool decode(const std::uint8_t* data, std::size_t size, Packet& packet) const override {
        if (size < protocol_header_v1_size) {
            return false;
        }

        if (load_be32(data + 0) != protocol_magic or data[6] != protocol_version_v1) {
            return false;
        }

        const auto type = static_cast<PacketType>(data[7]);
        if (type != PacketType::hello and type != PacketType::keepalive and type != PacketType::data) {
            return false;
        }

        packet.type = type;
        packet.tunnel_id = load_be16(data + 4);
        packet.protocol_version = protocol_version_v1;
        packet.sequence = 0;
        packet.payload.assign(data + protocol_header_v1_size, data + size);
        return true;
    }
};

class ProtocolV2 final : public Protocol {
public:
    ProtocolV2(std::uint16_t tunnel_id, const ascon::key_type& master_key)
        : tunnel_id_(tunnel_id),
          node_key_(ascon::derive_node_key(master_key, tunnel_id)) {
    }

    std::uint8_t version() const override {
        return protocol_version_v2;
    }

    std::vector<std::uint8_t> encode(const Packet& packet) const override {
        std::vector<std::uint8_t> output(protocol_header_v2_size + packet.payload.size());

        store_be32(output.data() + 0, protocol_magic);
        store_be16(output.data() + 4, packet.tunnel_id);
        output[6] = protocol_version_v2;
        output[7] = static_cast<std::uint8_t>(packet.type);
        store_be64(output.data() + 8, packet.sequence);

        if (not packet.payload.empty()) {
            std::memcpy(output.data() + protocol_header_v2_size, packet.payload.data(), packet.payload.size());
        }

        std::vector<std::uint8_t> mac_input(16 + packet.payload.size());
        std::memcpy(mac_input.data(), output.data(), 16);
        if (not packet.payload.empty()) {
            std::memcpy(mac_input.data() + 16, packet.payload.data(), packet.payload.size());
        }

        ascon::tag_type tag {};
        ascon::mac(node_key_, tunnel_id_, mac_input.data(), mac_input.size(), tag);
        std::memcpy(output.data() + 16, tag.data(), tag.size());

        std::cerr
            << "ENCODE v2 seq=" << packet.sequence
            << " payload=" << packet.payload.size()
            << " output=" << output.size()
            << "\n";

        return output;
    }

    bool decode(const std::uint8_t* data, std::size_t size, Packet& packet) const override {
        if (size < protocol_header_v2_size) {
            return false;
        }

        if (load_be32(data + 0) != protocol_magic or data[6] != protocol_version_v2) {
            return false;
        }

        const std::uint16_t tunnel_id = load_be16(data + 4);
        if (tunnel_id != tunnel_id_) {
            return false;
        }

        const auto type = static_cast<PacketType>(data[7]);
        if (type != PacketType::hello and type != PacketType::keepalive and type != PacketType::data) {
            return false;
        }

        std::vector<std::uint8_t> mac_input(16 + size - protocol_header_v2_size);
        std::memcpy(mac_input.data(), data, 16);
        if (size > protocol_header_v2_size) {
            std::memcpy(
                mac_input.data() + 16,
                data + protocol_header_v2_size,
                size - protocol_header_v2_size);
        }

        ascon::tag_type expected_tag {};
        ascon::mac(node_key_, tunnel_id_, mac_input.data(), mac_input.size(), expected_tag);

        if (not ascon::constant_time_equal(data + 16, expected_tag.data(), expected_tag.size())) {
            return false;
        }

        packet.type = type;
        packet.tunnel_id = tunnel_id;
        packet.protocol_version = protocol_version_v2;
        packet.sequence = load_be64(data + 8);
        packet.payload.assign(data + protocol_header_v2_size, data + size);
        return true;
    }

private:
    std::uint16_t tunnel_id_ = 0;
    ascon::key_type node_key_ {};
};

class TunDevice {
public:
    explicit TunDevice(const std::string& interface_name) {
        fd_ = ::open("/dev/net/tun", O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            throw std::runtime_error("Cannot open /dev/net/tun: " + std::string(std::strerror(errno)));
        }

        ifreq request {};
        request.ifr_flags = IFF_TUN | IFF_NO_PI;
        std::strncpy(request.ifr_name, interface_name.c_str(), IFNAMSIZ - 1);

        if (::ioctl(fd_, TUNSETIFF, &request) < 0) {
            const std::string error = std::strerror(errno);
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("TUNSETIFF failed: " + error);
        }
    }

    ~TunDevice() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    int fd() const {
        return fd_;
    }

    ssize_t read_packet(std::uint8_t* buffer, std::size_t size) {
        return ::read(fd_, buffer, size);
    }

    ssize_t write_packet(const std::uint8_t* buffer, std::size_t size) {
        return ::write(fd_, buffer, size);
    }

private:
    int fd_ = -1;
};

class UdpEndpoint {
public:
    ~UdpEndpoint() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    void open_server(std::uint16_t port) {
        fd_ = ::socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            throw std::runtime_error("socket() failed: " + std::string(std::strerror(errno)));
        }

        int v6_only = 0;
        ::setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, sizeof(v6_only));

        sockaddr_in6 address {};
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_any;
        address.sin6_port = htons(port);

        if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            throw std::runtime_error("bind() failed: " + std::string(std::strerror(errno)));
        }
    }

    void open_client(const std::string& host, std::uint16_t port) {
        addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;

        addrinfo* result = nullptr;
        const std::string service = std::to_string(port);

        const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
        if (rc != 0) {
            throw std::runtime_error("getaddrinfo() failed: " + std::string(gai_strerror(rc)));
        }

        for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
            const int candidate = ::socket(item->ai_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
            if (candidate < 0) {
                continue;
            }

            fd_ = candidate;
            std::memset(&peer_, 0, sizeof(peer_));
            std::memcpy(&peer_, item->ai_addr, item->ai_addrlen);
            peer_length_ = static_cast<socklen_t>(item->ai_addrlen);
            peer_valid_ = true;
            break;
        }

        ::freeaddrinfo(result);

        if (fd_ < 0 or not peer_valid_) {
            throw std::runtime_error("Cannot create UDP client socket");
        }
    }

    int fd() const {
        return fd_;
    }

    ssize_t receive(
        std::uint8_t* buffer,
        std::size_t size,
        sockaddr_storage& source,
        socklen_t& source_length) {

        source = {};
        source_length = sizeof(source);

        return ::recvfrom(
            fd_,
            buffer,
            size,
            0,
            reinterpret_cast<sockaddr*>(&source),
            &source_length);
    }

    void set_peer(const sockaddr_storage& peer, socklen_t peer_length) {
        peer_ = peer;
        peer_length_ = peer_length;
        peer_valid_ = true;
    }

    ssize_t send(const std::uint8_t* buffer, std::size_t size) {
        if (not peer_valid_) {
            return 0;
        }

        return ::sendto(
            fd_,
            buffer,
            size,
            0,
            reinterpret_cast<const sockaddr*>(&peer_),
            peer_length_);
    }

private:
    int fd_ = -1;
    sockaddr_storage peer_ {};
    socklen_t peer_length_ = 0;
    bool peer_valid_ = false;
};

class Tunnel {
public:
    Tunnel(
        std::uint16_t tunnel_id,
        bool server_mode,
        const std::string& interface_name,
        const std::string& remote_host,
        bool allow_v1)
        : tunnel_id_(tunnel_id),
          server_mode_(server_mode),
          tun_(interface_name),
          protocol_v2_(tunnel_id, parse_master_key()),
          allow_v1_(allow_v1) {

        const std::uint16_t port = static_cast<std::uint16_t>(40000 + tunnel_id_);

        if (server_mode_) {
            udp_.open_server(port);
        } else {
            udp_.open_client(remote_host, port);
        }
    }

    virtual ~Tunnel() = default;

    void run() {
        if (not server_mode_) {
            send_control(PacketType::hello);
        }

        auto last_keepalive = std::chrono::steady_clock::now();
        std::array<std::uint8_t, buffer_size> buffer {};

        while (true) {
            pollfd descriptors[2] {};
            descriptors[0].fd = tun_.fd();
            descriptors[0].events = POLLIN;
            descriptors[1].fd = udp_.fd();
            descriptors[1].events = POLLIN;

            const int rc = ::poll(descriptors, 2, 1000);
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("poll() failed: " + std::string(std::strerror(errno)));
            }

            if ((descriptors[0].revents & POLLIN) != 0) {
                const ssize_t received = tun_.read_packet(buffer.data(), buffer.size());
                if (received > 0) {
                    dump_bytes("TUN read", buffer.data(), static_cast<std::size_t>(received), 20);

                    Packet packet;
                    packet.type = PacketType::data;
                    packet.tunnel_id = tunnel_id_;
                    packet.protocol_version = protocol_version_v2;
                    packet.sequence = sequence_generator_.next();
                    packet.payload.assign(buffer.data(), buffer.data() + received);

                    if (process(packet, Direction::tun_to_udp)) {
                        const auto encoded = protocol_v2_.encode(packet);
                        udp_.send(encoded.data(), encoded.size());
                    }
                }
            }

            if ((descriptors[1].revents & POLLIN) != 0) {
                sockaddr_storage source {};
                socklen_t source_length = 0;

                const ssize_t received = udp_.receive(buffer.data(), buffer.size(), source, source_length);
                if (received > 0) {
                    dump_bytes("UDP recv", buffer.data(), static_cast<std::size_t>(received), 40);

                    Packet packet;
                    const std::uint8_t version = static_cast<std::size_t>(received) > 6 ? buffer[6] : 0;
                    bool decoded = false;

                    if (version == protocol_version_v2) {
                        decoded = protocol_v2_.decode(buffer.data(), static_cast<std::size_t>(received), packet);
                    } else if (version == protocol_version_v1 and allow_v1_) {
                        decoded = protocol_v1_.decode(buffer.data(), static_cast<std::size_t>(received), packet);
                    } else {
                        std::cerr << "DROP protocol version " << static_cast<unsigned>(version) << " not allowed\n";
                        continue;
                    }

                    if (not decoded) {
                        std::cerr << "DROP invalid/auth-failed protocol packet\n";
                        continue;
                    }

                    if (packet.tunnel_id != tunnel_id_) {
                        std::cerr << "DROP tunnel id mismatch\n";
                        continue;
                    }

                    if (packet.protocol_version == protocol_version_v2) {
                        if (not replay_window_.accept(packet.sequence)) {
                            std::cerr << "DROP replay/old seq=" << packet.sequence << "\n";
                            continue;
                        }
                    }

                    // Server learns/updates the NAT peer only after successful authentication.
                    if (server_mode_) {
                        udp_.set_peer(source, source_length);
                    }

                    std::cerr
                        << "ACCEPT v" << static_cast<unsigned>(packet.protocol_version)
                        << " seq=" << packet.sequence
                        << " type=" << static_cast<unsigned>(packet.type)
                        << " payload=" << packet.payload.size()
                        << "\n";

                    if (packet.type == PacketType::data and process(packet, Direction::udp_to_tun)) {
                        dump_bytes("TUN write", packet.payload.data(), packet.payload.size(), 20);
                        tun_.write_packet(packet.payload.data(), packet.payload.size());
                    }
                }
            }

            const auto now = std::chrono::steady_clock::now();
            if (not server_mode_ and now - last_keepalive >= std::chrono::seconds(keepalive_seconds)) {
                send_control(PacketType::keepalive);
                last_keepalive = now;
            }
        }
    }

protected:
    virtual bool process(Packet&, Direction) {
        // Future packet processing hooks (e.g. smithproxy integration) can override this.
        return true;
    }

private:
    void send_control(PacketType type) {
        Packet packet;
        packet.type = type;
        packet.tunnel_id = tunnel_id_;
        packet.protocol_version = protocol_version_v2;
        packet.sequence = sequence_generator_.next();

        const auto encoded = protocol_v2_.encode(packet);
        udp_.send(encoded.data(), encoded.size());
    }

    std::uint16_t tunnel_id_ = 0;
    bool server_mode_ = false;
    TunDevice tun_;
    UdpEndpoint udp_;
    ProtocolV1 protocol_v1_;
    ProtocolV2 protocol_v2_;
    bool allow_v1_ = false;
    SequenceGenerator sequence_generator_;
    ReplayWindow replay_window_;
};

void usage(const char* program_name) {
    std::cerr
        << "Usage:\n"
        << "  " << program_name << " server <id> <ifname> [--allow-v1]\n"
        << "  " << program_name << " client <id> <ifname> <host> [--allow-v1]\n"
        << "\n"
        << "Environment:\n"
        << "  TUNTOM_SECRET   32 hex characters (128-bit master key)\n";
}

std::uint16_t parse_tunnel_id(const char* value) {
    const unsigned long parsed = std::stoul(value);
    if (parsed == 0 or parsed > 255) {
        throw std::runtime_error("Tunnel id must be in range 1..255");
    }

    return static_cast<std::uint16_t>(parsed);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 4) {
            usage(argv[0]);
            return 1;
        }

        const std::string mode = argv[1];
        const std::uint16_t tunnel_id = parse_tunnel_id(argv[2]);
        const std::string interface_name = argv[3];

        if (mode == "server") {
            const bool allow_v1 = argc == 5 and std::string(argv[4]) == "--allow-v1";
            if (argc != 4 and not allow_v1) {
                usage(argv[0]);
                return 1;
            }

            Tunnel tunnel(tunnel_id, true, interface_name, "", allow_v1);
            tunnel.run();
            return 0;
        }

        if (mode == "client") {
            const bool allow_v1 = argc == 6 and std::string(argv[5]) == "--allow-v1";
            if ((argc != 5 and not allow_v1)) {
                usage(argv[0]);
                return 1;
            }

            Tunnel tunnel(tunnel_id, false, interface_name, argv[4], allow_v1);
            tunnel.run();
            return 0;
        }

        usage(argv[0]);
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << "\n";
        return 1;
    }
}
