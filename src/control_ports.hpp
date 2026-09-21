#pragma once
#include "control_protocol.hpp"
#include <algorithm>
#include <string>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace tuntom {
// Registrations contain printable ASCII without whitespace, so TSV needs no
// quoting. This describes attachment topology, not unadvertised capabilities.
class ControlPorts {
    struct Row { std::string port, attachment, via; };
    std::vector<Row> rows_;
    std::size_t bytes_ = std::string("port\tattachment\tvia\n").size();
    void add(const std::string& name, const std::string& attachment, const std::string& via) {
        const auto size = name.size() + attachment.size() + via.size() + 3;
        if (size > control_max_body - bytes_) throw std::runtime_error("port list exceeds control response limit");
        rows_.push_back({name, attachment, via});
        bytes_ += size;
    }
public:
    // Client-side rendering of the same TSV snapshot returned by show ports.
    static std::string tree(const std::string& snapshot) {
        std::istringstream input(snapshot);
        std::string line;
        if (!std::getline(input, line) || line != "port\tattachment\tvia")
            throw std::runtime_error("invalid port-list header");
        std::map<std::string, std::set<std::string>> ports;
        std::vector<std::pair<std::string, std::string>> relays;
        std::set<std::string> names;
        while (std::getline(input, line)) {
            const auto a = line.find('\t');
            const auto b = a == std::string::npos ? a : line.find('\t', a + 1);
            if (b == std::string::npos || line.find('\t', b + 1) != std::string::npos)
                throw std::runtime_error("invalid port-list row");
            const auto name = line.substr(0, a), kind = line.substr(a + 1, b - a - 1), via = line.substr(b + 1);
            const auto valid = [](const std::string& value) {
                return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
            };
            if (!valid(name) || !valid(via) || !names.insert(name).second)
                throw std::runtime_error("invalid or duplicate port name");
            if (kind == "direct" && via == "-") ports.emplace(name, std::set<std::string>{});
            else if (kind == "relay") relays.emplace_back(via, name);
            else throw std::runtime_error("invalid port attachment");
        }
        for (const auto& relay : relays) {
            const auto parent = ports.find(relay.first);
            if (parent == ports.end()) throw std::runtime_error("relay parent missing from port snapshot");
            parent->second.insert(relay.second);
        }
        std::string out = "switch\n";
        if (ports.empty()) return out + "`-- (no registered ports)\n";
        std::size_t index = 0;
        for (const auto& port : ports) {
            const bool last = ++index == ports.size();
            out += std::string(last ? "`-- " : "|-- ") + port.first + '\n';
            std::size_t child = 0;
            for (const auto& name : port.second)
                out += std::string(last ? "    " : "|   ") + (++child == port.second.size() ? "`-- " : "|-- ") + name + '\n';
        }
        return out;
    }
    void direct(const std::string& name) { add(name, "direct", "-"); }
    void relayed(const std::string& name, const std::string& parent) { add(name, "relay", parent); }
    std::string finish() {
        std::sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) { return a.port < b.port; });
        std::string out = "port\tattachment\tvia\n";
        for (const auto& row : rows_) {
            out += row.port + '\t' + row.attachment + '\t' + row.via + '\n';
        }
        return out;
    }
};
} // namespace tuntom
