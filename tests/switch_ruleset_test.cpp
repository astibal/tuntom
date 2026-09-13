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
int main() {
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
        auto rules = parse("label *,* to out, " + test.first + "\n");
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
    auto missing = parse("label *,* to out, [keep, keep, keep, keep]\n");
    std::array<std::uint64_t, switch_max_labels> output{}; std::size_t count = 0;
    require(!missing->statements[0].stack.apply(make_frame(), output, count), "missing keep position drops");
    for (const auto &text : {"switch capture", "switch a,* capture debug [id=test]", "switch to b,* capture"})
        fails([&] { parse(text); }, "capture is not supported yet");
    for (const auto &text : {"format 2\nserial 1\n", "serial 1\n", "format 1\n", "format 1\nserial -1\n", "format 1\nserial 18446744073709551616\n"})
        fails([&] { parse_switch_ruleset(text); }, "");
    for (const auto &text : {"label a,12* to b, [keep]", "label a,* to b, []", "label a,* to b, [1,2,3,4,5,6,7,8,9]",
                            "label a,* to b, [keep,...,1]", "switch a** drop", "switch a,1 to b,1 allow extra",
                            "switch allow [unknown=1]", "switch allow [id=x]\nswitch drop [id=x]"})
        fails([&] { parse(text); }, "line ");
    fails([&] { parse("exit exit*\ntrunk exit0\n"); }, "overlapping");
    fails([&] { parse(std::string(ruleset_max_bytes, '#')); }, "1 MiB");
    fails([&] { parse("switch a,1 to b,1 allow\0garbage"s); }, "ASCII");
    std::cout << "PASS: rules grammar, comments, first match, stack rewrites, serials, canonical export and validation\n";
}
