// Build with -O3 -march=native; reports codec throughput, not network throughput.
#include "../src/protocol.hpp"
#include <chrono>
#include <iostream>
int main() {
    using namespace tuntom;
    log_level = LogLevel::quiet;
    ascon::key_type key {};
    for (std::size_t size : {64, 512, 1400, 9000}) {
        for (bool encrypted : {false, true}) {
            ProtocolV5 codec(42, key, key, encrypted);
            Packet packet, decoded;
            packet.tunnel_id = 42; packet.message_id = 1;
            packet.original_length = static_cast<std::uint32_t>(size);
            packet.payload.resize(size, 42);
            std::vector<std::uint8_t> wire, scratch;
            constexpr unsigned iterations = 20000;
            auto start = std::chrono::steady_clock::now();
            unsigned checksum = 0;
            for (unsigned i = 0; i < iterations; ++i) {
                packet.sequence = i + 1;
                codec.encode_into(packet, wire, scratch);
                checksum += wire[32];
            }
            auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::cout << size << " B " << (encrypted ? "AEAD" : "AMAC")
                      << " encode " << size * iterations / elapsed / 1e6 << " MB/s";
            start = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < iterations; ++i) {
                if (!codec.decode_with_scratch(wire.data(), wire.size(), decoded, scratch)) return 1;
                checksum += decoded.payload[i % size];
            }
            elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::cout << " decode " << size * iterations / elapsed / 1e6 << " MB/s (" << checksum << ")\n";
        }
    }
}
