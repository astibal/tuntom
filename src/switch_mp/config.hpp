#pragma once

#include "../ipc/switch_protocol.hpp"
#include "../switch_routes.hpp"
#include "../switch_ruleset.hpp"
#include "../switch_admission.hpp"
#include "scheduler.hpp"
#include "../ipc/switch_v2.hpp"
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace tuntom::mp {

using RouteTarget = SwitchRouteTarget;
using PortRoutes = SwitchPortRoutes;

struct Config {
    std::string socket, control;
    std::unordered_map<std::string, PortRoutes> routes;
    std::unordered_set<std::string> exits, trunks;
    std::shared_ptr<const SwitchRuleset> ruleset;
    SwitchCapacity capacity;
    Policy policy;
    ipc::Options ipc;
    std::uint64_t mmap_budget = 256ULL * 1024 * 1024;
    std::size_t workers = 0, pool_size = 128, queue_size = 128;
    bool default_back = false, help = false;

    Kind kind(const std::string &port) const {
        if (ruleset) return ruleset->role(port, RuleStatement::Type::exit) ? Kind::adapter :
            ruleset->role(port, RuleStatement::Type::trunk) ? Kind::trunk : Kind::tunnel;
        return exits.count(port) ? Kind::adapter : trunks.count(port) ? Kind::trunk : Kind::tunnel;
    }
};

inline void usage(std::ostream &out, const char *program) {
    out << "Usage: " << program << " --socket PATH [options]\n"
        << "  --control-socket PATH            tuntomctl show stats endpoint\n"
        << "  --rules-file PATH                format 1 or 2; live rules check/load/show via control\n"
        << "  --route IN:LABEL=OUT:LABEL       trailing * on either port; multiple outputs use ECMP\n"
        << "  --exit-port ID                  adapter group; deliver EXIT opcode\n"
        << "  --trunk-port ID                 aggregate group; preserve SWITCH opcode\n"
        << "  --default-back=off|on           default off\n"
        << "  --max-ports N --max-pending N   defaults 256 / 16, limited by available FDs\n"
        << "  --workers N                     cap pre-spawned data workers (default: physical CPU "
           "budget)\n"
        << "  --work-per-thread N             split above weighted sum N (default 8)\n"
        << "  --rx-weight N --tx-weight N     direction cost multipliers (defaults 1 / 1)\n"
        << "  --adapter-weight N              aggregate port cost (default 2)\n"
        << "  --trunk-weight N                trunk port cost (default 4)\n"
        << "  --pool-size N --queue-size N    buffers per ingress / pointers per pair (defaults "
           "128 / 128)\n"
        << "  --ipc-mode auto|v1|inline        default auto; negotiate V2 mmap or legacy inline\n"
        << "  --ipc-batch N                    maximum references per record, 1..16 (default 8)\n"
        << "  --ipc-slots N                    slots per direction, 1..128 (default 128)\n"
        << "  --ipc-frame-capacity N           bytes per slot, 17..65607 (default 16384)\n"
        << "  --ipc-memory-mib N               active + pending mmap budget (default 256)\n"
        << "All other numeric options: 1..65535. Workers never exceed the detected CPU budget.\n";
}

inline std::size_t number(const std::string &text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Expected a number in 1..65535: " + text);
    const auto value = std::stoull(text);
    if (value == 0 || value > 65535)
        throw std::runtime_error("Number outside 1..65535: " + text);
    return static_cast<std::size_t>(value);
}

inline void validate_port(const std::string &port) { (void)encode_switch_registration(port); }


inline Config parse_config(int argc, char **argv) {
    Config config;
    std::string rules_file;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--help" || option == "-h") {
            config.help = true;
            continue;
        }
        if (option == "--default-back=on") {
            config.default_back = true;
            continue;
        }
        if (option == "--default-back=off") {
            config.default_back = false;
            continue;
        }
        if (++i >= argc)
            throw std::runtime_error(option + " requires a value");
        const std::string value = argv[i];
        if (option == "--socket" || option == "--control-socket") {
            auto &path = option == "--socket" ? config.socket : config.control;
            if (!path.empty())
                throw std::runtime_error("Duplicate " + option);
            path = value;
        } else if (option == "--route") {
            add_switch_route(config.routes, value);
        } else if (option == "--rules-file") {
            if (!rules_file.empty()) throw std::runtime_error("Duplicate --rules-file");
            rules_file = value;
        } else if (option == "--exit-port" || option == "--trunk-port") {
            validate_port(value);
            auto &ports = option == "--exit-port" ? config.exits : config.trunks;
            if (!ports.insert(value).second)
                throw std::runtime_error("Duplicate port role: " + value);
        } else if (option == "--ipc-mode")
            config.ipc.mode = ipc::parse_mode(value);
        else if (option == "--ipc-batch") {
            const auto n = number(value);
            if (n > ipc::max_batch) throw std::runtime_error("--ipc-batch must be 1..16");
            config.ipc.batch = static_cast<std::uint32_t>(n);
        } else if (option == "--ipc-slots") {
            const auto n = number(value);
            if (n > ipc::max_slots) throw std::runtime_error("--ipc-slots must be 1..128");
            config.ipc.slots = static_cast<std::uint32_t>(n);
        } else if (option == "--ipc-frame-capacity") {
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("--ipc-frame-capacity must be 17..65607");
            const auto n = std::stoull(value);
            if (n < ipc::min_frame || n > ipc::max_frame)
                throw std::runtime_error("--ipc-frame-capacity must be 17..65607");
            config.ipc.frame_capacity = static_cast<std::uint32_t>(n);
        } else if (option == "--ipc-memory-mib")
            config.mmap_budget = number(value) * 1024ULL * 1024;
        else if (option == "--max-ports")
            config.capacity.ports = number(value);
        else if (option == "--max-pending")
            config.capacity.pending = number(value);
        else if (option == "--workers")
            config.workers = number(value);
        else if (option == "--pool-size")
            config.pool_size = number(value);
        else if (option == "--queue-size")
            config.queue_size = number(value);
        else if (option == "--work-per-thread")
            config.policy.work_per_thread = number(value);
        else if (option == "--rx-weight")
            config.policy.rx_weight = number(value);
        else if (option == "--tx-weight")
            config.policy.tx_weight = number(value);
        else if (option == "--adapter-weight")
            config.policy.adapter_weight = number(value);
        else if (option == "--trunk-weight")
            config.policy.trunk_weight = number(value);
        else
            throw std::runtime_error("Unknown option: " + option);
    }
    for (const auto &port : config.exits)
        if (config.trunks.count(port))
            throw std::runtime_error("Port is both adapter and trunk: " + port);
    if (!rules_file.empty()) {
        if (!config.routes.empty() || !config.exits.empty() || !config.trunks.empty() || config.default_back)
            throw std::runtime_error("--rules-file cannot be combined with legacy routing options");
        config.ruleset = parse_switch_ruleset(read_rules_file(rules_file));
    }
    if (!config.help && config.socket.empty())
        throw std::runtime_error("--socket is required");
    return config;
}

} // namespace tuntom::mp
