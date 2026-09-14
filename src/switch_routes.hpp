#pragma once

#include "ipc/switch_protocol.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tuntom {

struct SwitchRouteTarget {
    std::string port;
    std::uint64_t label = 0;
};
using SwitchPortRoutes = std::unordered_map<std::uint64_t, SwitchRouteTarget>;
using SwitchRoutes = std::unordered_map<std::string, SwitchPortRoutes>;

inline bool wildcard_port(std::string_view port) {
    return !port.empty() && port.back() == '*';
}

inline bool route_port_matches(std::string_view pattern, std::string_view port) {
    if (!wildcard_port(pattern)) return pattern == port;
    pattern.remove_suffix(1);
    return port.substr(0, pattern.size()) == pattern;
}

inline void validate_port_wildcard(std::string_view port) {
    if (port == "*")
        throw std::runtime_error("port wildcard '*' requires a non-empty prefix");
    const auto star = port.find('*');
    if (star != std::string_view::npos && star != port.size() - 1)
        throw std::runtime_error("port wildcard must be a single trailing '*'");
}

inline std::pair<std::string, std::uint64_t> switch_route_endpoint(const std::string &text) {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == text.size())
        throw std::runtime_error("Invalid route endpoint: " + text);
    const auto port = text.substr(0, colon);
    (void)encode_switch_registration(port);
    validate_port_wildcard(port);
    const auto label = text.substr(colon + 1);
    if (label[0] == '-' || label.find_first_of(" \t\r\n") != std::string::npos)
        throw std::runtime_error("Invalid label: " + label);
    std::size_t used = 0;
    const auto value = std::stoull(label, &used, 0);
    if (used != label.size()) throw std::runtime_error("Invalid label: " + label);
    return {port, value};
}

inline void add_switch_route(SwitchRoutes &routes, const std::string &text) {
    const auto equals = text.find('=');
    if (equals == std::string::npos) throw std::runtime_error("Invalid route: " + text);
    const auto input = switch_route_endpoint(text.substr(0, equals));
    const auto output = switch_route_endpoint(text.substr(equals + 1));
    if (!routes[input.first].emplace(input.second, SwitchRouteTarget{output.first, output.second}).second)
        throw std::runtime_error("Duplicate route: " + text);
}

// Resolve ingress patterns at registration / plan preparation, never per packet.
// Precedence is per label: exact name, then longest matching non-empty prefix.
inline SwitchPortRoutes routes_for_port(const SwitchRoutes &routes, const std::string &port) {
    SwitchPortRoutes resolved;
    const auto exact = routes.find(port);
    if (exact != routes.end() && !wildcard_port(exact->first)) resolved = exact->second;
    std::vector<const SwitchRoutes::value_type *> patterns;
    for (const auto &entry : routes)
        if (wildcard_port(entry.first) && route_port_matches(entry.first, port))
            patterns.push_back(&entry);
    std::sort(patterns.begin(), patterns.end(), [](const auto *a, const auto *b) {
        return a->first.size() > b->first.size();
    });
    for (const auto *entry : patterns)
        for (const auto &route : entry->second) resolved.emplace(route);
    return resolved;
}

} // namespace tuntom
