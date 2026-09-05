// No TUN device, root privileges, or network access needed.
#define main tuntom_program_main
#include "../udp_tun.cpp"
#undef main

#include <limits>
#include <random>

void require(bool condition, const char* message) {
    if (not condition) {
        throw std::runtime_error(message);
    }
}

void test_reordering_and_duplicates() {
    ReplayWindow window;
    require(not window.accept(0), "zero sequence accepted");
    require(window.accept(1'000'003'000), "first packet rejected");
    require(window.accept(1'000'001'000), "reordered packet rejected");
    require(window.accept(1'000'002'000), "second reordered packet rejected");
    require(not window.accept(1'000'001'000), "duplicate accepted");
    require(not window.accept(1'000'003'000), "highest duplicate accepted");
}

void test_full_window() {
    std::mt19937 random(42);
    for (int trial = 0; trial < 100; ++trial) {
        ReplayWindow window;
        std::array<std::uint64_t, 64> timestamps {};
        for (std::size_t i = 0; i < timestamps.size(); ++i) {
            timestamps[i] = 1'000'000'000 + (i + 1) * 10'000;
        }
        std::shuffle(timestamps.begin(), timestamps.end(), random);
        for (auto sequence : timestamps) {
            require(window.accept(sequence), "shuffled packet rejected");
        }
        for (auto sequence : timestamps) {
            require(not window.accept(sequence), "retained duplicate accepted");
        }
        require(not window.accept(1'000'000'001), "packet below full window accepted");
        require(window.accept(2'000'000'000), "newer sender timestamp rejected");
        for (auto sequence : timestamps) {
            require(not window.accept(sequence), "duplicate accepted after eviction");
        }
        // An unseen value inside a full, sparse timestamp window is valid.
        require(window.accept(1'000'635'000), "gap inside full window rejected");
        require(not window.accept(1'000'635'000), "gap duplicate accepted");
    }
}

void test_integer_boundaries() {
    ReplayWindow window;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    require(window.accept(maximum), "maximum sequence rejected");
    require(window.accept(1), "unseen early sequence rejected before window filled");
    require(not window.accept(maximum), "maximum duplicate accepted");
    for (std::uint64_t i = 1; i < 64; ++i) {
        require(window.accept(maximum - i), "high reordered sequence rejected");
    }
    require(not window.accept(maximum - 64), "65th older sequence accepted");
    require(not window.accept(1), "evicted early duplicate accepted");
    require(not window.accept(0), "zero accepted after window filled");
}

void test_fragment_receive_path() {
    ascon::key_type key {};
    ProtocolV4 sender(42, key, false);
    ProtocolV4 receiver(42, key, true);
    ReplayWindow replay;
    Reassembler reassembler(9000);
    std::vector<std::uint8_t> original(9000);
    for (std::size_t i = 0; i < original.size(); ++i) {
        original[i] = static_cast<std::uint8_t>(i);
    }
    const auto plan = make_fragment_plan(original.size(), 141);
    require(plan.count == 64, "test must exercise 64 fragments");
    std::vector<std::vector<std::uint8_t>> datagrams;
    std::size_t offset = 0;
    for (std::size_t i = 0; i < plan.count; ++i) {
        Packet fragment;
        fragment.type = PacketType::data;
        fragment.tunnel_id = 42;
        fragment.sequence = 1'000'000'000 + i * 10'000;
        fragment.message_id = 1234;
        fragment.original_length = static_cast<std::uint32_t>(original.size());
        fragment.fragment_offset = static_cast<std::uint32_t>(offset);
        const auto size = plan.base_size + (i < plan.larger_fragments ? 1 : 0);
        fragment.payload.assign(original.begin() + offset, original.begin() + offset + size);
        datagrams.push_back(sender.encode(fragment));
        offset += size;
    }
    std::reverse(datagrams.begin(), datagrams.end());
    std::vector<std::uint8_t> complete;
    std::size_t completions = 0;
    for (const auto& datagram : datagrams) {
        Packet decoded;
        require(receiver.decode(datagram.data(), datagram.size(), decoded), "decode failed");
        require(replay.accept(decoded.sequence), "reordered fragment rejected");
        require(not replay.accept(decoded.sequence), "duplicate fragment accepted");
        if (reassembler.accept(decoded, complete)) {
            ++completions;
            require(complete == original, "reassembled data differs");
        }
    }
    require(completions == 1, "packet must be delivered exactly once");
}

int main() {
    log_level = LogLevel::quiet;
    test_reordering_and_duplicates();
    test_full_window();
    test_integer_boundaries();
    test_fragment_receive_path();
    std::cout << "PASS: replay ordering, duplicates, eviction, integer boundaries, "
                 "and 64-fragment receive path\n";
}
