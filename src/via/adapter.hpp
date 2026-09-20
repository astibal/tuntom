#pragma once

#include "codec.hpp"
#include <optional>
#include <variant>

namespace tuntom::via {
struct AdapterContext {
    std::variant<divert::Envelope, Envelope> value;
    void write_flow(std::ostream& out, const char* prefix) const {
        std::visit([&](const auto& env) { env.write_flow(out, prefix); }, value);
    }
    bool same_context(const AdapterContext& other) const {
        if (value.index() != other.value.index()) return false;
        if (const auto* env = std::get_if<Envelope>(&value)) return env->same_context(std::get<Envelope>(other.value));
        return std::get<divert::Envelope>(value).same_context(std::get<divert::Envelope>(other.value));
    }
};
// One adapter event loop and admission implementation for both wire formats.
class AdapterCodec {
    bool via_;
    std::optional<divert::Codec> legacy_;
public:
    AdapterCodec(bool via, const std::string& cookie) : via_(via) {
        if (!via_) legacy_.emplace(cookie);
        else if (!cookie.empty()) throw std::runtime_error("VIA cookies are automatic; omit --cookie");
    }
    bool receive(const Stack& labels, unsigned side, AdapterContext& context) const {
        if (via_) {
            Envelope env;
            if (!Codec::split(labels, env) || !env.present || env.action != offer || env.reverse != (side == 1) || env.step == UINT16_MAX) return false;
            context.value = env;
        } else {
            divert::Envelope env;
            if (!legacy_->split(labels, env) || !env.present || env.action() != (side == 0 ? divert::offered : divert::onward)) return false;
            context.value = env;
        }
        return true;
    }
    bool attach(const AdapterContext& context, Stack& output) const {
        if (via_) return Codec::attach(std::get<Envelope>(context.value), output);
        const auto& env = std::get<divert::Envelope>(context.value);
        return legacy_->attach(env.base, env.body, output);
    }
    void bypass(AdapterContext& context) const {
        if (via_) std::get<Envelope>(context.value).action = via::bypass;
        else std::get<divert::Envelope>(context.value).body.values[1] = divert::bypass;
    }
    void onward(AdapterContext& context, unsigned side) const {
        if (via_) {
            auto& env = std::get<Envelope>(context.value);
            env.reverse = side == 0; env.action = via::onward;
        } else std::get<divert::Envelope>(context.value).body.values[1] = side == 0 ? divert::to_client : divert::onward;
    }
};
} // namespace tuntom::via
