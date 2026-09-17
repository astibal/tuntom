#include "../src/relay/registry.hpp"
#include "../src/protocol.hpp"
#include "../src/reassembly.hpp"
#include "../src/fragmentation.hpp"
#include <cassert>
#include <iostream>
using namespace tuntom;
int main() {
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
