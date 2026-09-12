#pragma once

#include "../src/switch_mp/config.hpp"
#include <optional>
#include <set>

namespace tuntom::mp {

struct PlanOptions {
    bool automatic = false;
    bool adapter_weight_explicit = false, trunk_weight_explicit = false;
    std::optional<std::size_t> reserve;
};

struct StartupProfile {
    Config config;
    std::size_t budget = 1, reserved = 0, tunnels = 0, adapters = 0, trunks = 0;
    std::vector<Kind> ports;
    Assignment assignment;
};

// Pure policy so hardware limits, small VMs and explicit overrides can be
// checked without pretending that synthetic CPUs exist on the test machine.
inline StartupProfile startup_profile(Config config, const Hardware &hardware,
                                      const PlanOptions &options) {
    StartupProfile result;
    const auto available = std::min<std::size_t>(65535, hardware.limit());
    if (!available)
        throw std::runtime_error("No CPU budget available");
    if (options.reserve && !options.automatic)
        throw std::runtime_error("--reserve-cpus requires --auto-pool");
    if (options.automatic) {
        result.reserved = options.reserve.value_or(std::min(available - 1, (available + 1) / 2));
        if (result.reserved >= available)
            throw std::runtime_error("--reserve-cpus must leave at least one data worker");
        // Share an aggregate pair across at most two adapters at default
        // weights; give each trunk a full group's weight. These remain startup
        // estimates, not measurements of port utilization.
        if (!options.adapter_weight_explicit)
            config.policy.adapter_weight = std::max<std::size_t>(2, (config.policy.work_per_thread + 1) / 2);
        if (!options.trunk_weight_explicit)
            config.policy.trunk_weight = std::max<std::size_t>(4, config.policy.work_per_thread);
    }
    result.budget = available - result.reserved;
    if (config.workers)
        result.budget = std::min(result.budget, config.workers);
    config.workers = result.budget;

    std::set<std::string> names(config.exits.begin(), config.exits.end());
    names.insert(config.trunks.begin(), config.trunks.end());
    for (const auto &source : config.routes) {
        names.insert(source.first);
        for (const auto &route : source.second)
            names.insert(route.second.port);
    }
    for (const auto &name : names) {
        const auto kind = config.kind(name);
        result.ports.push_back(kind);
        if (kind == Kind::tunnel) ++result.tunnels;
        else if (kind == Kind::adapter) ++result.adapters;
        else ++result.trunks;
    }
    result.assignment = schedule(result.ports, result.budget, config.policy);
    result.config = std::move(config);
    return result;
}

} // namespace tuntom::mp
