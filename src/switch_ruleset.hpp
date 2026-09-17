#pragma once

#include "switch_routes.hpp"
#include "switch_stack_match.hpp"
#include <array>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <map>
#include <set>

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
    validate_port_wildcard(s);
    if (s.find_first_of(",[]#") != std::string::npos)
        throw std::runtime_error("invalid port pattern (unexpected delimiter)");
}

inline std::uint64_t rules_label_value(const std::string &text) {
    if (!text.empty() && text.front() == '"') {
        if (text.size() < 2 || text.back() != '"') throw std::runtime_error("unterminated label string");
        std::uint64_t value = 0; std::size_t bytes = 0;
        for (std::size_t i = 1; i + 1 < text.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (c == '\\') {
                if (++i + 1 >= text.size() || (text[i] != '\\' && text[i] != '"'))
                    throw std::runtime_error("label strings support only escaped quote and backslash");
                c = static_cast<unsigned char>(text[i]);
            }
            if (++bytes > 8) throw std::runtime_error("label string exceeds 8 bytes");
            value = (value << 8) | c;
        }
        for (; bytes < 8; ++bytes) value <<= 8;
        return value;
    }
    unsigned base = 10; std::size_t start = 0;
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { base = 16; start = 2; }
    else if (text.size() >= 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) { base = 2; start = 2; }
    else if (!text.empty() && (text[0] == 'b' || text[0] == 'B')) { base = 2; start = 1; }
    if (start == text.size()) throw std::runtime_error("label literal needs digits");
    std::uint64_t value = 0;
    for (std::size_t i = start; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const unsigned digit = c >= '0' && c <= '9' ? c - '0' :
                               c >= 'a' && c <= 'f' ? c - 'a' + 10U :
                               c >= 'A' && c <= 'F' ? c - 'A' + 10U : 16U;
        if (digit >= base) throw std::runtime_error("invalid digit in label literal");
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / base)
            throw std::runtime_error("label literal exceeds uint64");
        value = value * base + digit;
    }
    return value;
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
    RuleStackMatch match;
    std::string text() const { return port + "," + label.text(); }
    std::string text_v2() const {
        std::string out = port == "*" ? "" : port;
        // A comma disambiguates source ports named after grammar keywords.
        const bool keyword = port == "to" || port == "allow" || port == "drop" || port == "capture";
        if (!match.unrestricted() || keyword) out += (out.empty() ? "" : ", ") + match.text();
        return out;
    }
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
    bool identity() const { return rest && items.size() == 1 && items[0].keep; }
    std::string text(bool stars = false) const {
        std::string s = "[";
        for (const auto &item : items) {
            if (s.size() > 1) s += ", ";
            s += item.keep ? (stars ? "*" : "keep") : std::to_string(item.value);
        }
        if (rest) s += ", ...";
        return s + "]";
    }
};

struct RuleStatement {
    enum class Type { exit, trunk, policy, mapping, capture, forward } type = Type::policy;
    RuleEndpoint input, output;
    RuleStack stack;
    bool allow = false;
    std::string id, target;
    std::vector<std::string> via;
    std::size_t line = 0;

    std::string text(unsigned format = 1) const {
        if (format >= 2 && type != Type::exit && type != Type::trunk) {
            std::string out = "switch";
            const auto source = input.text_v2();
            if (!source.empty()) out += " " + source;
            std::string destination;
            if (type == Type::forward) {
                if (output.port != "*") destination = output.port;
                if (!stack.identity()) destination += (destination.empty() ? "" : ", ") + stack.text(true);
            } else destination = output.text_v2();
            if (!destination.empty()) out += " to " + destination;
            if (!via.empty()) {
                out += " via [";
                for (const auto& name : via) { if (out.back() != '[') out += ", "; out += name; }
                out += "]";
            }
            out += type == Type::forward ? " allow" : type == Type::capture ? " capture" : " drop";
            if (!target.empty()) out += " " + target;
            if (!id.empty()) out += " [id=" + id + "]";
            return out;
        }
        std::string s;
        switch (type) {
        case Type::exit: return "exit " + output.port;
        case Type::trunk: return "trunk " + output.port;
        case Type::mapping: s = "label " + input.text() + " to " + output.port + ", " + stack.text(); break;
        case Type::policy:
        case Type::capture:
            // Omitted selectors stay omitted: an explicit bare port '*' is invalid.
            s = "switch";
            if (input.port != "*") s += " " + input.text();
            if (output.port != "*") s += " to " + output.text();
            if (type == Type::policy) s += allow ? " allow" : " drop";
            else {
                s += " capture";
                if (!target.empty()) s += " " + target;
            }
            break;
        case Type::forward: throw std::runtime_error("format-2 forwarding rule in format 1");
        }
        if (!id.empty()) s += " [id=" + id + "]";
        return s;
    }
};

