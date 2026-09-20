#pragma once

#include "ip_flow.hpp"
#include "switch_ruleset.hpp"
#include <arpa/inet.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace tuntom {

struct ClassifierPrefix {
    std::uint8_t version = 0;
    unsigned bits = 0;
    std::array<std::uint8_t, 16> address{};

    bool matches(std::uint8_t family, const std::array<std::uint8_t, 16>& value) const {
        if (!version) return true;
        if (version != family) return false;
        const auto bytes = bits / 8;
        if (std::memcmp(address.data(), value.data(), bytes) != 0) return false;
        const unsigned remaining = bits % 8;
        return remaining == 0 || ((address[bytes] ^ value[bytes]) & (0xffU << (8 - remaining))) == 0;
    }

    static ClassifierPrefix parse(const std::string& text) {
        ClassifierPrefix prefix;
        if (text == "*") return prefix;
        const auto slash = text.find('/');
        const auto ip = text.substr(0, slash);
        if (::inet_pton(AF_INET, ip.c_str(), prefix.address.data()) == 1) prefix.version = 4;
        else if (::inet_pton(AF_INET6, ip.c_str(), prefix.address.data()) == 1) prefix.version = 6;
        else throw std::runtime_error("expected an IP address or CIDR prefix");
        const auto bits = slash == std::string::npos ? (prefix.version == 4 ? 32 : 128) :
                          rules_number(text.substr(slash + 1));
        if (bits > (prefix.version == 4 ? 32U : 128U)) throw std::runtime_error("invalid CIDR prefix length");
        prefix.bits = static_cast<unsigned>(bits);
        return prefix;
    }
};

struct ClassifierPort {
    bool specified = false;
    std::uint16_t first = 0, last = 65535;
    bool matches(std::uint16_t value) const { return value >= first && value <= last; }
    static ClassifierPort parse(RulesLine& line) {
        ClassifierPort port; port.specified = true;
        if (line.eat("*")) return port;
        const bool range = line.eat("<");
        const auto first = rules_number(line.take());
        auto last = first;
        if (range) { line.need(","); last = rules_number(line.take()); line.need(">"); }
        if (first > last || last > 65535) throw std::runtime_error("port must be in 0..65535 with ordered range bounds");
        port.first = static_cast<std::uint16_t>(first); port.last = static_cast<std::uint16_t>(last);
        return port;
    }
};

struct ClassifierRule {
    unsigned version = 0;
    int protocol = -1;
    ClassifierPrefix source, destination;
    ClassifierPort source_port, destination_port;
    std::vector<std::uint64_t> labels;

    bool matches(const ParsedIpFlow& flow) const {
        if (version && version != flow.l3.version) return false;
        if (!source.matches(flow.l3.version, flow.l3.source) ||
            !destination.matches(flow.l3.version, flow.l3.destination)) return false;
        if (protocol >= 0 && (!flow.has_protocol || protocol != flow.protocol)) return false;
        if (source_port.specified || destination_port.specified)
            return flow.has_l4 && source_port.matches(flow.l4.source_port) &&
                   destination_port.matches(flow.l4.destination_port);
        return true;
    }
};

