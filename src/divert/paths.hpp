#pragma once

#include "../via/registration.hpp"
#include <set>

namespace tuntom::divert {
struct AdapterPath {
    std::string id, socket, input, output;
};
inline std::vector<AdapterPath> adapter_paths(const std::string& socket, const std::vector<std::string>& paths,
        const std::string& instance, const std::string& input, const std::string& output) {
    if (input == output) throw std::runtime_error("two distinct adapter ports are required");
    if (paths.empty()) {
        if (socket.empty()) throw std::runtime_error("--switch-socket or --relay-path is required");
        return {{"", socket, instance.empty() ? input : via::port_name(input, instance, false),
                             instance.empty() ? output : via::port_name(output, instance, true)}};
    }
    if (!socket.empty() || instance.empty())
        throw std::runtime_error("--relay-path requires --via-instance and replaces --switch-socket");
    if (paths.size() > 16) throw std::runtime_error("at most 16 relay paths are supported");
    std::set<std::string> ids, sockets;
    std::vector<AdapterPath> result;
    for (const auto& value : paths) {
        const auto at = value.find('=');
        if (at == std::string::npos || !at || at + 1 == value.size())
            throw std::runtime_error("--relay-path expects ID=SOCKET");
        const auto id = value.substr(0, at), path = value.substr(at + 1);
        if (id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos ||
            !ids.insert(id).second || !sockets.insert(path).second)
            throw std::runtime_error("relay path IDs must be unique alphanumeric/_/- names; sockets must be distinct");
        const auto identity = instance + "#path-" + id;
        result.push_back({id, path, via::port_name(input + "." + id, identity, false),
                                    via::port_name(output + "." + id, identity, true)});
    }
    return result;
}
} // namespace tuntom::divert
