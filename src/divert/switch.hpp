#pragma once

#include "codec.hpp"
#include "../switch_ruleset.hpp"
#include "../switch_ecmp.hpp"
#include <atomic>
#include <map>
#include <sstream>

namespace tuntom::divert {

struct Config {
    std::string cookie, input, output;
    bool via = false;
    std::vector<RuleStatement> via_matches;
    std::map<std::string, std::uint64_t> origins;
    std::map<std::uint64_t, std::string> names;
    std::shared_ptr<const SwitchRuleset> matches;
    mutable std::atomic<bool> enabled{false};
    std::string command(const std::string& operation) const {
        if (operation == "divert.enable") enabled.store(true, std::memory_order_release);
        else if (operation == "divert.stop") enabled.store(false, std::memory_order_release);
        else if (operation != "divert.show") throw std::runtime_error("unknown divert operation");
        return std::string("divert_enabled=") + (enabled.load(std::memory_order_acquire) ? "1\n" : "0\n");
    }
};
inline std::shared_ptr<Config> read_config(const std::string& path) {
    auto config = std::make_shared<Config>();
    std::istringstream file(read_rules_file(path));
    std::string line, match_rules = "format 2\nserial 0\n";
    std::size_t matches = 0;
    while (std::getline(file, line)) {
        std::istringstream fields(line);
        std::string op, extra;
        if (!(fields >> op) || op[0] == '#') continue;
        if (op == "format") {
            std::string value;
            if (config->via || matches || !config->cookie.empty() || !(fields >> value) || value != "3" || (fields >> extra))
                throw std::runtime_error("expected format 3 before VIA divert matches");
            config->via = true; continue;
        }
        if (config->via) {
            RulesLine p(line, true); p.need("match");
            RuleStatement match; match.input = p.endpoint_v2(); p.need("via"); match.via = p.names();
            if (!p.done()) throw std::runtime_error("unexpected VIA divert field");
            config->via_matches.push_back(std::move(match));
            if (config->via_matches.size() > ruleset_max_statements) throw std::runtime_error("too many divert matches");
            continue;
        }
        if (op == "cookie") {
            if (!config->cookie.empty() || !(fields >> config->cookie)) throw std::runtime_error("expected one divert cookie");
        } else if (op == "ports") {
            if (!config->input.empty() || !(fields >> config->input >> config->output))
                throw std::runtime_error("expected divert ports IN OUT");
        } else if (op == "origin") {
            std::string name, number;
            if (!(fields >> name >> number) || number.empty() || number.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("expected divert origin PORT STABLE_ID");
            const auto id = std::stoull(number);
            (void)encode_switch_registration(name);
            if (!id || !config->origins.emplace(name, id).second || !config->names.emplace(id, name).second)
                throw std::runtime_error("duplicate or zero divert origin ID");
        } else if (op == "match") {
            std::string selector;
            std::getline(fields, selector);
            // Parse each selector separately so it cannot inject another statement.
            auto rule = parse_switch_ruleset("format 2\nserial 0\nswitch " + selector + " to divert-match allow\n");
            (void)rule;
            match_rules += "switch " + selector + " to divert-match allow\n";
            ++matches;
            continue;
        } else throw std::runtime_error("unknown divert directive: " + op);
        if (fields >> extra) throw std::runtime_error("unexpected divert configuration field: " + extra);
    }
    if (config->via) {
        if (config->via_matches.empty()) throw std::runtime_error("VIA divert requires a match");
        return config;
    }
    (void)Codec(config->cookie);
    (void)encode_switch_registration(config->input);
    (void)encode_switch_registration(config->output);
    if (!matches || config->origins.empty() || config->input == config->output ||
        config->origins.count(config->input) || config->origins.count(config->output))
        throw std::runtime_error("divert requires distinct adapter ports, origins and at least one match");
    config->matches = parse_switch_ruleset(match_rules);
    return config;
}

enum class Result { normal, forward, malformed, overflow, policy, missing };
struct Decision {
    Result result = Result::normal;
    const std::string* target = nullptr;
    Stack labels;
    bool exit = false, multipath = false;
};

// Immutable per topology/rules generation. No per-flow lookup in the switch.
class SwitchPath {
    struct Origin {
        std::uint64_t id;
        RulesProgram match, normal;
        Origin(std::uint64_t value, const Config& c, const SwitchRuleset& r, const std::string& name)
            : id(value), match(*c.matches, name), normal(r, name) {}
    };
    std::shared_ptr<const Config> config_;
    std::shared_ptr<const SwitchRuleset> rules_;
    Codec codec_;
    std::map<std::string, Origin> origins_;
    std::vector<std::string> ports_;
public:
    SwitchPath(std::shared_ptr<const Config> config, std::shared_ptr<const SwitchRuleset> rules,
               std::vector<std::string> ports)
        : config_(std::move(config)), rules_(std::move(rules)), codec_(config_->cookie), ports_(std::move(ports)) {
        if (!rules_) throw std::runtime_error("--divert-file requires --rules-file");
        for (const auto& origin : config_->origins)
            origins_.emplace(std::piecewise_construct, std::forward_as_tuple(origin.first),
                std::forward_as_tuple(origin.second, *config_, *rules_, origin.first));
    }
    bool possible(const std::string& from, const std::string& to) const {
        if (from == config_->input && config_->origins.count(to)) return true;
        if (from == config_->input || from == config_->output) {
            for (const auto& origin : origins_)
                for (const auto* rule : origin.second.normal.mappings)
                    if (route_port_matches(rule->output.port, to)) return true;
            return false;
        }
        return (config_->origins.count(from) && to == config_->input) || to == config_->output;
    }
    template<class Live> Decision route(const std::string& physical, const SwitchFrameView& frame, Live live) const {
        Decision d;
        const auto reject = [&](Result result) { d.result = result; return d; };
        auto labels = labels_of(frame);
        Envelope env;
        if (!codec_.split(labels, env)) return reject(Result::malformed);
        const auto deliver = [&](const std::string& target, const Stack& stack, bool exit) {
            d.target = &target; d.labels = stack; d.exit = exit;
            d.result = live(target) ? Result::forward : Result::missing;
            return d;
        };
        const bool adapter_in = physical == config_->input, adapter_out = physical == config_->output;
        if (!adapter_in && !adapter_out) {
            if (env.present) {
                if (env.action() != onward || !config_->names.count(env.origin()) ||
                    !rules_->role(physical, RuleStatement::Type::exit)) return reject(Result::malformed);
                return deliver(config_->output, labels, true);
            }
            const auto origin = origins_.find(physical);
            if (!config_->enabled.load(std::memory_order_acquire) || origin == origins_.end() ||
                !origin->second.match.mapping(frame)) return d;
            if (!codec_.offer(labels, origin->second.id, d.labels)) return reject(Result::overflow);
            return deliver(config_->input, d.labels, true);
        }
        if (!env.present) return reject(Result::malformed);
        const auto name = config_->names.find(env.origin());
        if (name == config_->names.end()) return reject(Result::malformed);
        if (adapter_in && env.action() == to_client) return deliver(name->second, env.original(), false);
        if (!(adapter_in && env.action() == bypass) && !(adapter_out && env.action() == onward))
            return reject(Result::malformed);
        // Both continuations resume the original logical ingress, even if it is
        // temporarily disconnected. The saved original stack is authoritative.
        auto base = env.original();
        SwitchFrameHeader header;
        auto view = view_of(base, frame, header);
        const auto& program = origins_.at(name->second).normal;
        const auto* rule = program.mapping(view);
        if (!rule) return reject(Result::missing);
        if (rule->type == RuleStatement::Type::policy) return reject(Result::policy);
        Stack rewritten;
        if (!rule->stack.apply(view, rewritten.values, rewritten.size)) return reject(Result::overflow);
        EcmpSelector selector(view);
        bool connected = false;
        for (const auto& port : ports_) {
            if (!route_port_matches(rule->output.port, port) || !live(port)) continue;
            connected = true;
            if (port == config_->input || port == config_->output ||
                !program.allowed(rule, view, port, rewritten.values.data(), rewritten.size)) continue;
            if (selector.consider(ecmp_port_identity(port), port)) d.target = &port;
        }
        if (!d.target) return reject(connected ? Result::policy : Result::missing);
        d.labels = rewritten;
        if (adapter_out && !codec_.attach(rewritten, env.body, d.labels)) return reject(Result::overflow);
        d.exit = rules_->role(*d.target, RuleStatement::Type::exit);
        d.multipath = selector.multipath();
        d.result = Result::forward;
        return d;
    }
};
} // namespace tuntom::divert