struct ViaService {
    std::string name, client, server, relay;
    bool failover = false, pass = false;
    std::vector<std::string> instances;
    std::string text() const {
        std::string out = "service " + name + " {\n    client-side " + client +
            "\n    server-side " + server + "\n    stickiness " + (failover ? "failover" : "hash") +
            "\n    unavailable " + (pass ? "pass" : "drop") + "\n";
        if (!relay.empty()) out += "    relay " + relay + "\n";
        if (!instances.empty()) {
            out += "    instances [";
            for (const auto& id : instances) { if (out.back() != '[') out += ", "; out += "\"" + id + "\""; }
            out += "]\n";
        }
        return out + "}\n";
    }
};

struct SwitchRuleset {
    unsigned format = 1;
    std::uint64_t serial = 0;
    std::vector<RuleStatement> statements;
    std::map<std::string, std::uint64_t> origins;
    std::map<std::string, ViaService> services;
    std::string text() const {
        std::string out = "format " + std::to_string(format) + "\nserial " + std::to_string(serial) + "\n";
        for (const auto& port : origins) out += "port " + port.first + " id " + std::to_string(port.second) + "\n";
        for (const auto& service : services) out += service.second.text();
        for (const auto &s : statements) out += s.text(format) + "\n";
        return out;
    }
    bool role(const std::string &port, RuleStatement::Type type) const {
        for (const auto &s : statements)
            if (s.type == type && route_port_matches(s.output.port, port)) return true;
        return false;
    }
};

