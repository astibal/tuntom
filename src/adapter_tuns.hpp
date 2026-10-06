#pragma once

#include "tun_device.hpp"
#include "tun_provider.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace tuntom::adapter_tuns {

struct Interface {
    std::string name;
    bool multiqueue = false;
};

inline std::vector<std::unique_ptr<TunDevice>> open(
    const std::vector<Interface>& interfaces, std::size_t mtu,
    const std::string& netns = {}) {
    std::vector<std::unique_ptr<TunDevice>> result;
    result.reserve(interfaces.size());
    if (netns.empty()) {
        for (const auto& interface : interfaces) {
            auto tun = std::make_unique<TunDevice>(interface.name, mtu, interface.multiqueue);
            tun->set_up();
            result.push_back(std::move(tun));
        }
        return result;
    }

    std::vector<tun_provider::Request> requests;
    requests.reserve(interfaces.size());
    for (const auto& interface : interfaces)
        requests.push_back({interface.name, mtu, interface.multiqueue, true});
    auto received = tun_provider::create_many_in_child(requests, netns);
    try {
        for (std::size_t index = 0; index < interfaces.size(); ++index) {
            if (received[index].interface_name != interfaces[index].name || received[index].mtu != mtu)
                throw std::runtime_error("TUN provider returned mismatched interface metadata");
            const bool multiqueue = interfaces[index].multiqueue;
            result.push_back(std::make_unique<TunDevice>(std::move(received[index]), multiqueue));
            received[index].fd = -1;
        }
    } catch (...) {
        for (const auto& item : received) if (item.fd >= 0) ::close(item.fd);
        throw;
    }
    return result;
}

} // namespace tuntom::adapter_tuns
