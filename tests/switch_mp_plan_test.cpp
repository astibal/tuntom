#include "../tools/switch_mp_plan.hpp"
#include <iostream>

using namespace tuntom::mp;

void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

Config topology(std::size_t tunnels, std::size_t adapters, std::size_t trunks = 0) {
    Config config;
    for (std::size_t i = 0; i < tunnels; ++i) {
        const auto name = "t" + std::to_string(i);
        config.routes[name].emplace(1, RouteTarget{name, 2});
        config.routes[name].emplace(3, RouteTarget{name, 4}); // Count ports, not routes.
    }
    for (std::size_t i = 0; i < adapters; ++i) config.exits.insert("a" + std::to_string(i));
    for (std::size_t i = 0; i < trunks; ++i) config.trunks.insert("trunk" + std::to_string(i));
    return config;
}

int main() {
    PlanOptions automatic;
    automatic.automatic = true;
    const Hardware xeon{32, 16, 0}, ryzen{16, 8, 0};
    auto profile = startup_profile(topology(20, 3), xeon, automatic);
    check(profile.budget == 8 && profile.reserved == 8, "SMT must not double the physical budget");
    check(profile.tunnels == 20 && profile.adapters == 3, "Unique configured port count");
    check(profile.assignment.shards == std::array<std::size_t, 4>{2, 2, 2, 2}, "20+3 Xeon role split");
    profile = startup_profile(topology(8, 2), ryzen, automatic);
    check(profile.budget == 4 && profile.assignment.shards == std::array<std::size_t, 4>{1, 1, 1, 1},
          "8+2 Ryzen baseline");
    profile = startup_profile(topology(0, 3), xeon, automatic);
    check(profile.assignment.shards[2] == 2 && profile.assignment.shards[3] == 2, "Third adapter splits aggregate roles");
    profile = startup_profile(topology(0, 0, 2), xeon, automatic);
    check(profile.assignment.shards[2] == 2 && profile.assignment.shards[3] == 2, "Two trunks split aggregate roles");
    for (const auto hardware : {Hardware{2, 1, 0}, Hardware{32, 16, 1}, Hardware{32, 16, 3}}) {
        profile = startup_profile(topology(20, 3), hardware, automatic);
        check(profile.budget == 1 && profile.assignment.active == 1 &&
              profile.assignment.worker_roles[0] == 15, "Small affinity/quota shares all roles");
    }
    profile = startup_profile(topology(20, 3), Hardware{32, 16, quota_workers(250000, 100000)}, automatic);
    check(profile.budget == 1 && profile.reserved == 1, "Fractional quota is rounded down before reserving");
    automatic.reserve = 0;
    profile = startup_profile(topology(20, 3), xeon, automatic);
    check(profile.budget == 16, "Zero reserve explicitly permits the full physical budget");
    auto config = topology(20, 3);
    config.workers = 6;
    config.policy.adapter_weight = 2;
    config.policy.trunk_weight = 3;
    config.policy.tx_weight = 2;
    automatic.adapter_weight_explicit = automatic.trunk_weight_explicit = true;
    profile = startup_profile(config, xeon, automatic);
    check(profile.budget == 6 && profile.config.policy.adapter_weight == 2 &&
          profile.config.policy.trunk_weight == 3 && profile.config.policy.tx_weight == 2,
          "Explicit caps and weights survive auto planning");
    automatic.reserve = 16;
    bool rejected = false;
    try { (void)startup_profile(config, xeon, automatic); }
    catch (const std::runtime_error &) { rejected = true; }
    check(rejected, "Reserve cannot consume every CPU");
    automatic.automatic = false;
    rejected = false;
    try { (void)startup_profile(config, xeon, automatic); }
    catch (const std::runtime_error &) { rejected = true; }
    check(rejected, "Reserve without auto must fail");
    automatic.reserve.reset();
    profile = startup_profile(config, xeon, automatic);
    check(profile.budget == 6 && profile.reserved == 0 && profile.config.policy.adapter_weight == 2,
          "Manual mode keeps original policy");
    for (std::size_t physical = 1; physical <= 32; ++physical) {
        for (std::size_t adapters = 0; adapters <= 5; ++adapters) {
            PlanOptions options;
            options.automatic = true;
            profile = startup_profile(topology(20, adapters, 2), Hardware{physical * 2, physical, 0}, options);
            check(profile.assignment.active <= profile.budget && profile.budget + profile.reserved == physical,
                  "Every profile respects the hardware budget");
            for (const auto &owners : profile.assignment.owners)
                check(owners[0] < profile.budget && owners[1] < profile.budget, "Port owners within pool");
        }
    }
    std::cout << "PASS: auto pool, SMT/affinity/quota budgets, adapters/trunks, overrides and small VMs\n";
}
