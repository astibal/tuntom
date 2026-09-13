#pragma once

#include "switch_routes.hpp"
#include <array>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>

namespace tuntom {

constexpr std::size_t ruleset_max_bytes = 1024 * 1024;
constexpr std::size_t ruleset_max_statements = 4096;

inline std::uint64_t rules_number(const std::string &s) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("expected an unsigned decimal integer");
    std::uint64_t n = 0;
    for (char c : s) {
        const auto digit = static_cast<unsigned>(c - '0');
        if (n > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            throw std::runtime_error("integer exceeds uint64");
        n = n * 10 + digit;
    }
    return n;
}

inline void rules_port(const std::string &s) {
    (void)encode_switch_registration(s);
    if (s.find_first_of(",[]#") != std::string::npos ||
        (s.find('*') != std::string::npos && s.find('*') != s.size() - 1))
        throw std::runtime_error("invalid port pattern (only one trailing '*' is supported)");
}

struct RuleLabel {
    bool any = true;
    std::uint64_t value = 0;
    bool matches(std::uint64_t n) const { return any || value == n; }
    std::string text() const { return any ? "*" : std::to_string(value); }
};
struct RuleEndpoint {
    std::string port = "*";
    RuleLabel label;
    std::string text() const { return port + "," + label.text(); }
};
struct RuleStack {
    struct Item { bool keep = false; std::uint64_t value = 0; };
    std::vector<Item> items;
    bool rest = false;

    bool apply(const SwitchFrameView &frame, std::array<std::uint64_t, switch_max_labels> &out,
               std::size_t &count) const {
        count = items.size();
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].keep && i >= frame.label_count) return false;
            out[i] = items[i].keep ? frame.label(i) : items[i].value;
        }
        if (rest)
            for (std::size_t i = items.size(); i < frame.label_count; ++i) out[count++] = frame.label(i);
        return count > 0 && count <= switch_max_labels;
    }
    std::string text() const {
        std::string s = "[";
        for (const auto &item : items) {
            if (s.size() > 1) s += ", ";
            s += item.keep ? "keep" : std::to_string(item.value);
        }
        if (rest) s += ", ...";
        return s + "]";
    }
};

struct RuleStatement {
    enum class Type { exit, trunk, policy, mapping, capture } type = Type::policy;
    RuleEndpoint input, output;
    RuleStack stack;
    bool allow = false;
    std::string id, target;
    std::size_t line = 0;

    std::string text() const {
        std::string s;
        switch (type) {
        case Type::exit: return "exit " + output.port;
        case Type::trunk: return "trunk " + output.port;
        case Type::mapping: s = "label " + input.text() + " to " + output.port + ", " + stack.text(); break;
        case Type::policy: s = "switch " + input.text() + " to " + output.text() + (allow ? " allow" : " drop"); break;
        case Type::capture: s = "switch " + input.text() + " to " + output.text() + " capture";
            if (!target.empty()) s += " " + target;
            break;
        }
        if (!id.empty()) s += " [id=" + id + "]";
        return s;
    }
};

struct SwitchRuleset {
    std::uint64_t serial = 0;
    std::vector<RuleStatement> statements;
    std::string text() const {
        std::string out = "format 1\nserial " + std::to_string(serial) + "\n";
        for (const auto &s : statements) out += s.text() + "\n";
        return out;
    }
    bool role(const std::string &port, RuleStatement::Type type) const {
        for (const auto &s : statements)
            if (s.type == type && route_port_matches(s.output.port, port)) return true;
        return false;
    }
};

