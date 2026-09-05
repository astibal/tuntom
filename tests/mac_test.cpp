#define main tuntom_program_main
#include "../udp_tun.cpp"
#undef main
#include <random>

void require(bool ok, const char* message) {
    if (not ok) throw std::runtime_error(message);
}

// Independent, slow lookup implementation of Ascon v1.2 section 2.6,
// Tables 4/5 (x0 is the S-box MSB). Test-only: not constant time.
// https://ascon.isec.tugraz.at/files/asconv12-nist.pdf
using State = std::array<std::uint64_t, 5>;
State reference_permute(State state, int rounds) {
    constexpr unsigned sbox[32] = {
        4, 11, 31, 20, 26, 21, 9, 2, 27, 5, 8, 18, 29, 3, 6, 28,
        30, 19, 7, 14, 0, 13, 17, 24, 16, 12, 1, 25, 22, 10, 15, 23
    };
    constexpr unsigned rotations[5][2] = {{19,28},{61,39},{1,6},{10,17},{7,41}};
    for (int r = 12 - rounds; r < 12; ++r) {
        state[2] ^= static_cast<std::uint64_t>(((15 - r) << 4) | r);
        State substituted {};
        for (unsigned bit = 0; bit < 64; ++bit) {
            unsigned input = 0;
            for (unsigned word = 0; word < 5; ++word) {
                input = (input << 1) | static_cast<unsigned>((state[word] >> bit) & 1);
            }
            for (unsigned word = 0; word < 5; ++word) {
                substituted[word] |= std::uint64_t((sbox[input] >> (4 - word)) & 1) << bit;
            }
        }
        for (unsigned word = 0; word < 5; ++word) {
            const auto value = substituted[word];
            state[word] = value;
            for (auto rotation : rotations[word]) {
                state[word] ^= (value >> rotation) | (value << (64 - rotation));
            }
        }
    }
    return state;
}

void test_permutation() {
    std::mt19937_64 random(42);
    for (int rounds : {8, 12}) {
        for (int trial = 0; trial < 1002; ++trial) {
            State input {};
            for (auto& word : input) {
                word = trial == 0 ? 0 : trial == 1 ? ~std::uint64_t(0) : random();
            }
            auto actual = input;
            ascon::permute(actual, rounds);
            require(actual == reference_permute(input, rounds), "permutation reference mismatch");
        }
    }
}

void test_mac_vectors() {
    // Project-specific MAC vectors, generated independently using the
    // specification's lookup S-box. These are NOT standardized Ascon-Mac KATs.
    const std::pair<std::size_t, const char*> vectors[] = {
        {0, "2aad841a9aa2adc884208ea8f62a8a87"},
        {1, "683e70577abb233f972101110d599890"},
        {7, "9560aa1bbe356a3a4b6fb983023354e3"},
        {8, "bf5b71e039cf24a2bce494d490da4235"},
        {9, "635187af2fce91c92095af5548a6e9d1"},
        {15, "69b4260c66d4a7281f320018a3e63c67"},
        {16, "8893da72cf4c75d71999fd6c816eb9e2"},
        {17, "8f3090050ecbc089bdc5d0c3661981e6"},
        {64, "67b1cfc1b8b6d600e5190b5b787eec34"},
    };
    ascon::key_type key {};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i);
    for (const auto& vector : vectors) {
        std::vector<std::uint8_t> data(vector.first);
        for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i);
        ascon::tag_type tag {};
        ascon::mac(key, 42, data.data(), data.size(), tag);
        for (std::size_t i = 0; i < tag.size(); ++i) {
            const auto expected = std::stoul(std::string(vector.second + i * 2, 2), nullptr, 16);
            require(tag[i] == expected, "MAC vector mismatch");
        }
    }
}

void test_protocol(const Protocol& protocol, const Protocol& wrong_key,
                   const Protocol& wrong_tunnel) {
    Packet packet;
    packet.tunnel_id = 42;
    packet.sequence = 1'000'000'000;
    packet.message_id = 1234;
    packet.original_length = 64;
    packet.payload.resize(64, 0x45);
    auto encoded = protocol.encode(packet);
    Packet decoded;
    require(protocol.decode(encoded.data(), encoded.size(), decoded), "valid packet rejected");
    require(decoded.payload == packet.payload, "payload mismatch");
    require(not wrong_key.decode(encoded.data(), encoded.size(), decoded), "wrong key accepted");
    require(not wrong_tunnel.decode(encoded.data(), encoded.size(), decoded), "wrong tunnel accepted");
    for (std::size_t byte = 0; byte < encoded.size(); ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            auto changed = encoded;
            changed[byte] ^= static_cast<std::uint8_t>(1U << bit);
            require(not protocol.decode(changed.data(), changed.size(), decoded), "bit flip accepted");
        }
    }
    // Regression for the previous boolean-operator bug: combine three
    // valid datagrams into a fourth, without using a key to construct it.
    for (int trial = 1; trial <= 100; ++trial) {
        std::array<std::vector<std::uint8_t>, 3> inputs;
        for (unsigned i = 0; i < inputs.size(); ++i) {
            auto variant = packet;
            variant.sequence += i * 1024;
            if (i & 1) variant.payload[1] ^= static_cast<std::uint8_t>(trial);
            if (i & 2) variant.payload[2] ^= static_cast<std::uint8_t>(trial);
            inputs[i] = protocol.encode(variant);
        }
        auto forged = inputs[0];
        for (std::size_t i = 0; i < forged.size(); ++i) {
            forged[i] = static_cast<std::uint8_t>(forged[i] ^ inputs[1][i] ^ inputs[2][i]);
        }
        require(not protocol.decode(forged.data(), forged.size(), decoded), "XOR forgery accepted");
    }
}

int main() {
    log_level = LogLevel::quiet;
    test_permutation();
    test_mac_vectors();
    ascon::key_type key {}, other {};
    other[0] = 1;
    test_protocol(ProtocolV2(42, key), ProtocolV2(42, other), ProtocolV2(43, key));
    test_protocol(ProtocolV3(42, key), ProtocolV3(42, other), ProtocolV3(43, key));
    std::cout << "PASS: reference permutations, MAC vectors, v2/v3 tampering and XOR forgery rejection\n";
}