// Ordered, stateless assignment of an initial switch label stack. No packet or
// flow is retained. The returned stack belongs to this immutable configuration.
class PacketClassifier {
    std::vector<ClassifierRule> rules_;
    bool enabled_ = false;
    std::uint64_t hits_ = 0, misses_ = 0, parse_errors_ = 0;
public:
    static PacketClassifier parse(const std::string& text) {
        if (text.size() > ruleset_max_bytes) throw std::runtime_error("classifier exceeds 1 MiB");
        PacketClassifier result; result.enabled_ = true;
        std::istringstream input(text); std::string text_line;
        std::size_t number = 0; bool header = false;
        while (std::getline(input, text_line)) {
            ++number;
            try {
                RulesLine line(text_line, true);
                if (line.done()) continue;
                if (!header) {
                    line.need("format");
                    if (rules_number(line.take()) != 1) throw std::runtime_error("unsupported classifier format");
                    header = true;
                } else {
                    line.need("classify");
                    ClassifierRule rule;
                    std::unordered_set<std::string> fields;
                    while (line.peek() != "to") {
                        const auto field = line.take();
                        const auto key = field == "ip4" || field == "ip6" ? "family" : field;
                        if (!fields.insert(key).second) throw std::runtime_error("duplicate classifier field: " + field);
                        if (field == "ip4" || field == "ip6") rule.version = field == "ip4" ? 4 : 6;
                        else if (field == "src") rule.source = ClassifierPrefix::parse(line.take());
                        else if (field == "dst") rule.destination = ClassifierPrefix::parse(line.take());
                        else if (field == "sport") rule.source_port = ClassifierPort::parse(line);
                        else if (field == "dport") rule.destination_port = ClassifierPort::parse(line);
                        else if (field == "proto") {
                            const auto value = line.take();
                            if (value == "*") rule.protocol = -1;
                            else if (value == "tcp") rule.protocol = 6;
                            else if (value == "udp") rule.protocol = 17;
                            else if (value == "icmp") rule.protocol = 1;
                            else if (value == "icmp6") rule.protocol = 58;
                            else {
                                const auto protocol = rules_number(value);
                                if (protocol > 255) throw std::runtime_error("IP protocol must be in 0..255");
                                rule.protocol = static_cast<int>(protocol);
                            }
                        } else throw std::runtime_error("unknown classifier field: " + field);
                    }
                    if ((rule.source.version && rule.destination.version && rule.source.version != rule.destination.version) ||
                        (rule.version && ((rule.source.version && rule.source.version != rule.version) ||
                                          (rule.destination.version && rule.destination.version != rule.version))))
                        throw std::runtime_error("conflicting IP families");
                    if ((rule.source_port.specified || rule.destination_port.specified) &&
                        rule.protocol >= 0 && rule.protocol != 6 && rule.protocol != 17)
                        throw std::runtime_error("port selectors require TCP/UDP or an omitted protocol");
                    line.need("to");
                    const auto stack = line.stack_match();
                    if (stack.rest) throw std::runtime_error("classifier output requires a complete literal stack");
                    for (const auto& element : stack.items) {
                        if (element.kind != RuleElement::Kind::exact)
                            throw std::runtime_error("classifier output requires literal labels");
                        rule.labels.push_back(element.value);
                    }
                    if (ruleset_max_statements == result.rules_.size()) throw std::runtime_error("maximum 4096 classifier rules");
                    result.rules_.push_back(std::move(rule));
                }
                if (!line.done()) throw std::runtime_error("unexpected token: " + line.take());
            } catch (const std::runtime_error& e) {
                throw std::runtime_error("classifier line " + std::to_string(number) + ": " + e.what());
            }
        }
        if (!header) throw std::runtime_error("classifier format header is required");
        return result;
    }
    static PacketClassifier from_file(const std::string& path) {
        return path.empty() ? PacketClassifier{} : parse(read_rules_file(path));
    }
    const std::vector<std::uint64_t>* classify(const std::uint8_t* packet, std::size_t size) {
        if (!enabled_) return nullptr;
        ParsedIpFlow flow;
        if (!parse_ip_flow(packet, size, flow)) { record_parse_error(); return nullptr; }
        return classify(flow);
    }
    // The caller must supply a successfully parsed flow.
    const std::vector<std::uint64_t>* classify(const ParsedIpFlow& flow) {
        if (!enabled_) return nullptr;
        for (const auto& rule : rules_)
            if (rule.matches(flow)) { ++hits_; return &rule.labels; }
        ++misses_;
        return nullptr;
    }
    // Preserve accounting when the caller owns parsing; disabled classifiers
    // do not participate in either matching or parse-error counters.
    void record_parse_error() { if (enabled_) ++parse_errors_; }
    void write_stats(std::ostream& output) const {
        output << "classifier_enabled=" << enabled_ << '\n'
               << "classifier_rules=" << rules_.size() << '\n'
               << "classifier_hits=" << hits_ << '\n'
               << "classifier_misses=" << misses_ << '\n'
               << "classifier_parse_errors=" << parse_errors_ << '\n';
    }
};

} // namespace tuntom
