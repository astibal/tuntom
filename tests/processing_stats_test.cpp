#include "../src/processing_stats.hpp"
#include "../src/reassembly.hpp"
#include <sstream>
#include <stdexcept>

static void require(bool ok) {
    if (!ok) throw std::runtime_error("processing statistics check failed");
}

int main() {
    tuntom::ProcessingStats stats;
    std::ostringstream empty;
    stats.write(empty, "tx");
    require(empty.str().find("tx_p99_us=0.000\n") != std::string::npos);
    for (unsigned i = 0; i <= 2048; ++i) {
        require(stats.select() == (i % 1024 == 0));
    }
    // Evict an outlier from the percentile window, retaining lifetime max/mean.
    stats.record(10000);
    for (unsigned i = 1; i <= 4096; ++i) stats.record(i);
    std::ostringstream output;
    stats.write(output, "tx");
    const auto text = output.str();
    require(text.find("tx_samples=4097\n") != std::string::npos);
    require(text.find("tx_window_samples=4096\n") != std::string::npos);
    require(text.find("tx_max_us=10000.000\n") != std::string::npos);
    require(text.find("tx_avg_us=2050.441\n") != std::string::npos);
    require(text.find("tx_p95_us=3892.000\n") != std::string::npos);
    require(text.find("tx_p99_us=4056.000\n") != std::string::npos);

    tuntom::Reassembler reassembler(1500);
    tuntom::ProcessingStats spans;
    tuntom::Packet packet;
    packet.message_id = 1;
    packet.original_length = 4;
    packet.fragment_offset = 2;
    packet.payload = {3, 4};
    std::vector<std::uint8_t> complete;
    require(!reassembler.accept(packet, complete, &spans));
    require(!reassembler.accept(packet, complete, &spans)); // duplicate
    packet.fragment_offset = 0;
    packet.payload = {1, 2};
    require(reassembler.accept(packet, complete, &spans));
    require(complete == std::vector<std::uint8_t>({1, 2, 3, 4}));
    packet.message_id = 2;
    packet.payload = {1, 2, 3, 4};
    require(reassembler.accept(packet, complete, &spans)); // no span sample
    std::ostringstream span_output;
    spans.write(span_output, "span");
    require(span_output.str().find("span_samples=1\n") != std::string::npos);
}
