#pragma once

#include "info_codec.hpp"
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

namespace tuntom::info {

// OS discovery stays outside the portable INFO codec.
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

} // namespace tuntom::info
