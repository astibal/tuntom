#pragma once

// Feasibility experiment only. Production IPC and rules remain unchanged.
#include "ipc/switch_protocol.hpp"
#include "ip_flow.hpp"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace divert_lab {
using Labels = std::vector<std::uint64_t>;

inline std::uint64_t text_label(const std::string& text) {
    if (text.size() > 8) throw std::runtime_error("label string exceeds 8 bytes");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
        value |= std::uint64_t(static_cast<unsigned char>(text[i])) << (56 - 8 * i);
    return value;
}

inline std::string pretty_label(std::uint64_t value) {
    if (!value) return "0";
    std::string result = "\"";
    bool ended = false;
    for (unsigned i = 0; i < 8; ++i) {
        const auto c = static_cast<unsigned char>(value >> (56 - 8 * i));
        if (!c) { ended = true; continue; }
        if (ended || c < 32 || c > 126) return std::to_string(value);
        if (c == '\\' || c == '"') result += '\\';
        result += static_cast<char>(c);
    }
    return result + '"';
}

inline std::string pretty_stack(const Labels& labels) {
    std::string out = "[";
    for (auto value : labels) {
        if (out.size() > 1) out += ',';
        out += pretty_label(value);
    }
    return out + ']';
}

enum Action : std::uint64_t { offered = 0, onward = 1, to_client = 2, bypass = 3 };

struct Envelope {
    Labels base;
    // Body: stable origin port ID, action, complete ORIGINAL label stack.
    Labels body;
    bool present = false;
    std::uint64_t origin() const { return body.at(0); }
    Action action() const { return static_cast<Action>(body.at(1)); }
    Labels original() const { return Labels(body.begin() + 2, body.end()); }
};

class Codec {
    std::uint64_t prefix_;
public:
    explicit Codec(const std::string& cookie) : prefix_(text_label(cookie)) {
        if (cookie.size() != 3) throw std::runtime_error("cookie needs three ASCII letters");
        for (char c : cookie)
            if (!(('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z')))
                throw std::runtime_error("cookie needs three ASCII letters");
    }
    Labels attach(const Labels& base, const Labels& body) const {
        if (body.size() < 3 || body.size() > 78 || body[1] > bypass)
            throw std::runtime_error("invalid divert body");
        if (base.empty() || base.size() + body.size() + 2 > tuntom::switch_max_labels)
            throw std::runtime_error("divert envelope exceeds production IPC label limit");
        Labels out = base;
        out.push_back(prefix_ | (std::uint64_t(body.size() + 48) << 32));
        out.push_back(text_label("DVRT"));
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }
    Labels offer(const Labels& base, std::uint64_t origin) const {
        Labels body{origin, offered};
        body.insert(body.end(), base.begin(), base.end());
        return attach(base, body);
    }
    Envelope split(const Labels& labels) const {
        Envelope result;
        for (std::size_t i = 0; i < labels.size();) {
            const auto v = labels[i];
            const bool marker = (v & 0xffffff0000000000ULL) == prefix_ &&
                (v & 0xffffffffULL) == 0 && i + 1 < labels.size() &&
                labels[i + 1] == text_label("DVRT");
            if (!marker) { result.base.push_back(v); ++i; continue; }
            const auto length_char = static_cast<unsigned>((v >> 32) & 255);
            if (result.present || length_char < 48 || length_char > 126)
                throw std::runtime_error("ambiguous or invalid divert marker");
            const auto n = length_char - 48;
            if (n < 3 || n > labels.size() - i - 2)
                throw std::runtime_error("truncated divert body");
            result.body.assign(labels.begin() + i + 2, labels.begin() + i + 2 + n);
            if (!result.origin() || result.body[1] > bypass)
                throw std::runtime_error("invalid origin or divert action");
            result.present = true;
            i += n + 2;
        }
        if (result.base.empty()) throw std::runtime_error("missing normal label stack");
        return result;
    }
};

inline Labels labels_of(const tuntom::SwitchFrameView& frame) {
    Labels labels;
    for (std::size_t i = 0; i < frame.label_count; ++i) labels.push_back(frame.label(i));
    return labels;
}

