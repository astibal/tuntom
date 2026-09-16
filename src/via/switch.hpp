#pragma once

#include "codec.hpp"
#include "registration.hpp"
#include "../divert/switch.hpp"
#include <sys/random.h>

namespace tuntom::via {
using divert::Decision;
using divert::Result;
// Control-plane state only. IDs are never reused during a switch lifetime.
struct State {
    std::uint32_t cookie = 0, next = 0;
    using Rules = std::weak_ptr<const SwitchRuleset>;
    std::map<Rules, std::map<std::string, std::uint32_t>, std::owner_less<Rules>> generations;
    State() {
        static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        for (unsigned i = 0; i < 3;) {
            unsigned char byte;
            ssize_t n;
            do { n = ::getrandom(&byte, 1, 0); } while (n < 0 && errno == EINTR);
            if (n != 1) throw std::runtime_error("cannot generate VIA cookie");
            if (byte >= 208) continue;
            cookie = (cookie << 8) | alphabet[byte % 52]; ++i;
        }
    }
    void maintain() {
        for (auto it = generations.begin(); it != generations.end();)
            if (it->first.expired()) it = generations.erase(it); else ++it;
    }
    std::uint32_t identify(const std::shared_ptr<const SwitchRuleset>& rules, const std::string& key) {
        auto& ids = generations[rules];
        const auto found = ids.find(key);
        if (found != ids.end()) return found->second;
        if (next == UINT32_MAX) throw std::runtime_error("VIA chain IDs exhausted");
        const auto id = ++next;
        ids.emplace(key, id); return id;
    }
};
inline bool enabled(const std::shared_ptr<const SwitchRuleset>& rules) { return rules && rules->format == 3; }
inline bool accepted(const SwitchRuleset& rules, const std::string& name) {
    if (!reserved(name)) return true;
    Registration r;
    if (!registration(name, r)) return false;
    unsigned matches = 0;
    for (const auto& item : rules.services) {
        const auto& service = item.second;
        if (route_port_matches(r.server ? service.server : service.client, r.attachment) &&
            (service.instances.empty() || std::find(service.instances.begin(), service.instances.end(), r.instance) != service.instances.end())) ++matches;
    }
    return matches == 1;
}

inline bool compatible(const SwitchRuleset& rules, const std::string& incoming, int fd,
                       const std::string& existing, int other) {
    if (!compatible(incoming, fd, existing, other)) return false;
    Registration a, b;
    if (!registration(incoming, a) || !registration(existing, b) || a.instance != b.instance) return true;
    for (const auto& item : rules.services) {
        const auto& service = item.second;
        if (route_port_matches(a.server ? service.server : service.client, a.attachment) &&
            route_port_matches(b.server ? service.server : service.client, b.attachment)) return true;
    }
    return false;
}

class SwitchPath {
    struct Instance {
        std::string id, client, server;
        std::uint64_t hash;
    };
    struct Service { const ViaService* config; std::vector<Instance> instances; };
    struct Chain {
        std::uint32_t id;
        std::string origin;
        std::uint64_t origin_id;
        const RuleStatement* rule;
        const RulesProgram* program;
        const RuleStatement* match; // Null for a permanent rule.
        std::vector<const Service*> services;
    };
    std::shared_ptr<const SwitchRuleset> rules_;
    std::shared_ptr<const divert::Config> adhoc_;
    std::uint32_t cookie_;
    std::vector<std::string> ports_;
    std::map<std::string, RulesProgram> programs_;
    std::map<std::string, Service> services_;
    std::map<std::uint32_t, Chain> chains_;
    std::map<std::string, std::vector<const Chain*>> ingress_;

public:
    SwitchPath(State& state, std::shared_ptr<const SwitchRuleset> rules,
               std::shared_ptr<const divert::Config> adhoc, std::vector<std::string> ports)
        : rules_(std::move(rules)), adhoc_(std::move(adhoc)), cookie_(state.cookie), ports_(std::move(ports)) {
        if (!enabled(rules_)) throw std::runtime_error("VIA requires rules format 3");
        if (adhoc_ && !adhoc_->via) throw std::runtime_error("format 3 requires a format-3 divert file");
        state.maintain();
        for (const auto& item : rules_->services) {
            const auto& config = item.second;
            Service service{&config, {}};
            std::map<std::string, Instance> pairs;
            for (const auto& port : ports_) {
                Registration r;
                if (!registration(port, r) || !route_port_matches(r.server ? config.server : config.client, r.attachment)) continue;
                if (!config.instances.empty() && std::find(config.instances.begin(), config.instances.end(), r.instance) == config.instances.end()) continue;
                auto& pair = pairs[r.instance]; pair.id = r.instance; pair.hash = ecmp_port_identity(r.instance);
                (r.server ? pair.server : pair.client) = port;
            }
            const auto append = [&](const Instance& pair) {
                if (!pair.client.empty() && !pair.server.empty()) service.instances.push_back(pair);
            };
            if (config.failover) {
                for (const auto& id : config.instances) { const auto p = pairs.find(id); if (p != pairs.end()) append(p->second); }
            } else for (const auto& pair : pairs) append(pair.second);
            services_.emplace(item.first, std::move(service));
        }
        for (const auto& port : ports_) programs_.emplace(port, RulesProgram(*rules_, port));
        for (const auto& origin : rules_->origins) programs_.emplace(origin.first, RulesProgram(*rules_, origin.first));
        for (const auto& rule : rules_->statements) {
            if (!rule.via.empty() && !wildcard_port(rule.input.port) && rule.input.port != "*" && !rules_->origins.count(rule.input.port))
                throw std::runtime_error("VIA ingress needs a stable port ID: " + rule.input.port);
        }
        if (adhoc_) for (const auto& match : adhoc_->via_matches)
            for (const auto& name : match.via) if (!services_.count(name)) throw std::runtime_error("unknown divert service: " + name);
        std::size_t work = 0;
        for (const auto& origin : rules_->origins) {
            const auto& program = programs_.at(origin.first);
            const auto add = [&](const std::vector<std::string>& names, const RuleStatement* rule, const RuleStatement* match) {
                if (++work > ruleset_max_statements) throw std::runtime_error("too many compiled VIA chains (maximum 4096)");
                std::string key = "origin=" + origin.first;
                key += match ? "\nadhoc=" + match->input.text_v2() :
                    "\nrule=" + std::to_string(rule - rules_->statements.data());
                Chain chain{0, origin.first, origin.second, rule, &program, match, {}};
                for (const auto& name : names) { key += "\nservice=" + name; chain.services.push_back(&services_.at(name)); }
                chain.id = state.identify(rules_, key);
                auto entry = chains_.emplace(chain.id, std::move(chain));
                ingress_[origin.first].push_back(&entry.first->second);
            };
            if (adhoc_) for (const auto& match : adhoc_->via_matches)
                if (route_port_matches(match.input.port, origin.first)) add(match.via, nullptr, &match);
            for (const auto* rule : program.mappings)
                if (!rule->via.empty()) add(rule->via, rule, nullptr);
        }
    }
    bool possible(const std::string& from, const std::string& to) const {
        for (const auto& item : chains_) {
            const auto& chain = item.second;
            const auto output_port = [&](const std::string& port) {
                if (reserved(port)) return false;
                if (chain.rule) return route_port_matches(chain.rule->output.port, port);
                for (const auto* rule : chain.program->mappings)
                    if (route_port_matches(rule->output.port, port)) return true;
                return false;
            };
            const auto output = [&] { return output_port(to); };
            const auto side = [&](std::size_t begin, std::size_t end, bool server) {
                for (auto i = begin; i < end; ++i)
                    for (const auto& instance : chain.services[i]->instances)
                        if (to == (server ? instance.server : instance.client)) return true;
                return false;
            };
            if (from == chain.origin && (output() || side(0, chain.services.size(), false))) return true;
            if (rules_->role(from, RuleStatement::Type::exit) && output_port(from) &&
                (to == chain.origin || side(0, chain.services.size(), true))) return true;
            for (std::size_t i = 0; i < chain.services.size(); ++i) {
                for (const auto& instance : chain.services[i]->instances) {
                    if (from == instance.client && (to == chain.origin || output() || side(0, i, true))) return true;
                    if (from == instance.server && (output() || side(i + 1, chain.services.size(), false))) return true;
                }
            }
        }
        return false;
    }
    template<class Live> Decision route(const std::string& physical, const SwitchFrameView& frame, Live live) const {
        Decision d;
        const auto reject = [&](Result result) { d.result = result; return d; };
        Envelope env;
        if (!Codec::split(labels_of(frame), env)) return reject(Result::malformed);
        const Chain* chain = nullptr;
        int next = 0;
        bool skip_chain = false;
        if (!env.present) {
            if (reserved(physical)) return reject(Result::malformed);
            const auto input = ingress_.find(physical);
            if (input == ingress_.end()) {
                const auto program = programs_.find(physical);
                const auto* rule = program == programs_.end() ? nullptr : program->second.mapping(frame);
                if (rule && !rule->via.empty()) return reject(Result::missing);
                if (adhoc_ && adhoc_->enabled.load(std::memory_order_acquire))
                    for (const auto& match : adhoc_->via_matches)
                        if (route_port_matches(match.input.port, physical) && match.input.match.matches(frame)) return reject(Result::missing);
                return d;
            }
            const RuleStatement* selected = nullptr;
            bool evaluated = false;
            for (const auto* candidate : input->second) {
                if (candidate->match) {
                    if (!adhoc_->enabled.load(std::memory_order_acquire) || !candidate->match->input.match.matches(frame)) continue;
                } else {
                    if (!evaluated) { selected = programs_.at(physical).mapping(frame); evaluated = true; }
                    if (candidate->rule != selected) continue;
                }
                chain = candidate; break;
            }
            if (!chain) return d;
            env.saved = env.base; env.cookie = cookie_; env.chain = chain->id; env.origin_id = chain->origin_id;
        } else {
            const auto found = chains_.find(env.chain);
            if (env.cookie != cookie_ || found == chains_.end()) return reject(Result::malformed);
            chain = &found->second;
            const auto& input_match = chain->match ? chain->match->input.match : chain->rule->input.match;
            if (env.origin_id != chain->origin_id || !input_match.matches(env.saved.values.data(), env.saved.size)) return reject(Result::malformed);
            if (env.action == complete) {
                SwitchFrameHeader original_header;
                const auto original = view_of(env.saved, frame, original_header);
                const auto* mapping = chain->rule ? chain->rule : chain->program->mapping(original);
                if (env.reverse || env.step != UINT16_MAX || reserved(physical) ||
                    !mapping || mapping->type != RuleStatement::Type::forward ||
                    !rules_->role(physical, RuleStatement::Type::exit) || !route_port_matches(mapping->output.port, physical)) return reject(Result::malformed);
                env.reverse = true; next = static_cast<int>(chain->services.size()) - 1;
            } else {
                if (env.step >= chain->services.size()) return reject(Result::malformed);
                const auto& service = *chain->services[env.step];
                const Instance* instance = nullptr;
                bool client = false;
                for (const auto& member : service.instances) {
                    if (physical == member.client) { instance = &member; client = true; break; }
                    if (physical == member.server) { instance = &member; break; }
                }
                if (!instance || !live(instance->client) || !live(instance->server)) return reject(Result::missing);
                if (env.action == bypass && client && !env.reverse) skip_chain = true;
                else if (env.action != onward || env.reverse != client) return reject(Result::malformed);
                next = static_cast<int>(env.step) + (env.reverse ? -1 : 1);
            }
        }
        if (!skip_chain) {
            for (; next >= 0 && next < static_cast<int>(chain->services.size()); next += env.reverse ? -1 : 1) {
                const auto& service = *chain->services[next];
                const Instance* selected = nullptr;
                EcmpSelector selector(frame);
                for (const auto& member : service.instances) {
                    if (!live(member.client) || !live(member.server)) continue;
                    if (service.config->failover) { selected = &member; break; }
                    if (selector.consider(member.hash, member.id)) selected = &member;
                }
                if (!selected) {
                    if (service.config->pass) continue;
                    return reject(Result::missing);
                }
                env.step = static_cast<std::uint16_t>(next); env.action = offer;
                if (!Codec::attach(env, d.labels)) return reject(Result::overflow);
                d.target = env.reverse ? &selected->server : &selected->client;
                d.exit = true; d.result = Result::forward; d.multipath = selector.multipath(); return d;
            }
        }
        if (env.reverse) {
            if (!live(chain->origin)) return reject(Result::missing);
            d.target = &chain->origin; d.labels = env.saved;
            d.result = Result::forward; return d;
        }
        SwitchFrameHeader header;
        const auto original = view_of(env.saved, frame, header);
        const auto* mapping = chain->rule ? chain->rule : chain->program->mapping(original);
        if (!mapping) return reject(Result::missing);
        if (mapping->type != RuleStatement::Type::forward) return reject(Result::policy);
        Stack output;
        if (!mapping->stack.apply(original, output.values, output.size)) return reject(Result::overflow);
        EcmpSelector selector(frame);
        for (const auto& port : ports_) {
            if (reserved(port) || !live(port) || !route_port_matches(mapping->output.port, port) ||
                !chain->program->allowed(mapping, original, port, output.values.data(), output.size)) continue;
            if (selector.consider(ecmp_port_identity(port), port)) d.target = &port;
        }
        if (!d.target) return reject(Result::missing);
        d.exit = rules_->role(*d.target, RuleStatement::Type::exit);
        if (skip_chain) d.labels = output;
        else {
            // Local VIA requires an exit that retains the complete label stack.
            if (!d.exit) return reject(Result::policy);
            env.base = output; env.step = UINT16_MAX; env.action = complete;
            if (!Codec::attach(env, d.labels)) return reject(Result::overflow);
        }
        d.multipath = selector.multipath(); d.result = Result::forward; return d;
    }
};
} // namespace tuntom::via