// Line-local tokenization: format 2 protects quoted label strings from comments.
class RulesLine {
    std::vector<std::string> tokens_;
    std::size_t pos_ = 0;
  public:
    explicit RulesLine(std::string line, bool stack_syntax = false) {
        if (stack_syntax) {
            const std::string_view separators = " \t\r,[]<>&#";
            for (std::size_t i = 0; i < line.size();) {
                if (line[i] == '#') break;
                if (line[i] == ' ' || line[i] == '\t' || line[i] == '\r') { ++i; continue; }
                const auto start = i;
                if (line[i] == '"') {
                    ++i; bool closed = false;
                    while (i < line.size()) {
                        const auto c = static_cast<unsigned char>(line[i++]);
                        if (c < 32 || c == 127) throw std::runtime_error("label string cannot contain control bytes");
                        if (c == '"') { closed = true; break; }
                        if (c == '\\') {
                            if (i == line.size() || (line[i] != '\\' && line[i] != '"'))
                                throw std::runtime_error("label strings support only escaped quote and backslash");
                            ++i;
                        }
                    }
                    if (!closed) throw std::runtime_error("unterminated label string");
                    if (i < line.size() && separators.find(line[i]) == std::string_view::npos)
                        throw std::runtime_error("expected separator after label string");
                } else if (std::string_view(",[]<>&").find(line[i]) != std::string_view::npos) ++i;
                else {
                    while (i < line.size() && separators.find(line[i]) == std::string_view::npos) {
                        const auto c = static_cast<unsigned char>(line[i++]);
                        if (c < 33 || c > 126 || c == '"') throw std::runtime_error("invalid token in ruleset");
                    }
                }
                tokens_.push_back(line.substr(start, i - start));
            }
            return;
        }
        line.erase(line.find('#') == std::string::npos ? line.size() : line.find('#'));
        for (unsigned char c : line)
            if (c != '\t' && c != '\r' && (c < 32 || c > 126)) throw std::runtime_error("expected printable ASCII");
        const std::string_view punctuation = ",[]";
        for (std::size_t i = 0; i < line.size();) {
            const unsigned char c = static_cast<unsigned char>(line[i]);
            if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
            if (c < 33 || c > 126) throw std::runtime_error("expected printable ASCII");
            if (punctuation.find(line[i]) != std::string_view::npos) {
                tokens_.push_back(line.substr(i++, 1)); continue;
            }
            const auto start = i++;
            while (i < line.size() && std::string_view(" \t\r").find(line[i]) == std::string_view::npos &&
                   punctuation.find(line[i]) == std::string_view::npos) ++i;
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
    std::vector<std::string> names(bool quoted = false) {
        std::vector<std::string> out;
        need("[");
        do {
            auto name = take();
            if (quoted && name.size() >= 2 && name.front() == '"' && name.back() == '"')
                name = name.substr(1, name.size() - 2);
            if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.#") != std::string::npos)
                throw std::runtime_error("invalid VIA name");
            if (std::find(out.begin(), out.end(), name) != out.end()) throw std::runtime_error("duplicate VIA name");
            out.push_back(name);
            if (out.size() > ruleset_max_statements) throw std::runtime_error("too many VIA names");
        } while (eat(","));
        need("]");
        return out;
    }
    RuleEndpoint endpoint() {
        RuleEndpoint e; e.port = take(); rules_port(e.port);
        if (eat(",")) {
            const auto label = take();
            if (label != "*") e.label = {false, rules_number(label)};
        }
        return e;
    }
    RuleElement element() {
        if (eat("*")) return {};
        if (eat("&")) return {RuleElement::Kind::bits, rules_label_value(take()), 0};
        if (eat("<")) {
            const auto low = rules_label_value(take()); need(",");
            const auto high = rules_label_value(take()); need(">");
            if (low > high) throw std::runtime_error("range lower bound exceeds upper bound");
            return {low == high ? RuleElement::Kind::exact : RuleElement::Kind::range, low, high};
        }
        return {RuleElement::Kind::exact, rules_label_value(take()), 0};
    }
    RuleStackMatch stack_match() {
        RuleStackMatch match; match.items.clear(); match.rest = false;
        if (!eat("[")) { match.items.push_back(element()); return match; }
        do {
            if (eat("...")) { match.rest = true; break; }
            match.items.push_back(element());
            if (match.items.size() > switch_max_labels) throw std::runtime_error("at most 8 match elements");
        } while (eat(","));
        need("]");
        if (match.items.empty()) throw std::runtime_error("stack match needs at least one element");
        return match;
    }
    RuleEndpoint endpoint_v2() {
        RuleEndpoint endpoint;
        if (peek() == "[") { endpoint.match = stack_match(); return endpoint; }
        endpoint.port = take(); rules_port(endpoint.port);
        if (eat(",")) endpoint.match = stack_match();
        return endpoint;
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

inline RuleStack output_stack(const RuleStackMatch &match) {
    RuleStack stack; stack.rest = match.rest;
    for (const auto &element : match.items) {
        if (element.kind != RuleElement::Kind::any && element.kind != RuleElement::Kind::exact)
            throw std::runtime_error("output stack accepts only values and '*'; ranges and bitmasks are match predicates");
        stack.items.push_back({element.kind == RuleElement::Kind::any, element.value});
    }
    return stack;
}

inline RuleStatement reverse_forward_rule(const RuleStatement &forward) {
    RuleStatement reverse = forward;
    reverse.input.port = forward.output.port;
    reverse.output.port = forward.input.port;
    const auto &source = forward.input.match;
    const auto &rewrite = forward.stack;
    auto &match = reverse.input.match;
    match.items.clear();
    match.rest = source.rest && rewrite.rest;
    const auto count = rewrite.rest ? std::max(source.items.size(), rewrite.items.size()) : rewrite.items.size();
    for (std::size_t i = 0; i < count; ++i) {
        if (i < rewrite.items.size() && !rewrite.items[i].keep)
            match.items.push_back({RuleElement::Kind::exact, rewrite.items[i].value, 0});
        else if (i < source.items.size()) match.items.push_back(source.items[i]);
        else {
            if (!source.rest)
                throw std::runtime_error("bidir cannot preserve a missing source position; write explicit switch rules");
            match.items.push_back({});
        }
    }
    reverse.stack.items.clear();
    reverse.stack.rest = source.rest && rewrite.rest;
    for (std::size_t i = 0; i < source.items.size(); ++i) {
        const auto &element = source.items[i];
        if (element.kind == RuleElement::Kind::exact) reverse.stack.items.push_back({false, element.value});
        else {
            const bool preserved = i < rewrite.items.size() ? rewrite.items[i].keep : rewrite.rest;
            if (!preserved)
                throw std::runtime_error("bidir cannot restore a wildcard, range or bitmask after overwrite; write explicit switch rules");
            reverse.stack.items.push_back({true, 0});
        }
    }
    return reverse;
}

inline std::shared_ptr<const SwitchRuleset> parse_switch_ruleset(const std::string &text) {
    if (text.size() > ruleset_max_bytes) throw std::runtime_error("ruleset exceeds 1 MiB");
    auto rules = std::make_shared<SwitchRuleset>();
    bool format = false, serial = false;
    std::istringstream input(text);
    std::string line;
    std::size_t number = 0;
    ViaService service;
    bool in_service = false;
    std::set<std::string> service_fields;
    while (std::getline(input, line)) {
        ++number;
        try {
            RulesLine p(line, rules->format >= 2);
            if (p.done()) continue;
            const auto command = p.take();
            if (in_service) {
                if (command == "}") {
                    if (service.client.empty() || service.server.empty() ||
                        (service.failover && service.instances.empty()))
                        throw std::runtime_error("service requires two sides; failover requires ordered instances");
                    if (!rules->services.emplace(service.name, service).second) throw std::runtime_error("duplicate service");
                    in_service = false;
                } else {
                    if (!service_fields.insert(command).second) throw std::runtime_error("duplicate service field");
                    if (command == "client-side" || command == "server-side") {
                        auto value = p.take(); rules_port(value);
                        if (value.find("~via:") != std::string::npos) throw std::runtime_error("reserved VIA suffix");
                        (command == "client-side" ? service.client : service.server) = value;
                    } else if (command == "relay") {
                        service.relay = p.take(); rules_port(service.relay);
                        if (service.relay == "*" || wildcard_port(service.relay) || service.relay.find("~via:") != std::string::npos)
                            throw std::runtime_error("relay requires an exact physical port ID");
                    } else if (command == "stickiness") {
                        auto value = p.take();
                        if (value != "hash" && value != "failover") throw std::runtime_error("expected hash or failover");
                        service.failover = value == "failover";
                    } else if (command == "unavailable") {
                        auto value = p.take();
                        if (value != "drop" && value != "pass") throw std::runtime_error("expected drop or pass");
                        service.pass = value == "pass";
                    } else if (command == "instances") service.instances = p.names(true);
                    else throw std::runtime_error("unknown service field");
                }
            } else if (command == "format") {
                if (format || serial || !rules->statements.empty()) throw std::runtime_error("format must appear once, first");
                const auto version = rules_number(p.take());
                if (version != 1 && version != 2 && version != 3) throw std::runtime_error("unsupported ruleset format");
                rules->format = static_cast<unsigned>(version);
                format = true;
            } else if (command == "serial") {
                if (!format || serial || !rules->statements.empty()) throw std::runtime_error("serial must appear once, after format");
                rules->serial = rules_number(p.take()); serial = true;
            } else {
                if (!serial) throw std::runtime_error("format and serial headers are required first");
                RuleStatement s; s.line = number;
                bool bidir = false;
                if (rules->format == 3 && command == "service") {
                    service = {}; service.name = p.take();
                    if (service.name.empty() || service.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != std::string::npos)
                        throw std::runtime_error("invalid service name");
                    p.need("{");
                    if (!p.done()) throw std::runtime_error("service fields must be on separate lines");
                    service_fields.clear(); in_service = true; continue;
                } else if (rules->format == 3 && command == "port") {
                    auto name = p.take(); rules_port(name); p.need("id");
                    auto id = rules_number(p.take());
                    if (!id || wildcard_port(name) || name.find("~via:") != std::string::npos || !p.done())
                        throw std::runtime_error("expected port NAME id NONZERO_ID");
                    for (const auto& entry : rules->origins) if (entry.second == id) throw std::runtime_error("duplicate origin ID");
                    if (!rules->origins.emplace(name, id).second) throw std::runtime_error("duplicate origin port");
                    continue;
                } else if (command == "exit" || command == "trunk") {
                    s.type = command == "exit" ? RuleStatement::Type::exit : RuleStatement::Type::trunk;
                    s.output.port = p.take(); rules_port(s.output.port);
                } else if (command == "label") {
                    if (rules->format >= 2) throw std::runtime_error("format 2 uses switch for forwarding and rewriting; label is only supported in format 1");
                    s.type = RuleStatement::Type::mapping; s.input = p.endpoint(); p.need("to");
                    s.output.port = p.take(); rules_port(s.output.port); p.need(","); s.stack = p.stack();
                    p.options(s);
                } else if (command == "switch" || command == "drop") {
                    if (command == "switch") {
                        if ((p.peek() != "to" || p.peek(1) == ",") && ((p.peek() != "allow" && p.peek() != "drop" && p.peek() != "capture") ||
                            p.peek(1) == "," || p.peek(1) == "to"))
                            s.input = rules->format >= 2 ? p.endpoint_v2() : p.endpoint();
                        if (p.eat("to")) s.output = rules->format >= 2 ? p.endpoint_v2() : p.endpoint();
                        if (rules->format == 3 && p.eat("via")) s.via = p.names();
                        const auto action = p.take();
                        if (action == "allow") {
                            s.allow = true;
                            if (rules->format >= 2) {
                                s.type = RuleStatement::Type::forward;
                                s.stack = output_stack(s.output.match);
                            }
                        }
                        else if (action == "capture") {
                            s.type = RuleStatement::Type::capture;
                            if (!p.done() && p.peek() != "[" && p.peek() != "bidir") { s.target = p.take(); rules_port(s.target); }
                        } else if (action != "drop") throw std::runtime_error("expected allow, drop or capture");
                    }
                    bidir = p.eat("bidir");
                    p.options(s);
                } else throw std::runtime_error("unknown directive: " + command);
                const auto append = [&](RuleStatement statement) {
                    if (!statement.id.empty())
                        for (const auto &existing : rules->statements)
                            if (existing.id == statement.id) throw std::runtime_error("duplicate rule id: " + statement.id);
                    rules->statements.push_back(std::move(statement));
                    if (rules->statements.size() > ruleset_max_statements)
                        throw std::runtime_error("too many statements after expansion (maximum 4096)");
                };
                if (bidir) {
                    // Expand once, immediately after the original. Runtime and
                    // canonical export then use ordinary ordered statements.
                    auto reverse = s;
                    if (s.type == RuleStatement::Type::forward) reverse = reverse_forward_rule(s);
                    else std::swap(reverse.input, reverse.output);
                    // VIA carries its own reverse traversal. The ordinary reverse
                    // rule remains useful for traffic which bypasses the chain.
                    reverse.via.clear();
                    if (!reverse.id.empty()) reverse.id += ".reverse";
                    append(std::move(s));
                    append(std::move(reverse));
                } else append(std::move(s));
            }
            if (!p.done()) throw std::runtime_error("unexpected token: " + p.take());
        } catch (const std::runtime_error &e) {
            throw std::runtime_error("line " + std::to_string(number) + ": " + e.what());
        }
    }
    if (in_service) throw std::runtime_error("unterminated service block");
    if (!format || !serial) throw std::runtime_error("format and serial headers are required");
    for (const auto &s : rules->statements) {
        if (!s.via.empty() && s.type != RuleStatement::Type::forward)
            throw std::runtime_error("via requires an allow forwarding rule");
        for (const auto& name : s.via)
            if (!rules->services.count(name)) throw std::runtime_error("unknown VIA service: " + name);
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
    unsigned format = 1;
    std::vector<const RuleStatement *> mappings, policies;
    std::unordered_map<std::uint64_t, const RuleStatement *> exact;
    const RuleStatement *fallback = nullptr;
    RulesProgram() = default;
    RulesProgram(const SwitchRuleset &rules, const std::string &port) : format(rules.format) {
        for (const auto &s : rules.statements) {
            if (!route_port_matches(s.input.port, port)) continue;
            if (format >= 2) {
                if (s.type == RuleStatement::Type::forward) mappings.push_back(&s);
                if (s.type == RuleStatement::Type::forward || s.type == RuleStatement::Type::policy)
                    policies.push_back(&s);
                continue;
            }
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
    const RuleStatement *mapping(const SwitchFrameView &frame) const {
        if (format == 1) return mapping(frame.label(0));
        for (const auto *s : policies) {
            if (!s->input.match.matches(frame)) continue;
            // A destination-scoped drop filters the selected rule's candidates.
            if (s->type == RuleStatement::Type::forward ||
                (s->output.port == "*" && s->output.match.unrestricted())) return s;
        }
        return nullptr;
    }
    bool allowed(const RuleStatement *selected, const SwitchFrameView &input, const std::string &port,
                 const std::uint64_t *output, std::size_t count) const {
        if (format == 1) return allowed(input.label(0), port, output[0]);
        for (const auto *s : policies) {
            if (s == selected) return true;
            if (s->type == RuleStatement::Type::policy && s->input.match.matches(input) &&
                route_port_matches(s->output.port, port) && s->output.match.matches(output, count)) return false;
        }
        return false;
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
