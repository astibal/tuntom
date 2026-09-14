#pragma once

#include "ipc/switch_protocol.hpp"
#include <string>
#include <vector>

namespace tuntom {

struct RuleElement {
    enum class Kind { any, exact, range, bits } kind = Kind::any;
    std::uint64_t value = 0, upper = 0;

    bool matches(std::uint64_t label) const {
        switch (kind) {
        case Kind::any: return true;
        case Kind::exact: return label == value;
        case Kind::range: return value <= label && label <= upper;
        case Kind::bits: return (label & value) != 0;
        }
        return false;
    }
    std::string text() const {
        switch (kind) {
        case Kind::any: return "*";
        case Kind::exact: return std::to_string(value);
        case Kind::range: return "<" + std::to_string(value) + ", " + std::to_string(upper) + ">";
        case Kind::bits: return "&" + std::to_string(value);
        }
        return {};
    }
};

struct RuleStackMatch {
    // An omitted selector accepts every valid (nonempty) stack.
    std::vector<RuleElement> items{RuleElement{}};
    bool rest = true;

    bool unrestricted() const {
        return rest && items.size() == 1 && items[0].kind == RuleElement::Kind::any;
    }
    template<class LabelAt>
    bool matches(std::size_t count, LabelAt label) const {
        if (count < items.size() || (!rest && count != items.size())) return false;
        for (std::size_t i = 0; i < items.size(); ++i)
            if (!items[i].matches(label(i))) return false;
        return true;
    }
    bool matches(const SwitchFrameView &frame) const {
        return matches(frame.label_count, [&](std::size_t i) { return frame.label(i); });
    }
    bool matches(const std::uint64_t *labels, std::size_t count) const {
        return matches(count, [&](std::size_t i) { return labels[i]; });
    }
    std::string text() const {
        std::string out = "[";
        for (const auto &item : items) {
            if (out.size() > 1) out += ", ";
            out += item.text();
        }
        if (rest) out += ", ...";
        return out + "]";
    }
};

} // namespace tuntom