// Line-local tokenization: '#' always begins a comment, including inside a token.
class RulesLine {
    std::vector<std::string> tokens_;
    std::size_t pos_ = 0;
  public:
    explicit RulesLine(std::string line) {
        line.erase(line.find('#') == std::string::npos ? line.size() : line.find('#'));
        for (unsigned char c : line)
            if (c != '\t' && c != '\r' && (c < 32 || c > 126)) throw std::runtime_error("expected printable ASCII");
        for (std::size_t i = 0; i < line.size();) {
            const unsigned char c = static_cast<unsigned char>(line[i]);
            if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
            if (c < 33 || c > 126) throw std::runtime_error("expected printable ASCII");
            if (line[i] == ',' || line[i] == '[' || line[i] == ']') {
                tokens_.push_back(line.substr(i++, 1)); continue;
            }
            const auto start = i++;
            while (i < line.size() && std::string_view(" \t\r,[]").find(line[i]) == std::string_view::npos) ++i;
            tokens_.push_back(line.substr(start, i - start));
        }
    }
    bool done() const { return pos_ == tokens_.size(); }
    std::string peek(std::size_t offset = 0) const { return pos_ + offset >= tokens_.size() ? "" : tokens_[pos_ + offset]; }
    std::string take() {
        if (done()) throw std::runtime_error("unexpected end of line");
        return tokens_[pos_++];
    }
    bool eat(const std::string &s) { if (peek() != s) return false; ++pos_; return true; }
    void need(const std::string &s) { if (!eat(s)) throw std::runtime_error("expected '" + s + "'"); }
    RuleEndpoint endpoint() {
        RuleEndpoint e; e.port = take(); rules_port(e.port);
        if (eat(",")) {
            const auto label = take();
            if (label != "*") e.label = {false, rules_number(label)};
        }
        return e;
    }
    RuleStack stack() {
        RuleStack result; need("[");
        do {
            const auto item = take();
            if (item == "...") { result.rest = true; break; }
            result.items.push_back({item == "keep", item == "keep" ? 0 : rules_number(item)});
            if (result.items.size() > switch_max_labels) throw std::runtime_error("at most 8 output labels");
        } while (eat(","));
        need("]");
        if (result.items.empty()) throw std::runtime_error("stack needs at least one explicit label or keep");
        return result;
    }
    void options(RuleStatement &s) {
        if (!eat("[")) return;
        while (!eat("]")) {
            const auto option = take();
            if (option.compare(0, 3, "id=") != 0 || option.size() == 3 || !s.id.empty())
                throw std::runtime_error("unknown or duplicate option: " + option);
            s.id = option.substr(3);
            (void)eat(",");
        }
    }
};

inline std::shared_ptr<const SwitchRuleset> parse_switch_ruleset(const std::string &text) {
    if (text.size() > ruleset_max_bytes) throw std::runtime_error("ruleset exceeds 1 MiB");
    auto rules = std::make_shared<SwitchRuleset>();
    bool format = false, serial = false;
    std::istringstream input(text);
    std::string line;
    std::size_t number = 0;
    while (std::getline(input, line)) {
        ++number;
        try {
            RulesLine p(line);
            if (p.done()) continue;
            const auto command = p.take();
            if (command == "format") {
                if (format || serial || !rules->statements.empty()) throw std::runtime_error("format must appear once, first");
                if (rules_number(p.take()) != 1) throw std::runtime_error("unsupported ruleset format");
                format = true;
            } else if (command == "serial") {
                if (!format || serial || !rules->statements.empty()) throw std::runtime_error("serial must appear once, after format");
                rules->serial = rules_number(p.take()); serial = true;
            } else {
                if (!serial) throw std::runtime_error("format and serial headers are required first");
                RuleStatement s; s.line = number;
                if (command == "exit" || command == "trunk") {
                    s.type = command == "exit" ? RuleStatement::Type::exit : RuleStatement::Type::trunk;
                    s.output.port = p.take(); rules_port(s.output.port);
                } else if (command == "label") {
                    s.type = RuleStatement::Type::mapping; s.input = p.endpoint(); p.need("to");
                    s.output.port = p.take(); rules_port(s.output.port); p.need(","); s.stack = p.stack();
                    p.options(s);
                } else if (command == "switch" || command == "drop") {
                    if (command == "switch") {
                        if ((p.peek() != "to" || p.peek(1) == ",") && ((p.peek() != "allow" && p.peek() != "drop" && p.peek() != "capture") ||
                            p.peek(1) == "," || p.peek(1) == "to"))
                            s.input = p.endpoint();
                        if (p.eat("to")) s.output = p.endpoint();
                        const auto action = p.take();
                        if (action == "allow") s.allow = true;
                        else if (action == "capture") {
                            s.type = RuleStatement::Type::capture;
                            if (!p.done() && p.peek() != "[") { s.target = p.take(); rules_port(s.target); }
                        } else if (action != "drop") throw std::runtime_error("expected allow, drop or capture");
                    }
                    p.options(s);
                } else throw std::runtime_error("unknown directive: " + command);
                if (!s.id.empty())
                    for (const auto &existing : rules->statements)
                        if (existing.id == s.id) throw std::runtime_error("duplicate rule id: " + s.id);
                rules->statements.push_back(std::move(s));
                if (rules->statements.size() > ruleset_max_statements) throw std::runtime_error("too many statements (maximum 4096)");
            }
            if (!p.done()) throw std::runtime_error("unexpected token: " + p.take());
        } catch (const std::runtime_error &e) {
            throw std::runtime_error("line " + std::to_string(number) + ": " + e.what());
        }
    }
    if (!format || !serial) throw std::runtime_error("format and serial headers are required");
    for (const auto &s : rules->statements) {
        if (s.type == RuleStatement::Type::capture)
            throw std::runtime_error("line " + std::to_string(s.line) + ": capture is not supported yet");
        if (s.type == RuleStatement::Type::exit)
            for (const auto &t : rules->statements)
                if (t.type == RuleStatement::Type::trunk) {
                    auto a = s.output.port, b = t.output.port;
                    if (wildcard_port(a)) a.pop_back();
                    if (wildcard_port(b)) b.pop_back();
                    if (route_port_matches(s.output.port, b) || route_port_matches(t.output.port, a))
                        throw std::runtime_error("overlapping exit and trunk roles");
                }
    }
    if (rules->text().size() > ruleset_max_bytes) throw std::runtime_error("canonical ruleset exceeds 1 MiB");
    return rules;
}

