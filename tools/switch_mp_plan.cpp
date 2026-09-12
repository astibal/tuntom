#include "switch_mp_plan.hpp"
#include <iostream>

int main(int argc, char **argv) {
    using namespace tuntom::mp;
    try {
        PlanOptions options;
        std::vector<char *> arguments{argv[0]};
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--auto-pool") options.automatic = true;
            else if (option == "--reserve-cpus") {
                if (++i == argc) throw std::runtime_error("--reserve-cpus requires a value");
                options.reserve = std::string(argv[i]) == "0" ? 0 : number(argv[i]);
            } else {
                arguments.push_back(argv[i]);
                if (option == "--adapter-weight") options.adapter_weight_explicit = true;
                if (option == "--trunk-weight") options.trunk_weight_explicit = true;
                if (option != "--help" && option != "-h" &&
                    option != "--default-back=on" && option != "--default-back=off" && i + 1 < argc)
                    arguments.push_back(argv[++i]);
            }
        }
        auto config = parse_config(static_cast<int>(arguments.size()), arguments.data());
        if (config.help) {
            usage(std::cout, argv[0]);
            std::cout << "Planner: --auto-pool [--reserve-cpus N (0..65535)]\n";
            return 0;
        }
        const auto hardware = detect_hardware();
        const auto profile = startup_profile(std::move(config), hardware, options);
        const auto &policy = profile.config.policy;
        std::cout << "hardware.logical_cpus=" << hardware.logical
                  << "\nhardware.physical_cores=" << hardware.physical
                  << "\nhardware.quota_cpus=" << hardware.quota
                  << "\nhardware.worker_limit=" << hardware.limit()
                  << "\nreserved_cpus=" << profile.reserved
                  << "\nconfigured.tunnels=" << profile.tunnels
                  << "\nconfigured.adapters=" << profile.adapters
                  << "\nconfigured.trunks=" << profile.trunks
                  << "\nworkers.pool=" << profile.budget
                  << "\nworkers.active_when_connected=" << profile.assignment.active
                  << "\nworkers.idle_when_connected=" << profile.budget - profile.assignment.active << '\n';
        for (std::size_t role = 0; role < 4; ++role)
            std::cout << "roles." << role_name(role) << '=' << profile.assignment.shards[role] << '\n';
        for (std::size_t worker = 0; worker < profile.budget; ++worker) {
            std::cout << "worker." << worker << '=';
            bool first = true;
            for (std::size_t role = 0; role < 4; ++role) {
                if (!(profile.assignment.worker_roles[worker] & (1U << role))) continue;
                std::cout << (first ? "" : ",") << role_name(role);
                first = false;
            }
            std::cout << (first ? "idle" : "") << '\n';
        }
        std::cout << "option.workers=" << profile.budget
                  << "\noption.work-per-thread=" << policy.work_per_thread
                  << "\noption.rx-weight=" << policy.rx_weight
                  << "\noption.tx-weight=" << policy.tx_weight
                  << "\noption.adapter-weight=" << policy.adapter_weight
                  << "\noption.trunk-weight=" << policy.trunk_weight << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
