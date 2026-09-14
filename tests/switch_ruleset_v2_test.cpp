#include "../src/switch_ruleset.hpp"
#include <iostream>
using namespace tuntom;
using Labels = std::vector<std::uint64_t>;
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
auto parse(const std::string &body) { return parse_switch_ruleset("format 2\nserial 42\n" + body); }
void fails(const std::string &body, const std::string &reason) {
    try { parse(body); } catch (const std::exception &e) {
        require(std::string(e.what()).find(reason) != std::string::npos, e.what()); return;
    }
    throw std::runtime_error("expected rejection: " + body);
}
struct Packet {
    std::vector<std::uint8_t> bytes;
    SwitchFrameView view;
    explicit Packet(const Labels &labels) {
        const std::uint8_t payload[]{0x45,1,2,3};
        bytes = encode_switch_frame(SwitchOpcode::switch_packet, labels, payload, sizeof(payload));
        require(decode_switch_frame(bytes.data(), bytes.size(), view), "decode frame");
    }
};
Labels forward(const SwitchRuleset &rules, const std::string &source, const Labels &labels, const std::string &dest) {
    Packet packet(labels); RulesProgram program(rules, source);
    const auto *rule = program.mapping(packet.view);
    require(rule && rule->type == RuleStatement::Type::forward, "expected forwarding rule");
    require(route_port_matches(rule->output.port, dest), "destination group");
    std::array<std::uint64_t,8> output{}; std::size_t count = 0;
    require(rule->stack.apply(packet.view, output, count), "stack rewrite");
    require(program.allowed(rule, packet.view, dest, output.data(), count), "policy");
    return Labels(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(count));
}
void matching() {
    struct Case { std::string match; Labels labels; bool expected; };
    for (const auto &c : std::vector<Case>{
        {"17",{17},true}, {"17",{17,99},false}, {"[17]",{17},true},
        {"*",{42},true}, {"*",{42,99},false},
        {"[17,...]",{17},true}, {"[17,...]",{17,99},true}, {"[17,...]",{18,17},false},
        {"[*,99]",{42,99},true}, {"[*,99]",{42,99,7},false},
        {"[*,99,...]",{42},false}, {"[*,99,...]",{42,99,7},true},
        {"[42,<90,99>,...]",{42,90},true}, {"[42,<90,99>,...]",{42,99},true},
        {"[42,<90,99>,...]",{42,89},false}, {"[42,<90,99>,...]",{42,100},false},
        {"[42,&16,...]",{42,16},true}, {"[42,&16,...]",{42,17,7},true},
        {"[42,&16,...]",{42,48},true}, {"[42,&16,...]",{42,8},false}, {"[42,&16,...]",{42,32},false},
        {"&24",{8},true}, {"&24",{16},true}, {"&24",{24},true}, {"&24",{32},false},
        {"&0",{UINT64_MAX},false}, {"&18446744073709551615",{0},false},
        {"&18446744073709551615",{UINT64_MAX},true}, {"&9223372036854775808",{UINT64_C(1)<<63},true},
        {"<0,18446744073709551615>",{UINT64_MAX},true},
        {"[1,2,3,4,5,6,7,&16]",{1,2,3,4,5,6,7,48},true},
        {"[1,2,3,4,5,6,7,&16,...]",{1,2,3,4,5,6,7,48},true}}) {
        auto rules = parse("switch src," + c.match + " to dst allow"); Packet packet(c.labels);
        require((RulesProgram(*rules,"src").mapping(packet.view) != nullptr) == c.expected, c.match.c_str());
        require(!ruleset_changed(rules,*parse_switch_ruleset(rules->text())), "match round trip");
    }
    require(parse("switch src,17 to dst,99 allow")->text() ==
            parse("switch src,[<17,17>] to dst,[99] allow")->text(), "canonical singleton forms");
}
void bidirectional() {
    struct Case { std::string from, to; Labels input, output, returned; };
    for (const auto &c : std::vector<Case>{
        {"[17,...]","[99,...]",{17,42,8},{99,42,8},{17,42,8}},
        {"[17,...]","[99,...]",{17},{99},{17}},
        {"[17,...]","99",{17,42,8},{99},{17}}, {"[17,42]","99",{17,42},{99},{17,42}},
        {"[42,*,...]","[42,*,...]",{42,99,7},{42,99,7},{42,99,7}},
        {"[42,&16,...]","[99,*,...]",{42,48,7},{99,48,7},{42,48,7}},
        {"[42,<90,99>,...]","[99,*,...]",{42,95,7},{99,95,7},{42,95,7}},
        {"[17]","[99,7]",{17},{99,7},{17}}}) {
        auto rules = parse("switch H42*,"+c.from+" to inet*,"+c.to+" allow bidir [id=path]");
        require(rules->statements.size()==2 && rules->statements[1].id=="path.reverse", "bidir IDs");
        require(forward(*rules,"H42_1",c.input,"inet1")==c.output, "forward rewrite");
        require(forward(*rules,"inet1",c.output,"H42_1")==c.returned, "reverse rewrite");
        require(!ruleset_changed(rules,*parse_switch_ruleset(rules->text())), "bidir round trip");
    }
    auto masked=parse("switch H42*,[42,&16,...] to inet*,[99,*,...] allow bidir"); Packet unrelated({99,8});
    require(!RulesProgram(*masked,"inet1").mapping(unrelated.view), "reverse retains bit predicate");
    for (const auto *body : {"switch H42*,[*,99,...] to inet*,[42,99,...] allow bidir",
                            "switch H42*,[42,<90,99>,...] to inet*,[42,99,...] allow bidir",
                            "switch H42*,[42,&16,...] to inet*,[42,99,...] allow bidir",
                            "switch H42*,[42,*] to inet*,42 allow bidir"}) fails(body,"cannot restore");
    fails("switch H42*,[42] to inet*,[42,*] allow bidir","missing source position");
    fails("switch a,17 to b,99 allow bidir [id=x]\nswitch drop [id=x.reverse]","duplicate rule id");
}
void ordering_and_omissions() {
    auto rules=parse("switch src,[42,...] to out-block*,[*,&16,...] drop\n"
                     "switch src,[<40,45>,...] to out*,[99,16,...] allow\n"
                     "switch src,[42,...] to other,77 allow\n");
    Packet packet({42,8,7}); RulesProgram program(*rules,"src");
    const auto *selected=program.mapping(packet.view); const std::uint64_t output[]{99,16,7};
    require(selected && selected->output.port=="out*", "first allow without specificity override");
    require(!program.allowed(selected,packet.view,"out-block0",output,3), "drop matches rewritten stack");
    require(program.allowed(selected,packet.view,"out0",output,3), "unrelated target allowed");
    auto before=parse("switch src,[42,&16,...] drop\nswitch src to out allow"); Packet flagged({42,48});
    require(RulesProgram(*before,"src").mapping(flagged.view)->type==RuleStatement::Type::policy, "terminal drop");
    auto after=parse("switch src to out allow\nswitch src drop");
    require(forward(*after,"src",{42,16,7},"out")==Labels({42,16,7}), "later drop cannot override");
    for (const auto *body : {"switch allow","switch to inet* allow bidir","switch H42* drop bidir",
                            "switch [17,...] to inet* allow bidir","switch H42*,[17,...] allow bidir",
                            "switch H42*,[17,...] to [99,...] allow bidir","switch drop","drop bidir"}) {
        auto ruleset=parse(body); auto exported=ruleset->text();
        require(!ruleset_changed(ruleset,*parse_switch_ruleset(exported)), "omitted port round trip");
        require(exported.find("switch *")==std::string::npos && exported.find(" to *")==std::string::npos,
                "export must not produce explicit bare port wildcard");
    }
    auto blocked=parse("switch H42* drop bidir\nswitch allow");
    require(blocked->statements[1].input.port=="*" && blocked->statements[1].output.port=="H42*", "drop bidir");
    auto missing=parse("switch src,17 to out,[*,*] allow"); Packet one({17});
    std::array<std::uint64_t,8> labels{}; std::size_t count=0;
    require(!missing->statements[0].stack.apply(one.view,labels,count), "missing output wildcard drops frame");
    for (const auto *port : {"to", "allow", "drop", "capture"}) {
        for (const auto *action : {"allow", "drop"}) {
            auto keywords=parse(std::string("switch ")+port+",[*,...] to dst "+action+" bidir");
            require(!ruleset_changed(keywords,*parse_switch_ruleset(keywords->text())), "keyword port round trip");
        }
    }
}
void literals() {
    require(rules_label_value("0xff")==255 && rules_label_value("0XFF")==255, "hex literals");
    require(rules_label_value("b1000")==8 && rules_label_value("0b1000")==8, "binary literals");
    require(rules_label_value("0xffffffffffffffff")==UINT64_MAX, "hex uint64 limit");
    require(rules_label_value("b"+std::string(64,'1'))==UINT64_MAX, "binary uint64 limit");
    require(rules_label_value(R"("ABCD")")==UINT64_C(0x4142434400000000), "string suffix padding");
    require(rules_label_value(R"("ABCDEFGH")")==UINT64_C(0x4142434445464748), "full eight-byte string");
    require(rules_label_value(R"("")")==0, "empty string");
    require(forward(*parse(u8"switch src,\"ČR\" to dst allow"),"src",{UINT64_C(0xc48c520000000000)},"dst")==
            Labels{UINT64_C(0xc48c520000000000)}, "UTF-8 strings count and pack bytes");
    auto rules=parse(R"(switch src,["ABCD",&b1000,<0x10,0x1f>,...] to dst,["EXIT",*,*,...] allow bidir # comment)");
    const Labels in{UINT64_C(0x4142434400000000),24,31,7}, out{UINT64_C(0x4558495400000000),24,31,7};
    require(forward(*rules,"src",in,"dst")==out, "mixed literals in match and rewrite");
    require(forward(*rules,"dst",out,"src")==in, "mixed literal inverse");
    require(!ruleset_changed(rules,*parse_switch_ruleset(rules->text())), "literal canonical round trip");
    require(parse("switch src,0xff to dst,b1000 allow")->text()==
            parse("switch src,255 to dst,8 allow")->text(), "numeric notation canonical equivalence");
    require(parse(R"(switch src,"ABCD" to dst,"" allow)")->text()==
            parse("switch src,0x4142434400000000 to dst,0 allow")->text(), "string numeric equivalence");
    auto quoted=parse(R"(switch src,"#,[]<>&" to dst,"\"\\" allow # comment)");
    require(forward(*quoted,"src",{UINT64_C(0x232c5b5d3c3e2600)},"dst")==Labels{UINT64_C(0x225c000000000000)},
            "quotes protect comments, delimiters and escapes");
    for (const auto &literal : std::vector<std::string>{"0x","b","0b","b102","0xgg","0x10000000000000000",
            "b1"+std::string(64,'0'),R"("ABCDEFGHI")",R"("unterminated)",R"("bad\n")",R"("AB"CD)"})
        fails("switch src,"+literal+" to dst allow","line 3:");
}
void validation() {
    for (const auto *body : {"switch src,[] to out allow","switch src,[...] to out allow",
                            "switch src,[1,...,2] to out allow","switch src,[1,2,3,4,5,6,7,8,9] to out allow",
                            "switch src,<9,2> to out allow","switch src,<1,2,3> to out allow",
                            "switch src,& to out allow","switch src,&-1 to out allow","switch src,&&16 to out allow",
                            "switch src,&18446744073709551616 to out allow",
                            "switch src,<0,18446744073709551616> to out allow",
                            "switch *,17 to out,99 allow","switch src,17 to *,99 allow","exit *","trunk *"})
        fails(body,"line 3:");
    fails("switch src to out,&16 allow","match predicates");
    fails("switch src to out,<1,2> allow","match predicates");
    fails("label src,17 to out,[99]","format 2 uses switch");
    fails("switch src,[42,&16,...] capture","capture is not supported yet");
    fails("exit inet*\ntrunk inet0","overlapping");
    auto old=parse_switch_ruleset("format 1\nserial 42\nswitch src,17 to out,99 allow\nlabel src,17 to out,[99,...]\n");
    Packet packet({17,42}); require(RulesProgram(*old,"src").mapping(packet.view),"legacy top-label match");
    bool rejected=false;
    try { ruleset_changed(old,*parse("switch src,17 to out,99 allow")); } catch (const std::runtime_error &) { rejected=true; }
    require(rejected,"format transition needs new serial");
    std::string full;
    for (std::size_t i=0;i<ruleset_max_statements-2;++i) full+="switch drop\n";
    require(parse(full+"switch a,17 to b,99 allow bidir")->statements.size()==ruleset_max_statements,"expanded limit");
    fails(full+"switch drop\nswitch a,17 to b,99 allow bidir","maximum 4096");
}
int main() {
    matching(); bidirectional(); ordering_and_omissions(); literals(); validation();
    std::cout << "PASS: format 2 matching, bitmasks, rewrites, bidir, ordering, omissions and validation\n";
}
