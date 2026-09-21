#include "../src/routed_control.hpp"
#include <deque>
#include <iostream>
using namespace tuntom;
namespace cr=tuntom::control_route;
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
struct Delivery { ControlRouter* node; std::string edge; std::vector<std::uint8_t> bytes; };
int main() {
    cr::Frame source; source.kind=cr::Kind::put; source.state=1;
    source.request=remote_control::new_id(); source.origin=remote_control::new_id();
    source.destination={cr::Hop::port("tunnel42"),cr::Hop::peer(),cr::Hop::port("divert-in")};
    source.command="classifier load 4"; source.total=4; source.data="text";
    auto encoded=cr::encode(source); cr::Frame parsed;
    require(cr::decode(encoded.data(),encoded.size(),parsed) && parsed.destination==source.destination && parsed.data=="text", "v2 roundtrip");
    require(encoded[0]==2 && encoded[44]==0 && encoded[45]==17,"wire layout");
    for(std::size_t size=0;size<52;++size) require(!cr::decode(encoded.data(),size,parsed),"truncated header");
    for(auto offset:{0u,1u,2u,3u,50u,51u}) {
        auto bad=encoded; bad[offset]=255;
        require(!cr::decode(bad.data(),bad.size(),parsed),"invalid fixed metadata");
    }
    auto bad=encoded; bad[53]=255;
    require(!cr::decode(bad.data(),bad.size(),parsed),"hop cannot exceed path boundary");
    auto v1=remote_control::encode(cr::unwrap(source)); remote_control::Frame legacy;
    require(remote_control::decode(v1,legacy) && legacy.command==source.command,"v1 interop");
    require(!remote_control::decode(encoded,legacy),"v1 cannot mistake v2 for a command");
    std::deque<Delivery> pending;
    ControlRouter root("switch"), a("tunnel"), b("tunnel"), adapter("divert-adapter"), other("switch");
    std::vector<cr::Frame> answers;
    std::size_t calls=0;
    auto now=ControlRouter::Clock::now();
    root.configure(true,[&](const cr::Frame& f){answers.push_back(f);});
    a.configure(true,[](auto){}); b.configure(true,[](auto){}); other.configure(true,[](auto){});
    adapter.configure(true,[&](const cr::Frame& f){
        ++calls;
        auto reply=cr::wrap(cr::unwrap(f),f.origin,f.reply_path);
        reply.kind=cr::Kind::reply; reply.state=4; reply.command.clear(); reply.offset=0; reply.total=2; reply.data="ok";
        adapter.send(reply,now);
    });
    auto connect=[&](ControlRouter& left,std::string lname,bool lp,ControlRouter& right,std::string rname,bool rp,std::uint64_t generation=1) {
        left.edge(lname,generation,lp,4096,[&,r=&right,rname](auto bytes){pending.push_back({r,rname,bytes});return true;});
        right.edge(rname,generation,rp,4096,[&,l=&left,lname](auto bytes){pending.push_back({l,lname,bytes});return true;});
    };
    connect(root,"tunnel42",false,a,"switch",false);
    connect(a,"peer",true,b,"peer",true);
    connect(b,"divert-in",false,adapter,"input",false);
    auto drain=[&]{unsigned budget=1000; while(!pending.empty()) {
        require(budget--!=0,"routing/discovery cycle"); auto d=std::move(pending.front());pending.pop_front();
        d.node->receive(d.edge,d.bytes.data(),d.bytes.size(),now);
    }};
    source.origin=root.instance(); root.send(source,now); drain();
    require(calls==1 && answers.size()==1 && answers[0].data=="ok","multi-hop command and reversed return stack");
    const auto bound=answers[0].reply_path;
    require(bound.size()==3,"bound route obtained from reply");
    // Bound route fails after adapter reconnect instead of reaching replacement.
    connect(b,"divert-in",false,adapter,"input",false,2);
    source.destination=bound; root.send(source,now);drain();
    require(calls==1 && answers.back().kind==cr::Kind::route_error && answers.back().data=="target_not_found","stale link rejected");
    source.destination={cr::Hop::port("absent")};root.send(source,now);drain();
    require(answers.back().data=="target_not_found","missing first target");
    // Diamond + cycle: same instance is FOUND once and ALT_PATH elsewhere.
    connect(root,"alternate",false,other,"upstream",false);
    connect(other,"divert-out",false,adapter,"output",false);
    answers.clear(); root.begin_discovery(now); drain();
    std::size_t found=0,alt=0;
    for(const auto& f:answers) {found+=f.kind==cr::Kind::found;alt+=f.kind==cr::Kind::alt_path;}
    require(found==5 && alt>=1 && calls==1,"discovery floods once, alternate paths do not execute or propagate");
    // Permission denial is silent for discovery, explicit for commands.
    adapter.configure(false,[&](auto){++calls;});answers.clear();root.begin_discovery(now);drain();
    for(const auto& f:answers) require(f.data.find(remote_control::hex(adapter.instance()))==std::string::npos,"disabled node advertised itself");
    source.destination={cr::Hop::port("alternate"),cr::Hop::port("divert-out")};root.send(source,now);drain();
    require(answers.back().data=="control_denied" && calls==1,"denied command");
    // MTU failure is explicit, not an oversized datagram silently sent.
    root.edge("tiny",1,false,52,[](auto){throw std::runtime_error("oversized send");return false;});
    source.destination={cr::Hop::port("tiny")};root.send(source,now);
    require(answers.back().data=="path_mtu_exceeded","bounded MTU");
    // Lost first reply and final confirmation must not repeat execution.
    RoutedControl sender("switch"), receiver("adapter");
    ControlDispatcher dispatcher;
    unsigned executions=0; bool lost_reply=false,lost_confirmation=false;
    dispatcher.classifier=[&](const std::string&,const std::string& body) {++executions;return body;};
    sender.configure(false,{}); receiver.configure(true,dispatcher);
    connect(sender.router(),"adapter",false,receiver.router(),"switch",false);
    auto request=sender.submit({"classifier load 2048",std::string(2048,'x')},5,10,{cr::Hop::port("adapter")});
    std::optional<ControlResponse> result;
    for(unsigned attempt=0;attempt<100 && !result;++attempt) {
        unsigned budget=2000;
        while(!pending.empty()) {
            require(budget--!=0,"transaction retry storm");
            auto d=std::move(pending.front());pending.pop_front(); cr::Frame f;
            require(cr::decode(d.bytes.data(),d.bytes.size(),f),"transaction encoded frame");
            if(f.kind==cr::Kind::reply && !lost_reply){lost_reply=true;continue;}
            if(f.kind==cr::Kind::confirmed && !lost_confirmation){lost_confirmation=true;continue;}
            d.node->receive(d.edge,d.bytes.data(),d.bytes.size(),now);
        }
        result=request.poll();
        now+=std::chrono::milliseconds(11);sender.tick(now);receiver.tick(now);
    }
    require(result && result->success && result->body==std::string(2048,'x') && executions==1 && lost_confirmation,
        "routed multi-block transaction survives lost ACK/confirmation exactly once");
    std::cout<<"PASS CONTROL v2 codec, routing, reply stacks, reconnect, discovery cycles/ALT_PATH, permissions and MTU\n";
}