inline std::string read_rules_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open rules file: " + path);
    std::string result;
    std::array<char, 16384> buffer{};
    while (in.read(buffer.data(), buffer.size()) || in.gcount()) {
        result.append(buffer.data(), static_cast<std::size_t>(in.gcount()));
        if (result.size() > ruleset_max_bytes) throw std::runtime_error("ruleset exceeds 1 MiB");
    }
    if (!in.eof()) throw std::runtime_error("cannot read rules file: " + path);
    return result;
}

// Compile ingress port selectors once. Numeric matches retain FILE ORDER.
struct RulesProgram {
    std::vector<const RuleStatement *> mappings, policies;
    std::unordered_map<std::uint64_t, const RuleStatement *> exact;
    const RuleStatement *fallback = nullptr;
    RulesProgram() = default;
    RulesProgram(const SwitchRuleset &rules, const std::string &port) {
        for (const auto &s : rules.statements) {
            if (!route_port_matches(s.input.port, port)) continue;
            if (s.type == RuleStatement::Type::mapping && !fallback) {
                if (s.input.label.any) { fallback = &s; mappings.push_back(&s); }
                else if (exact.emplace(s.input.label.value, &s).second) mappings.push_back(&s);
            }
            if (s.type == RuleStatement::Type::policy) policies.push_back(&s);
        }
    }
    const RuleStatement *mapping(std::uint64_t label) const {
        const auto found = exact.find(label);
        return found == exact.end() ? fallback : found->second;
    }
    bool allowed(std::uint64_t input, const std::string &port, std::uint64_t output) const {
        for (const auto *s : policies)
            if (s->input.label.matches(input) && s->output.label.matches(output) &&
                route_port_matches(s->output.port, port)) return s->allow;
        return false;
    }
};

inline void rewrite_rules_frame(std::uint8_t *data, std::size_t &size, const SwitchFrameView &frame,
                               const std::array<std::uint64_t, switch_max_labels> &labels,
                               std::size_t count, bool exit) {
    const auto header = switch_base_header_size + count * switch_label_size;
    std::memmove(data + header, frame.payload, frame.payload_size);
    for (std::size_t i = 0; i < count; ++i) store_be64(data + switch_base_header_size + i * switch_label_size, labels[i]);
    data[1] = static_cast<std::uint8_t>(exit ? SwitchOpcode::exit_packet : SwitchOpcode::switch_packet);
    data[3] = static_cast<std::uint8_t>(count);
    size = header + frame.payload_size;
    store_be32(data + 4, static_cast<std::uint32_t>(size));
}

inline bool ruleset_changed(const std::shared_ptr<const SwitchRuleset> &current, const SwitchRuleset &next) {
    if (!current) return true;
    if (next.serial < current->serial) throw std::runtime_error("serial is older than the active ruleset");
    if (next.serial == current->serial) {
        if (next.text() != current->text()) throw std::runtime_error("different configuration with the same serial");
        return false;
    }
    return true;
}

} // namespace tuntom
