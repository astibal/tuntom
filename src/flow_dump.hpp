#pragma once

#include "control_protocol.hpp"
#include "ip_flow.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <unistd.h>

namespace tuntom {
// A read-only snapshot of retained state, not an additional packet tracker.
class FlowDump {
    std::ostringstream out_;
    std::size_t count_ = 0;
public:
    explicit FlowDump(const char* tracking = "retained") {
        out_.exceptions(std::ios::badbit | std::ios::failbit);
        out_ << "format=txt\nformat_version=1\nview=flows\npid=" << ::getpid()
             << "\ntracking=" << tracking << '\n';
    }
    static std::string address(const IpPairKey& key, bool source) {
        char text[INET6_ADDRSTRLEN]{};
        if (!::inet_ntop(key.version == 4 ? AF_INET : AF_INET6,
                         (source ? key.source : key.destination).data(), text, sizeof(text)))
            throw std::runtime_error("cannot format flow address");
        return text;
    }
    static void key(std::ostream& out, const IpPairKey& value) {
        out << " ip_version=" << unsigned(value.version) << " src=" << address(value, true)
            << " dst=" << address(value, false);
    }
    static void key(std::ostream& out, const FlowKey& value) {
        key(out, static_cast<const IpPairKey&>(value));
        out << " protocol=" << unsigned(value.protocol) << " src_port=" << value.source_port
            << " dst_port=" << value.destination_port;
    }
    static void labels(std::ostream& out, const std::uint64_t* values, std::size_t size) {
        out << '[';
        for (std::size_t i = 0; i < size; ++i) {
            if (i) out << ',';
            out << "0x" << std::hex << std::setw(16) << std::setfill('0') << values[i] << std::dec;
        }
        out << ']';
    }
    template<class Key, class Details> void add(const char* table, const Key& value, Details details) {
        out_ << "flow table=" << table;
        key(out_, value);
        details(out_);
        out_ << '\n';
        ++count_;
        if (out_.tellp() > static_cast<std::streamoff>(control_max_flows - 128))
            throw std::runtime_error("flow snapshot exceeds 256 MiB; no partial dump returned");
    }
    template<class Duration> static void idle(std::ostream& out, Duration duration) {
        out << " idle_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    }
    std::string finish() {
        out_ << "flow_count=" << count_ << '\n';
        return out_.str();
    }
};
} // namespace tuntom
