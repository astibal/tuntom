#pragma once
#include "protocol.hpp"
#include "../via/switch.hpp"

namespace tuntom::relay {
using Clock = std::chrono::steady_clock;
struct Registry {
    Directory directory;
    Clock::time_point expires{};
    bool live(Clock::time_point now = Clock::now()) const { return directory.epoch && now < expires; }
};
inline bool configured(const SwitchRuleset& rules, const std::string& parent) {
    for (const auto& item : rules.services) if (!item.second.relay.empty() && item.second.relay_matches(parent)) return true;
    return false;
}
// Snapshots are atomic and idempotent. A parent connection has one epoch; a new
// tunnel session must reconnect the parent before advertising another epoch.
inline bool update(Registry& registry, const SwitchRuleset& rules, const std::string& parent,
                   const View& view, const std::vector<std::string>& occupied, bool& changed) {
    changed = false;
    if (!configured(rules,parent) || view.type != Type::snapshot ||
        (registry.directory.epoch && registry.directory.epoch != view.epoch) ||
        view.channel < registry.directory.revision) return false;
    std::map<std::uint32_t,Channel> channels;
    if (!snapshot(view,channels)) return false;
    for (const auto& item : channels) {
        const auto& c = item.second;
        if (!via::reserved(c.name) || !via::accepted(rules,c.name,parent) ||
            std::find(occupied.begin(),occupied.end(),c.name) != occupied.end()) return false;
        via::Registration a; via::registration(c.name,a);
        for (const auto& name : occupied) {
            via::Registration b;
            // A pair belongs to one relay parent, never two independently owned tunnels.
            if (via::registration(name,b) && a.instance == b.instance) return false;
        }
        for (const auto& other : channels) if (other.first != item.first) {
            via::Registration b; via::registration(other.second.name,b);
            if (a.instance == b.instance && (a.server == b.server || c.owner != other.second.owner)) return false;
        }
    }
    // An equal revision must describe exactly the same directory.
    const auto old = snapshot(registry.directory.epoch,registry.directory.revision,registry.directory.channels);
    const auto incoming = snapshot(view.epoch,view.channel,channels);
    if (view.channel == registry.directory.revision && old != incoming) return false;
    changed = !registry.live() || old != incoming;
    registry.directory = {view.epoch,view.channel,std::move(channels)};
    registry.expires = Clock::now() + std::chrono::seconds(3);
    return true;
}
} // namespace tuntom::relay
