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
    std::size_t wildcard_patterns = 0;
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
    std::set<std::string> patterns;
    const auto add_name = [&](const std::string &name) {
        if (wildcard_port(name)) patterns.insert(name);
        else names.insert(name);
    };
    for (const auto &source : config.routes) {
        add_name(source.first);
        for (const auto &route : source.second) add_name(route.second.port);
    }
    if (config.ruleset) {
        for (const auto &statement : config.ruleset->statements) {
            if (statement.type == RuleStatement::Type::mapping || statement.type == RuleStatement::Type::forward) {
                add_name(statement.input.port);
                add_name(statement.output.port);
            } else if (statement.type == RuleStatement::Type::exit || statement.type == RuleStatement::Type::trunk) {
                add_name(statement.output.port);
            }
        }
    }
    if (config.divert_config && !config.divert_config->via) {
        names.insert(config.divert_config->input);
        names.insert(config.divert_config->output);
        for (const auto &origin : config.divert_config->origins) names.insert(origin.first);
    }
    if (via::enabled(config.ruleset)) {
        for (const auto& origin : config.ruleset->origins) names.insert(origin.first);
        for (const auto& item : config.ruleset->services) {
            const auto& service = item.second;
            for (const auto& side : {service.client, service.server})
                if (wildcard_port(side)) patterns.insert(side);
            // IDs with exact attachment names can be estimated before registration.
            for (const auto& id : service.instances) {
                if (!wildcard_port(service.client)) names.insert(via::port_name(service.client, id, false));
                if (!wildcard_port(service.server)) names.insert(via::port_name(service.server, id, true));
            }
        }
    }
    result.wildcard_patterns = patterns.size();
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
