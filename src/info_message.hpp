#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

namespace tuntom::info {
// INFO contains ASCII key=value lines. Values remain opaque text.
// Bound parsing and stats expansion (including one peer_info_ prefix per key).
inline constexpr std::size_t max_payload = 4096;
using Addresses = std::vector<std::uint32_t>; // IPv4, host byte order.

inline Addresses loopback_addresses(const ifaddrs* interfaces) {
    Addresses result;
    for (auto* item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || !(item->ifa_flags & IFF_LOOPBACK) ||
            item->ifa_addr->sa_family != AF_INET) continue;
        const auto address = ntohl(reinterpret_cast<const sockaddr_in*>(item->ifa_addr)->sin_addr.s_addr);
        if ((address >> 24) != 127) result.push_back(address);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

inline Addresses loopback_addresses() {
    ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0) throw std::runtime_error("INFO getifaddrs failed");
    std::unique_ptr<ifaddrs, decltype(&::freeifaddrs)> owner(interfaces, ::freeifaddrs);
    return loopback_addresses(interfaces);
}

using Fields = std::map<std::string, std::string>;

inline bool valid_key(std::string_view key) {
    if (key.empty() || key.front() < 'a' || key.front() > 'z') return false;
    for (const auto c : key)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

inline std::string_view trim_value(std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

// Decode transactionally: any error leaves the previous snapshot intact.
inline bool decode(const std::vector<std::uint8_t>& payload, Fields& fields) {
    if (payload.empty() || payload.size() > max_payload) return false;
    for (const auto c : payload)
        if ((c < 0x20 || c > 0x7e) && c != '\t' && c != '\n') return false;
    const std::string_view text(reinterpret_cast<const char*>(payload.data()), payload.size());
    Fields result;
    for (std::size_t at = 0; at < text.size();) {
        const auto end = text.find('\n', at);
        const auto line = text.substr(at, end == std::string_view::npos ? text.size() - at : end - at);
        const auto equal = line.find('=');
        if (equal == std::string_view::npos) return false;
        const auto key = line.substr(0, equal);
        if (!valid_key(key) || !result.emplace(key, trim_value(line.substr(equal + 1))).second) return false;
        if (end == std::string_view::npos) break;
        at = end + 1;
    }
    fields = std::move(result);
    return true;
}

inline std::vector<std::uint8_t> encode(const Fields& fields) {
    if (fields.empty()) throw std::runtime_error("INFO requires at least one field");
    std::string text;
    for (const auto& field : fields) {
        if (!valid_key(field.first)) throw std::runtime_error("Invalid INFO key");
        std::string value = field.second;
        for (auto& c : value) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte >= 0x80) c = '?'; // One replacement per byte, no Unicode decoding.
            else if ((byte < 0x20 || byte == 0x7f) && c != '\t')
                throw std::runtime_error("Invalid INFO value");
        }
        const auto trimmed = trim_value(value);
        if (text.size() + field.first.size() + trimmed.size() + 2 > max_payload)
            throw std::runtime_error("INFO exceeds payload limit");
        text += field.first;
        text += '=';
        text.append(trimmed.data() ? trimmed.data() : "", trimmed.size());
        text += '\n';
    }
    return {text.begin(), text.end()};
}

inline std::vector<std::uint8_t> encode_access(const Addresses& addresses, Fields fields = {}) {
    std::string value;
    for (const auto address : addresses) {
        if (!value.empty()) value += ',';
        value += std::to_string(address >> 24) + "." + std::to_string((address >> 16) & 255) +
                 "." + std::to_string((address >> 8) & 255) + "." + std::to_string(address & 255);
    }
    if (!fields.emplace("access", std::move(value)).second)
        throw std::runtime_error("INFO access is supplied automatically");
    return encode(fields);
}

class PeerSnapshot {
public:
    void activated(std::uint64_t exchange) {
        // INFO can precede CONFIRM_ACK on the client; preserve that snapshot.
        if (exchange_ == exchange) return;
        fields_.clear();
        received_ = false;
        sequence_ = 0;
        exchange_ = exchange;
    }

    // Caller authenticates and rejects old sessions before delivering INFO here.
    bool accept(std::uint64_t exchange, std::uint64_t sequence,
                const std::vector<std::uint8_t>& payload) {
        if (exchange == exchange_ && sequence <= sequence_) return false;
        Fields fields;
        if (!decode(payload, fields)) return false;
        fields_ = std::move(fields);
        received_ = true;
        exchange_ = exchange;
        sequence_ = sequence;
        return true;
    }

    void write_stats(std::ostream& output) const {
        // Operational fields live outside peer_info_, which belongs to the peer.
        output << "info_msg_peer_received=" << (received_ ? 1 : 0) << "\n";
        for (const auto& field : fields_)
            output << "peer_info_" << field.first << "=" << field.second << "\n";
    }

private:
    Fields fields_;
    bool received_ = false;
    std::uint64_t exchange_ = 0, sequence_ = 0;
};
} // namespace tuntom::info
