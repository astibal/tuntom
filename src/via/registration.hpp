#pragma once

#include "../ipc/switch_protocol.hpp"
#include <sys/socket.h>

namespace tuntom::via {
inline constexpr const char* suffix = "~via:";
struct Registration {
    std::string attachment, instance;
    bool server = false;
};
inline bool reserved(const std::string& name) { return name.find(suffix) != std::string::npos; }
inline bool registration(const std::string& name, Registration& result) {
    const auto at = name.find(suffix);
    if (at == std::string::npos || !at || at + 7 >= name.size() || name[at + 6] != ':' ||
        (name[at + 5] != 'c' && name[at + 5] != 's')) return false;
    result = {name.substr(0, at), name.substr(at + 7), name[at + 5] == 's'};
    return result.attachment.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") == std::string::npos &&
        result.instance.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.#") == std::string::npos;
}
inline std::string port_name(const std::string& attachment, const std::string& instance, bool server) {
    const auto name = attachment + suffix + (server ? "s:" : "c:") + instance;
    Registration parsed;
    if (!registration(name, parsed)) throw std::runtime_error("invalid VIA attachment or instance ID");
    (void)encode_switch_registration(name);
    return name;
}
inline bool same_owner(int a, int b) {
    ucred left{}, right{}; socklen_t size = sizeof(ucred);
    if (::getsockopt(a, SOL_SOCKET, SO_PEERCRED, &left, &size) || size != sizeof(ucred)) return false;
    size = sizeof(ucred);
    return !::getsockopt(b, SOL_SOCKET, SO_PEERCRED, &right, &size) && size == sizeof(ucred) &&
        left.pid == right.pid && left.uid == right.uid && left.gid == right.gid;
}
// Call for every live registration before publishing either side. Both sockets
// must be owned by the same process; an existing side is never replaced.
inline bool compatible(const std::string& incoming, int fd, const std::string& existing, int other) {
    Registration a, b;
    if (!registration(incoming, a)) return !reserved(incoming);
    if (!registration(existing, b)) return existing != incoming;
    if (a.instance != b.instance) return true;
    return a.server != b.server && same_owner(fd, other);
}
} // namespace tuntom::via