struct PacketInfo {
    tuntom::FlowKey flow;
    unsigned flags = 0, ttl = 0;
    std::uint32_t seq = 0;
};

// The routed lab deliberately exercises complete IPv4 TCP/UDP packets only.
inline bool packet_info(const std::uint8_t* data, std::size_t size, PacketInfo& out) {
    tuntom::ParsedIpFlow flow;
    if (!tuntom::parse_ip_flow(data, size, flow) || flow.l3.version != 4 ||
        !flow.has_l4 || flow.fragmented) return false;
    const auto offset = std::size_t(data[0] & 15) * 4;
    const auto end = tuntom::load_be16(data + 2);
    if (flow.protocol == 6) {
        if (offset + 20 > end) return false;
        const auto tcp_size = std::size_t(data[offset + 12] >> 4) * 4;
        if (tcp_size < 20 || offset + tcp_size > end) return false;
        out.flags = data[offset + 13];
        out.seq = tuntom::load_be32(data + offset + 4);
    } else if (flow.protocol == 17) {
        if (offset + 8 > end) return false;
    } else return false;
    out.flow = flow.l4;
    out.ttl = data[8];
    return true;
}

inline tuntom::FlowKey canonical(tuntom::FlowKey key) {
    auto reversed = tuntom::reverse_key(key);
    if (reversed.source < key.source ||
        (reversed.source == key.source && reversed.source_port < key.source_port)) return reversed;
    return key;
}

class Admission {
    std::unordered_set<tuntom::FlowKey, tuntom::FlowHash> existing_tcp_, diverted_tcp_, existing_udp_;
    bool tcp_positive_released_ = false, tcp_negative_released_ = false, udp_released_ = false;
    static constexpr std::size_t limit = 100000;
    template<class Set> void release(Set& set, bool& released) {
        if (!released) { Set{}.swap(set); released = true; }
    }
    template<class Set> void remember(Set& set, const tuntom::FlowKey& key) {
        if (set.count(key)) return;
        if (set.size() == limit) throw std::runtime_error("lab admission capacity reached; no eviction");
        set.insert(key);
    }
public:
    bool proxy(const PacketInfo& packet, double seconds) {
        const auto key = canonical(packet.flow);
        if (packet.flow.protocol == 6) {
            if (seconds >= 3600) release(diverted_tcp_, tcp_positive_released_);
            if (seconds >= 86400) { release(existing_tcp_, tcp_negative_released_); return true; }
            if (existing_tcp_.count(key)) return false;
            if (seconds >= 3600 || diverted_tcp_.count(key)) return true;
            if ((packet.flags & 0x12) == 0x02) { remember(diverted_tcp_, key); return true; }
            remember(existing_tcp_, key);
            return false;
        }
        if (seconds >= 3600) { release(existing_udp_, udp_released_); return true; }
        if (seconds < 60) { remember(existing_udp_, key); return false; }
        return !existing_udp_.count(key);
    }
};

inline std::string quote(const std::string& text) {
    std::ostringstream out;
    out << std::quoted(text);
    return out.str();
}

class Trace {
    std::ofstream out_;
public:
    explicit Trace(const std::string& path) : out_(path) {
        if (!out_) throw std::runtime_error("cannot open trace");
    }
    void event(const std::string& event, const std::string& from, const std::string& to,
               const Labels& labels = {}, const std::uint8_t* data = nullptr, std::size_t size = 0,
               const std::string& logical = "") {
        out_ << "{\"event\":" << quote(event) << ",\"from\":" << quote(from)
             << ",\"to\":" << quote(to) << ",\"logical\":" << quote(logical)
             << ",\"stack\":" << quote(pretty_stack(labels)) << ",\"labels\":[";
        for (std::size_t i = 0; i < labels.size(); ++i) {
            if (i) out_ << ',';
            out_ << labels[i];
        }
        out_ << ']';
        PacketInfo packet;
        if (data && packet_info(data, size, packet))
            out_ << ",\"sport\":" << packet.flow.source_port << ",\"dport\":" << packet.flow.destination_port
                 << ",\"flags\":" << packet.flags << ",\"seq\":" << packet.seq << ",\"ttl\":" << packet.ttl;
        out_ << "}\n";
        out_.flush();
    }
};
} // namespace divert_lab
