#include "../src/relay/registry.hpp"
#include "../src/protocol.hpp"
#include "../src/reassembly.hpp"
#include "../src/fragmentation.hpp"
#include <cassert>
#include <iostream>
using namespace tuntom;

static void split_sides() {
    const std::string header = "format 3\nserial 1\nport edge id 123\nservice proxy {\nclient-side in*\nserver-side out*\n";
    const std::string tail = "}\nexit exit\nswitch edge,[17] to exit,[99] via [proxy] allow bidir\n";
    const auto rules = parse_switch_ruleset(header + "client-relay in-link*\nserver-relay out-link*\n" + tail);
    assert(parse_switch_ruleset(rules->text())->text() == rules->text());
    for (const auto& bad : {"client-relay in-link\n", "server-relay out-link\n",
            "relay link\nclient-relay in-link\nserver-relay out-link\n",
            "client-relay link*\nserver-relay link-out*\n",
            "client-relay link\nserver-relay link\n", "client-relay *\nserver-relay out-link\n"}) {
        bool rejected = false;
        try { parse_switch_ruleset(header + bad + tail); } catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
    assert(relay::configured(*rules, "in-link0") && relay::configured(*rules, "out-link0"));
    assert(via::accepted(*rules, "in0~via:c:p0", "in-link0"));
    assert(!via::accepted(*rules, "in0~via:c:p0", "out-link0"));
    assert(!via::accepted(*rules, "out0~via:s:p0", "in-link0"));
    assert(!via::accepted(*rules, "in0~via:c:p0"));
    auto record = relay::snapshot(100, 1, {{1,{1,44,"in0~via:c:p0"}}});
    relay::View view; assert(relay::decode(record.data(), record.size(), view));
    relay::Registry registry; bool changed = false;
    assert(relay::update(registry, *rules, "in-link0", view, {"in1~via:c:p0", "out0~via:s:p0"}, changed));
    assert(!relay::update(registry, *rules, "in-link0", view, {"in0~via:c:p0"}, changed));
    assert(!relay::update(registry, *rules, "out-link0", view, {}, changed));
    assert(!relay::update(registry, *rules, "in-link0", view, {"other~via:s:p0"}, changed));

    std::vector<std::string> ports{"edge", "exit"};
    std::map<std::string,std::string> parents;
    for (const auto& instance : {"p0", "p1"}) for (unsigned side = 0; side < 2; ++side) for (unsigned i = 0; i < 2; ++i) {
        const auto name = via::port_name(std::string(side ? "out" : "in") + instance + std::to_string(i), instance, side != 0);
        ports.push_back(name); parents[name] = std::string(side ? "out-link" : "in-link") + instance + std::to_string(i);
    }
    via::State state;
    via::SwitchPath path(state, rules, {}, ports, parents);
    std::set<std::string> dead, seen;
    const auto live = [&](const std::string& name) { return !dead.count(name); };
    std::uint8_t payload[40]{};
    payload[0] = 0x45; payload[3] = 40; payload[9] = 6; payload[12] = 1; payload[16] = 2; payload[23] = 80; payload[32] = 0x50;
    const auto route = [&](const std::string& from, const via::Stack& labels) {
        auto wire = encode_switch_frame(SwitchOpcode::switch_packet,
            std::vector<std::uint64_t>(labels.values.begin(), labels.values.begin() + labels.size), payload, sizeof(payload));
        SwitchFrameView frame; assert(decode_switch_frame(wire.data(), wire.size(), frame));
        return path.route(from, frame, live);
    };
    via::Stack input; input.push(17);
    for (unsigned flow = 0; flow < 64; ++flow) {
        payload[20] = static_cast<std::uint8_t>(flow + 1);
        const auto offer = route("edge", input);
        assert(offer.result == divert::Result::forward && offer.target);
        via::Registration selected; assert(via::registration(*offer.target, selected) && !selected.server);
        seen.insert(selected.instance);
        // Path loss within an instance does not change the instance's HRW weight.
        dead.insert(*offer.target);
        const auto moved = route("edge", input);
        via::Registration other; assert(moved.target && via::registration(*moved.target, other));
        assert(other.instance == selected.instance && *moved.target != *offer.target);
        assert(path.possible("edge", *moved.target));
        // Return through any live OUT worker of the selected instance.
        const auto out = via::port_name("out" + selected.instance + "1", selected.instance, true);
        via::Envelope env; assert(via::Codec::split(offer.labels, env));
        env.action = via::onward; via::Stack onward; assert(via::Codec::attach(env, onward));
        const auto completed = route(out, onward);
        assert(completed.target && *completed.target == "exit");
        const auto reverse = route("exit", completed.labels);
        via::Registration back; assert(reverse.target && via::registration(*reverse.target, back));
        assert(back.server && back.instance == selected.instance);
        // A whole missing side makes that instance unavailable.
        for (const auto& port : ports) {
            via::Registration r;
            if (via::registration(port, r) && r.instance == selected.instance && r.server) dead.insert(port);
        }
        const auto failover = route("edge", input);
        assert(failover.target && via::registration(*failover.target, back) && back.instance != selected.instance);
        dead.clear();
    }
    assert(seen.size() == 2);
    for (const auto& port : ports) if (parents.count(port)) dead.insert(port);
    assert(route("edge", input).result == divert::Result::missing);
}

int main() {
    split_sides();
    const auto rules = parse_switch_ruleset("format 3\nserial 1\nservice proxy {\nclient-side in*\nserver-side out*\nrelay link\nstickiness hash\nunavailable drop\n}\n");
    assert(parse_switch_ruleset(rules->text())->services.at("proxy").relay == "link");
    assert(!via::accepted(*rules,"in0~via:c:p#0"));
    assert(via::accepted(*rules,"in0~via:c:p#0","link"));
    auto grouped = parse_switch_ruleset("format 3\nserial 1\nport edge id 123\nservice proxy {\nclient-side in*\nserver-side out*\nrelay link*\n}\nexit exit\nswitch edge,[17] to exit,[99] via [proxy] allow bidir\n");
    assert(parse_switch_ruleset(grouped->text())->services.at("proxy").relay == "link*");
    assert(relay::configured(*grouped,"link-a") && relay::configured(*grouped,"link-b"));
    assert(!relay::configured(*grouped,"") && !relay::configured(*grouped,"other"));
    assert(!via::accepted(*grouped,"in0~via:c:p#0") && via::accepted(*grouped,"in0~via:c:p#0","link-a"));
    // A wildcard relay group must not combine halves from different parents.
    via::State state;
    std::vector<std::string> ports{"edge", "exit", "in0~via:c:p#0", "out0~via:s:p#0"};
    via::SwitchPath split(state,grouped,{},ports,{{ports[2],"link-a"},{ports[3],"link-b"}});
    assert(!split.possible("edge",ports[2]));
    via::SwitchPath paired(state,grouped,{},ports,{{ports[2],"link-a"},{ports[3],"link-a"}});
    assert(paired.possible("edge",ports[2]));
    std::map<std::uint32_t,relay::Channel> channels{{1,{1,10,"in0~via:c:p#0"}},{2,{2,10,"out0~via:s:p#0"}}};
    auto record = relay::snapshot(123,1,channels);
    relay::View v; assert(relay::decode(record.data(),record.size(),v));
    relay::Registry registry; bool changed = false;
    assert(relay::update(registry,*rules,"link",v,{},changed) && changed);
    assert(relay::update(registry,*rules,"link",v,{},changed) && !changed);
    assert(!relay::update(registry,*rules,"other",v,{},changed));
    assert(!relay::update(registry,*rules,"link",v,{"in0~via:c:p#0"},changed));
    assert(!relay::update(registry,*rules,"link",v,{"out-elsewhere~via:s:p#0"},changed));
    channels.at(2).owner = 11;
    record = relay::snapshot(123,2,channels); assert(relay::decode(record.data(),record.size(),v));
    assert(!relay::update(registry,*rules,"link",v,{},changed));
    channels.at(2).owner = 10; channels.erase(2);
    record = relay::snapshot(123,1,channels); assert(relay::decode(record.data(),record.size(),v));
    assert(!relay::update(registry,*rules,"link",v,{},changed)); // same revision, different contents
    record = relay::snapshot(123,2,channels); assert(relay::decode(record.data(),record.size(),v));
    assert(relay::update(registry,*rules,"link",v,{},changed) && changed);
    record = relay::snapshot(124,3,channels); assert(relay::decode(record.data(),record.size(),v));
    assert(!relay::update(registry,*rules,"link",v,{},changed)); // new epoch needs reconnect
    for(std::size_t n=0;n<32;++n) assert(!relay::decode(record.data(),n,v));
    std::vector<std::uint8_t> payload(65535,42);
    auto largest = encode_switch_frame(SwitchOpcode::exit_packet,std::vector<std::uint64_t>(8,1),payload.data(),payload.size());
    auto wrapped = relay::encode(relay::Type::data,1,123,largest.data(),largest.size());
    assert(wrapped.size() == relay::max_record && relay::decode(wrapped.data(),wrapped.size(),v));
    wrapped[5] = 1; assert(!relay::decode(wrapped.data(),wrapped.size(),v));
    ascon::key_type key{};
    for(bool encrypted : {false,true}) {
        ProtocolV5 sender(42,key,key,encrypted), receiver(42,key,key,encrypted);
        Reassembler reassembly(relay::max_record);
        std::vector<std::uint8_t> original(relay::max_record,42), complete;
        const auto plan = make_fragment_plan(original.size(),411,max_ipc_fragments_per_packet);
        assert(plan.count > 64 && plan.count < 256);
        std::size_t offset=0;
        for(std::size_t i=0;i<plan.count;++i) {
            const auto n=plan.base_size+(i<plan.larger_fragments);
            Packet p; p.type=PacketType::ipc; p.sequence=i+1; p.message_id=99;
            p.original_length=static_cast<std::uint32_t>(original.size()); p.fragment_offset=static_cast<std::uint32_t>(offset);
            p.payload.assign(original.begin()+offset,original.begin()+offset+n);
            const auto wire=sender.encode(p); Packet out;
            assert(wire.size()==41+n && receiver.decode(wire.data(),wire.size(),out));
            assert(out.fragment_offset==offset && out.original_length==original.size());
            assert(reassembly.accept(out,complete)==(i+1==plan.count)); offset+=n;
        }
        assert(complete==original);
    }
    std::cout << "PASS relay registration, ownership, revisions and encrypted 32-bit fragmentation\n";
}
