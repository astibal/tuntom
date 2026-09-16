#include "../src/switch_ruleset.hpp"
#include <iostream>
#include <functional>
using namespace tuntom;
using namespace std::string_literals;

void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void fails(const std::function<void()> &action, const std::string &reason) {
    try { action(); } catch (const std::exception &e) {
        require(std::string(e.what()).find(reason) != std::string::npos, e.what()); return;
    }
    throw std::runtime_error("expected failure: " + reason);
}
std::shared_ptr<const SwitchRuleset> parse(const std::string &body) {
    return parse_switch_ruleset("format 1\nserial 42\n" + body);
}
void bidirectional_policies() {
    const auto expanded = parse("switch H42*,17 to exit0,99 allow [id=path]\n"
                                "switch exit0,99 to H42*,17 allow [id=path.reverse]\n"
                                "switch exit0,99 drop\n");
    auto rules = parse("switch H42*,17 to exit0,99 allow bidir [id=path] # comment\n"
                       "switch exit0,99 drop\n");
    require(rules->text() == expanded->text(), "bidir expands directly below original with both selectors swapped");
    require(rules->statements[0].line == rules->statements[1].line, "generated rule retains source line");
    require(RulesProgram(*rules, "H42_1").allowed(17, "exit0", 99), "bidir forward allow");
    require(RulesProgram(*rules, "exit0").allowed(99, "H42_2", 17), "reverse precedes following drop");
    require(!RulesProgram(*rules, "exit0").allowed(99, "H42_2", 18), "reverse swaps label selectors");
    require(!ruleset_changed(rules, *expanded), "bidir and explicit rules have equal canonical content");
    require(parse_switch_ruleset(rules->text())->text() == rules->text(), "export does not expand again");
    auto earlier = parse("switch exit0,99 drop\nswitch H42*,17 to exit0,99 allow bidir\n");
    require(!RulesProgram(*earlier, "exit0").allowed(99, "H42_1", 17), "earlier policy beats generated reverse");
    auto block = parse("switch blocked* drop bidir#comment\nswitch allow\n");
    require(!RulesProgram(*block, "blocked-1").allowed(7, "out", 9), "bidir drop on source");
    require(!RulesProgram(*block, "in").allowed(7, "blocked-2", 9), "bidir drop on destination");
    require(RulesProgram(*block, "in").allowed(7, "out", 9), "unrelated traffic falls through");
    auto omitted = parse("switch to exit*,99 allow bidir\n");
    require(RulesProgram(*omitted, "exit1").allowed(99, "anything", 12), "omitted source becomes wildcard destination");
    require(!RulesProgram(*omitted, "exit1").allowed(98, "anything", 12), "reversed exact label remains exact");
    require(parse("switch allow bidir")->statements.size() == 2, "symmetric rule still produces two statements");
    require(parse("drop bidir")->statements.size() == 2, "drop alias supports bidir");
    auto keyword = parse("switch bidir,1 to out,2 allow bidir\n");
    require(RulesProgram(*keyword, "out").allowed(2, "bidir", 1), "bidir remains valid as a port name");
    fails([] { parse("label in*,* to out,[keep,...] bidir"); }, "unexpected token: bidir");
    fails([] { parse("switch allow bidir bidir"); }, "unexpected token: bidir");
    fails([] { parse("switch allow [bidir]"); }, "unknown or duplicate option: bidir");
    fails([] { parse("switch allow [id=x,bidir]"); }, "unknown or duplicate option: bidir");
    fails([] { parse("switch allow [id=x] bidir"); }, "unexpected token: bidir");
    fails([] { parse("switch allow bidir=yes"); }, "unexpected token: bidir=yes");
    fails([] { parse("switch allow bidir [id=x]\nswitch drop [id=x.reverse]"); }, "duplicate rule id: x.reverse");
    fails([] { parse("switch drop [id=x.reverse]\nswitch allow bidir [id=x]"); }, "duplicate rule id: x.reverse");
    fails([] { parse("switch capture debug bidir [id=cap]"); }, "capture is not supported yet");
    fails([] { parse("switch capture bidir [id=cap]"); }, "capture is not supported yet");
    std::string full;
    for (std::size_t i = 0; i < ruleset_max_statements - 2; ++i) full += "switch drop\n";
    require(parse(full + "switch allow bidir")->statements.size() == ruleset_max_statements, "limit includes expanded rules");
    fails([&] { parse(full + "switch drop\nswitch allow bidir"); }, "maximum 4096");
}
void port_wildcards() {
    for (const auto *body : {"exit *", "trunk *", "switch * allow", "switch to * drop",
                            "switch *,17 to inet*,99 allow bidir",
                            "switch H42*,17 to *,99 allow bidir",
                            "label *,17 to inet*,[99,...]", "label H42*,17 to *,[99,...]",
                            "switch H42* capture *"})
        fails([&] { parse(body); }, "line 3: port wildcard '*' requires a non-empty prefix");

    const auto prefix = parse("exit inet*\nswitch H42*,* to inet*,* allow bidir\n"
                              "label H42*,* to inet*,[keep,...]\n");
    require(RulesProgram(*prefix, "H42").allowed(17, "inet", 99), "prefix accepts its literal name and any label");
    require(RulesProgram(*prefix, "inet1").allowed(99, "H42_1", 17), "prefix bidir reverse survives");
    require(!RulesProgram(*prefix, "other").allowed(17, "inet1", 99), "prefix does not match unrelated source");
    require(!RulesProgram(*prefix, "H42_1").allowed(17, "other", 99), "prefix does not match unrelated output");

    // Export must not turn a permitted omission into a forbidden literal '*'.
    for (const auto *body : {"switch allow", "switch drop", "drop bidir", "switch allow bidir",
                            "switch H42* drop", "switch H42* drop bidir", "switch to inet* allow bidir"}) {
        const auto rules = parse(body);
        const auto exported = rules->text();
        require(exported.find("switch *") == std::string::npos && exported.find(" to *") == std::string::npos,
                "export preserves omitted port selectors");
        require(!ruleset_changed(rules, *parse_switch_ruleset(exported)), "omitted selector round trip");
    }
    require(RulesProgram(*parse("switch allow"), "anything").allowed(1, "anywhere", 2),
            "omitted allow selectors retain their match-all behavior");
    require(parse("switch H42* drop")->text() == parse("switch H42*,* drop")->text(),
            "label wildcard still has canonical shorthand equivalence");
}
int main() {
    bidirectional_policies();
    port_wildcards();
    auto a = parse_switch_ruleset("# comment\nformat 01# header\n\tserial 00042\r\n"
        "exit exit* # all exits\nexit local\ntrunk backbone*\n"
        "switch block-*,* drop [id=block]\n"
        "switch client*,* to exit*,1001 allow\n"
        "label client*,* to exit*, [1001, ...]\n"
        "label client-42,17 to other, [44]\n");
    auto b = parse_switch_ruleset(a->text());
    require(a->text() == b->text() && !ruleset_changed(a, *b), "export/load must be idempotent");
    require(a->role("exit0", RuleStatement::Type::exit) && a->role("local", RuleStatement::Type::exit), "multiple wildcard exit roles");
    require(a->role("backbone0", RuleStatement::Type::trunk), "wildcard trunk");
    RulesProgram program(*a, "client-42");
    require(program.mapping(17)->output.port == "exit*", "mapping is first match, not most specific");
    require(program.allowed(17, "exit0", 1001), "allow after rewrite");
    require(!program.allowed(17, "exit0", 17), "output label filter");
    require(!program.allowed(17, "other", 1001), "implicit drop");
    auto ordered = parse("switch drop\nswitch client-42,17 to exit0,1001 allow\n");
    require(!RulesProgram(*ordered, "client-42").allowed(17, "exit0", 1001), "early drop is terminal");
    auto allowed = parse("switch allow\nswitch client-42,17 drop\n");
    require(RulesProgram(*allowed, "client-42").allowed(17, "exit0", 1001), "early allow is terminal");
    auto endpoints = parse("switch to exit*,* allow\nswitch drop,1 allow\nswitch to,17 allow\n");
    require(RulesProgram(*endpoints, "anything").allowed(99, "exit0", 33), "omitted source");
    require(RulesProgram(*endpoints, "drop").allowed(1, "other", 2), "keyword port with explicit label");
    require(RulesProgram(*endpoints, "to").allowed(17, "other", 2), "to as a source port");
    auto changed = parse_switch_ruleset("format 1\nserial 43\nswitch allow\n");
    require(ruleset_changed(a, *changed), "higher serial");
    fails([&] { ruleset_changed(changed, *a); }, "older");
    fails([&] { ruleset_changed(a, *allowed); }, "same serial");

    std::array<std::uint8_t, 128> storage{};
    const std::array<std::uint64_t, 3> input{17, 55, 66};
    const std::uint8_t payload[]{0x45, 1, 2, 3};
    const auto make_frame = [&] {
        SwitchFrameHeader header;
        const auto n = encode_switch_header(header, SwitchOpcode::switch_packet, input.data(), input.size(), sizeof(payload));
        std::memcpy(storage.data(), header.data(), n);
        std::memcpy(storage.data() + n, payload, sizeof(payload));
        SwitchFrameView frame;
        require(decode_switch_frame(storage.data(), n + sizeof(payload), frame), "input frame");
        return frame;
    };
    for (const auto &test : std::vector<std::pair<std::string, std::vector<std::uint64_t>>>{
             {"[keep, ...]", {17, 55, 66}}, {"[1001, ...]", {1001, 55, 66}},
             {"[1001]", {1001}}, {"[9, keep, ...]", {9, 55, 66}},
             {"[1,2,3,4,5,6,7,18446744073709551615]", {1,2,3,4,5,6,7,UINT64_MAX}}}) {
        auto rules = parse("label in*,* to out, " + test.first + "\n");
        auto frame = make_frame();
        std::array<std::uint64_t, switch_max_labels> output{};
        std::size_t count = 0, size = 8 + 8 * input.size() + sizeof(payload);
        require(rules->statements[0].stack.apply(frame, output, count), "stack apply");
        require(count == test.second.size(), "stack length");
        rewrite_rules_frame(storage.data(), size, frame, output, count, true);
        SwitchFrameView result;
        require(decode_switch_frame(storage.data(), size, result) && result.opcode == SwitchOpcode::exit_packet, "rewritten frame");
        for (std::size_t i = 0; i < count; ++i) require(result.label(i) == test.second[i], "rewritten label");
        require(result.payload_size == sizeof(payload) && std::memcmp(result.payload, payload, sizeof(payload)) == 0, "payload preserved after resize");
    }
    auto missing = parse("label in*,* to out, [keep, keep, keep, keep]\n");
    std::array<std::uint64_t, switch_max_labels> output{}; std::size_t count = 0;
    require(!missing->statements[0].stack.apply(make_frame(), output, count), "missing keep position drops");
    for (const auto &text : {"switch capture", "switch a,* capture debug [id=test]", "switch to b,* capture"})
        fails([&] { parse(text); }, "capture is not supported yet");
    for (const auto &text : {"format 4\nserial 1\n", "serial 1\n", "format 1\n", "format 1\nserial -1\n", "format 1\nserial 18446744073709551616\n"})
        fails([&] { parse_switch_ruleset(text); }, "");
    for (const auto &text : {"label a,12* to b, [keep]", "label a,* to b, []", "label a,* to b, [1,2,3,4,5,6,7,8,9]",
                            "label a,* to b, [keep,...,1]", "switch a** drop", "switch a,1 to b,1 allow extra",
                            "switch allow [unknown=1]", "switch allow [id=x]\nswitch drop [id=x]"})
        fails([&] { parse(text); }, "line ");
    fails([&] { parse("exit exit*\ntrunk exit0\n"); }, "overlapping");
    fails([&] { parse(std::string(ruleset_max_bytes, '#')); }, "1 MiB");
    fails([&] { parse("switch a,1 to b,1 allow\0garbage"s); }, "ASCII");
    std::cout << "PASS: rules grammar, bidir expansion, comments, first match, stack rewrites, serials, canonical export and validation\n";
}
