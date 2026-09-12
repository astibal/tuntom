#pragma once

#include "../ipc/switch_protocol.hpp"
#include "../switch_admission.hpp"
#include "scheduler.hpp"
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace tuntom::mp {

struct RouteTarget {
    std::string port;
    std::uint64_t label = 0;
};
using PortRoutes = std::unordered_map<std::uint64_t, RouteTarget>;

struct Config {
    std::string socket, control;
    std::unordered_map<std::string, PortRoutes> routes;
    std::unordered_set<std::string> exits, trunks;
    SwitchCapacity capacity;
    Policy policy;
    std::size_t workers = 0, pool_size = 128, queue_size = 128;
    bool default_back = false, help = false;

    Kind kind(const std::string &port) const {
        return exits.count(port) ? Kind::adapter : trunks.count(port) ? Kind::trunk : Kind::tunnel;
    }
};

inline void usage(std::ostream &out, const char *program) {
    out << "Usage: " << program << " --socket PATH [options]\n"
        << "  --control-socket PATH            tuntomctl show stats endpoint\n"
        << "  --route IN:LABEL=OUT:LABEL       repeatable, same wire protocol as tuntom-switch\n"
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
        << "All numeric options: 1..65535. Workers never exceed the detected CPU budget.\n";
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

inline std::pair<std::string, std::uint64_t> endpoint(const std::string &text) {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == text.size())
        throw std::runtime_error("Invalid route endpoint: " + text);
    const auto port = text.substr(0, colon);
    validate_port(port);
    const auto label = text.substr(colon + 1);
    if (label[0] == '-' || label.find_first_of(" \t\r\n") != std::string::npos)
        throw std::runtime_error("Invalid label: " + label);
    std::size_t used = 0;
    const auto value = std::stoull(label, &used, 0);
    if (used != label.size())
        throw std::runtime_error("Invalid label: " + label);
    return {port, value};
}

inline Config parse_config(int argc, char **argv) {
    Config config;
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
            const auto equals = value.find('=');
            if (equals == std::string::npos)
                throw std::runtime_error("Invalid route: " + value);
            const auto input = endpoint(value.substr(0, equals));
            const auto output = endpoint(value.substr(equals + 1));
            if (!config.routes[input.first]
                     .emplace(input.second, RouteTarget{output.first, output.second})
                     .second)
                throw std::runtime_error("Duplicate route: " + value);
        } else if (option == "--exit-port" || option == "--trunk-port") {
            validate_port(value);
            auto &ports = option == "--exit-port" ? config.exits : config.trunks;
            if (!ports.insert(value).second)
                throw std::runtime_error("Duplicate port role: " + value);
        } else if (option == "--max-ports")
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
    if (!config.help && config.socket.empty())
        throw std::runtime_error("--socket is required");
    return config;
}

} // namespace tuntom::mp
