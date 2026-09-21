#include "../src/routed_control.hpp"
#include "../src/protocol.hpp"
#include <deque>
#include <iostream>
using namespace tuntom;
namespace ca=control_auth;
namespace rc=remote_control;
static bool fail_random=false;
extern "C" ssize_t __real_getrandom(void*,size_t,unsigned);
extern "C" ssize_t __wrap_getrandom(void* p,size_t n,unsigned flags) {
    if(fail_random){errno=EAGAIN;return -1;}
    return __real_getrandom(p,n,flags);
}
static void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Keys {
    ca::Config node,authority;
    Keys() {
        auto s=std::make_shared<ca::SigningKey>();
        s->secret.bytes=ca::parse_key("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
        x25519::public_key(s->grant.pub,s->secret.bytes);
        require(ca::hex(s->grant.pub)=="8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a","RFC7748 public key");
        s->grant.caps=63;s->grant.levels.fill(7);
        node.trusted[s->grant.pub]=s->grant;node.level=3;node.allow_trusted=true;
        authority.signing.push_back(s);authority.allow_trusted=true;
    }
};
static void primitives() {
    Keys k;ca::Auth n,a;ca::Id origin{};origin[0]=9;n.configure(k.node,origin);a.configure(k.authority,origin);
    ca::Id id{};id[0]=1;auto now=ca::Time{};
    auto challenge=n.challenge(id,5,now);require(challenge.size()==80,"challenge size");
    ca::Challenge c;require(ca::Challenge::decode(challenge,c) && c.caps==5 && c.level==3,"requirements");
    auto second=n.challenge(id,5,now+std::chrono::milliseconds(21));
    require(second!=challenge && !second.empty(),"retransmission has fresh N");
    require(a.accept_challenge(id,challenge,now),"accept first pending challenge after second sent");
    ca::Bytes message{1,2,3};auto proof=a.protect(id,message,false);ca::Key principal{};
    auto bad=proof;bad.back()^=1;
    require(!n.verify(id,message,bad,5,principal),"tampering denied before replay state update");
    require(!n.verify(id,{1,2,4},proof,5,principal),"message binding");
    ca::Id other=id;other[1]=1;
    require(!n.verify(other,message,proof,5,principal),"request binding");
    for(std::size_t i=0;i<proof.size();++i) {
        auto changed=proof;changed[i]^=1;
        require(!n.verify(id,message,changed,5,principal),"every proof byte authenticated");
    }
    require(n.verify(id,message,proof,5,principal),"valid MAC");
    require(principal==k.authority.signing[0]->grant.pub,"verified principal");
    require(!n.verify(id,message,proof,5,principal),"replay denied");
    auto reply=n.protect(id,{9},true);
    require(a.verify_reply(id,{9},reply),"reply MAC");
    require(!a.verify_reply(id,{9},reply),"reply replay denied");
    require(!n.verify(id,{9},reply,5,principal),"direction separation");
    ca::Auth old;old.configure(k.authority,origin);require(old.accept_challenge(id,second,now),"accept alternate");
    require(!n.verify(id,message,old.protect(id,message,false),5,principal),"alternate retired on proof");
    proof=a.protect(id,message,false);n.tick(now+std::chrono::minutes(3));
    require(!n.verify(id,message,proof,5,principal),"expired challenge rejected");
    ca::Window w;require(w.accept(70)&&w.accept(69)&&!w.accept(69)&&!w.accept(6)&&!w.accept(0),"replay window");
    ca::Auth wrong_origin;ca::Id different=origin;different[1]=1;wrong_origin.configure(k.authority,different);
    now+=std::chrono::minutes(4);challenge=n.challenge(id,5,now);
    require(wrong_origin.accept_challenge(id,challenge,now),"derive in wrong origin");
    require(!n.verify(id,message,wrong_origin.protect(id,message,false),5,principal),"origin binding");
    auto low=k.node;low.trusted.begin()->second.levels[2]=2;n.reset();n.configure(low,origin);
    challenge=n.challenge(id,5,now);require(a.accept_challenge(id,challenge,now),"sender hints not trusted");
    require(!n.verify(id,message,a.protect(id,message,false),5,principal),"minimum level for each cap");
    low=k.node;low.authority=low.trusted.begin()->first;low.caps=4;low.level=3;n.reset();n.configure(low,origin);
    challenge=n.challenge(id,1,now);require(ca::Challenge::decode(challenge,c)&&c.caps==0&&c.level==0&&c.authority==low.authority,"specific authority clears req fields");
    require(a.accept_challenge(id,challenge,now),"specific authority accepted");
    require(n.verify(id,message,a.protect(id,message,false),1,principal),"specific authority still enforces local rights");
    auto zero=challenge;std::fill_n(zero.begin(),32,0);require(!a.accept_challenge(id,zero,now),"zero public key");
    std::fill_n(zero.begin(),32,0);zero[0]=1;require(!a.accept_challenge(id,zero,now),"low order public key");
    n.reset();n.configure(k.node);a.reset();a.configure(k.authority);
    require(n.challenge({},0,now).empty(),"unsolicited challenges removed");
    challenge=n.challenge(id,1,now);require(a.accept_challenge(id,challenge,now),"request challenge accepted");
    require(!a.accept_challenge({},challenge,now),"zero request challenge rejected");
    require(n.verify(id,message,a.protect(id,message,false),1,principal),"request-specific challenge");
    n.reset();require(!n.verify(id,message,a.protect(id,message,false),1,principal),"rekey clears challenges");
    fail_random=true;const auto failed_challenge=n.challenge(id,1,now);
    fail_random=false;require(failed_challenge.empty(),"entropy failure fails closed without killing the daemon");
    // Any untrusted key (even one claiming all caps) must fail verification.
    auto impostor=std::make_shared<ca::SigningKey>();impostor->secret.bytes.fill(42);
    x25519::public_key(impostor->grant.pub,impostor->secret.bytes);impostor->grant.caps=UINT64_MAX;impostor->grant.levels.fill(UINT64_MAX);
    ca::Config impostor_config;impostor_config.allow_trusted=true;impostor_config.signing.push_back(impostor);ca::Auth imp;imp.configure(impostor_config);
    now+=std::chrono::seconds(1);challenge=n.challenge(id,1,now);require(imp.accept_challenge(id,challenge,now),"impostor can select its own key");
    require(!n.verify(id,message,imp.protect(id,message,false),1,principal),"untrusted authority rejected");
    auto missing=k.node;missing.trusted.begin()->second.caps=1;n.reset();n.configure(missing);
    challenge=n.challenge(id,5,now);a.reset();a.configure(k.authority);require(a.accept_challenge(id,challenge,now),"key claims sufficient caps");
    require(!n.verify(id,message,a.protect(id,message,false),5,principal),"all requested capabilities required locally");
    // Null requirement ignores level; the full 64-bit capability space works.
    ca::Grant grant;grant.caps=std::uint64_t(1)<<63;grant.levels[63]=UINT64_MAX;
    require(grant.allows(0,UINT64_MAX)&&grant.allows(std::uint64_t(1)<<63,UINT64_MAX)&&!grant.allows(1,0),"64-bit capabilities and zero semantics");
}
static void access_modes() {
    Keys k;auto now=ca::Time{};ca::Id id{};id[0]=1;
    auto disabled=k.node;disabled.allow_trusted=false;disabled.signing=k.authority.signing;
    ca::validate(disabled);ca::Auth off;off.configure(disabled);
    require(!off.enabled() && off.challenge({},0,now).empty(),"keys alone do not enable challenge emission");
    ca::Auth node;node.configure(k.node);auto challenge=node.challenge(id,1,now);
    require(!off.accept_challenge(id,challenge,now),"disabled sender ignores challenges even with private key");
    require(off.protect(id,{1},false).empty(),"disabled sender produces no proofs");
    bool threw=false;auto conflicting=k.node;conflicting.allow_all=true;
    try{ca::validate(conflicting);}catch(const std::runtime_error&){threw=true;}
    require(threw,"trusted and debug modes cannot be combined");
    for(const auto flag:{"--allow-control-trusted","--allow-control-all"}) {
        ca::Config parsed;char program[]="test";auto text=std::string(flag);char* argv[]={program,text.data()};int at=1;
        require(ca::option(parsed,text,at,2,argv)&&parsed.enabled(),"shared access flag parser");ca::validate(parsed);
    }
    ca::Config parsed;int at=0;
    require(!ca::option(parsed,"--control-challenge-after-rekey",at,0,nullptr),"obsolete optional challenge flag removed");
    RemoteControl remote;unsigned sent=0,executed=0;
    remote.configure_auth(disabled);
    remote.configure({},[&](const auto&){++sent;},[&](const auto&){++executed;return ControlResponse{};});
    rc::Frame command;command.id=id;command.kind=rc::Kind::put;command.command="show stats";
    remote.receive(rc::encode(command),now);
    rc::Frame offer;offer.kind=rc::Kind::challenge;offer.id=id;offer.data.assign(challenge.begin(),challenge.end());
    remote.receive(rc::encode(offer),now);
    threw=false;try{remote.submit({"show stats",{}},1,std::chrono::milliseconds(20),now);}catch(const std::runtime_error&){threw=true;}
    require(threw,"disabled direct outgoing command rejected locally");
    threw=false;try{remote.query(id,1,std::chrono::milliseconds(20),now);}catch(const std::runtime_error&){threw=true;}
    require(threw && sent==0 && executed==0,"disabled stack never responds, emits, or executes");
    RoutedControl routed("off");routed.configure_auth(disabled);routed.configure(false,{});
    threw=false;try{routed.submit({"show stats",{}},1,20,{control_route::Hop::peer()});}catch(const std::runtime_error&){threw=true;}
    require(threw,"disabled routed outgoing command rejected locally");
    // Debug mode can challenge without pins; neither grants nor authority ID
    // restrict access. A valid proof still establishes bidirectional MACs.
    auto debug=k.node;debug.allow_trusted=false;debug.allow_all=true;
    debug.authority=debug.trusted.begin()->first;debug.caps=UINT64_MAX;debug.level=UINT64_MAX;
    debug.trusted.begin()->second.caps=0;ca::validate(debug);
    ca::Auth n,a;n.configure(debug);a.configure(k.authority);
    challenge=n.challenge(id,0,now);ca::Challenge c;
    require(ca::Challenge::decode(challenge,c)&&c.authority==ca::Key{}&&!c.caps&&!c.level,"debug challenge has no trust requirements");
    require(a.accept_challenge(id,challenge,now),"debug challenge usable by authority");
    ca::Key principal{};require(n.verify(id,{1},a.protect(id,{1},false),UINT64_MAX,principal),"debug deliberately bypasses capability grants");
    ca::Config no_pins;no_pins.allow_all=true;n.reset();n.configure(no_pins);a.reset();
    challenge=n.challenge(id,0,now);require(!challenge.empty()&&a.accept_challenge(id,challenge,now),"debug challenge without trust keys");
    require(n.verify(id,{1},a.protect(id,{1},false),1,principal),"debug accepts unpinned authority proof");
    remote.configure_auth(debug);remote.configure(ControlAccess::all(),[&](const auto&){++sent;},[&](const auto&){++executed;return ControlResponse{};});
    remote.receive(rc::encode(command),now);require(executed==1,"debug explicitly accepts unsigned command despite configured pins");
}
static void transactions() {
    Keys k;RemoteControl a,b;std::deque<ca::Bytes> ab,ba;int calls=0;
    a.configure_auth(k.authority);b.configure_auth(k.node);
    a.configure({},[&](const auto& bytes){ab.push_back(bytes);},[](const auto&){return ControlResponse{};});
    b.configure(ControlAccess::all(),[&](const auto& bytes){ba.push_back(bytes);},[&](const auto& request){++calls;require(request.body==std::string(700,'x'),"body preserved");return ControlResponse{true,std::string(1100,'r')};});
    a.payload_limit(500);b.payload_limit(500);auto now=RemoteControl::Time{};
    const auto id=a.submit({"classifier load 700",std::string(700,'x')},15,std::chrono::milliseconds(40),now);
    const auto unsigned_request=ab.front();b.receive(unsigned_request,now);ab.clear();
    require(calls==0 && ba.size()==1,"unsigned command only challenges despite allow all");
    rc::Frame challenge;require(rc::decode(ba.front(),challenge)&&challenge.kind==rc::Kind::challenge,"challenge codec");
    const auto encoded_challenge=rc::encode(challenge);
    for(std::size_t size=0;size<encoded_challenge.size();++size) {
        rc::Frame decoded;require(!rc::decode(ca::Bytes(encoded_challenge.begin(),encoded_challenge.begin()+static_cast<std::ptrdiff_t>(size)),decoded),"truncated challenge rejected");
    }
    auto oversized=encoded_challenge;oversized.push_back(0);rc::Frame decoded_challenge;
    require(!rc::decode(oversized,decoded_challenge),"oversized challenge rejected");
    ca::Bytes captured;bool drop_reply=false,drop_confirm=false;
    for(int step=0;step<5000 && !a.result(id);++step) {
        now+=std::chrono::milliseconds(1);
        if(!ba.empty()) {
            auto bytes=ba.front();ba.pop_front();rc::Frame f;require(rc::decode(bytes,f),"reply decode");
            if(f.kind==rc::Kind::reply && f.state==rc::State::succeeded && !drop_reply)drop_reply=true;
            else if(f.kind==rc::Kind::confirmed && !drop_confirm)drop_confirm=true;
            else a.receive(bytes,now);
        }
        if(!ab.empty()) {
            auto bytes=ab.front();ab.pop_front();if(captured.empty())captured=bytes;
            b.receive(bytes,now);
        }
        a.tick(now);b.tick(now);
    }
    require(a.result(id)&&a.result(id)->exit==0&&a.result(id)->body==std::string(1100,'r'),"authenticated fragmented transaction with lost response/confirmation");
    require(calls==1 && drop_reply && drop_confirm,"one execution");
    b.receive(captured,now);b.receive(unsigned_request,now);require(calls==1,"replays do not execute");
    // CONTROL_CHALLENGE gets its own outer packet type and authenticates there too.
    ascon::key_type key{};ProtocolV5 tx(1,key,false),rx(1,key,true);Packet packet,decoded;
    packet.type=PacketType::control_challenge;packet.sequence=1;packet.payload=rc::encode(challenge);
    auto wire=tx.encode(packet);require(rx.decode(wire.data(),wire.size(),decoded)&&decoded.type==PacketType::control_challenge,"outer challenge type");
    wire.back()^=1;require(!rx.decode(wire.data(),wire.size(),decoded),"outer challenge authentication");
    // A legacy response cannot downgrade a configured authority.
    RemoteControl secure;secure.configure_auth(k.authority);secure.configure({},[](const auto&){},[](const auto&){return ControlResponse{};});
    auto rid=secure.submit({"show stats",{}},1,std::chrono::milliseconds(50),now);
    rc::Frame forged;forged.id=rid;forged.kind=rc::Kind::reply;forged.state=rc::State::succeeded;
    bool accepted=false;secure.receive(rc::encode(forged),now,[&]{accepted=true;});require(!accepted,"unsigned reply downgrade rejected");
}
static void routed() {
    Keys k;RoutedControl a("authority"),relay("relay"),b("node");int calls=0;
    a.configure_auth(k.authority);b.configure_auth(k.node);
    ControlDispatcher d;d.stats=[&]{++calls;return std::string(1700,'s');};
    ca::Config transit;transit.allow_trusted=true;relay.configure_auth(transit);
    a.configure(false,{});relay.configure(false,{});b.configure(false,d);
    std::deque<std::function<void()>> queue;
    auto edge=[&](RoutedControl& from,const char* port,RoutedControl& to,const char* ingress) {
        from.router().edge(port,1,false,600,[&,ingress](const auto& bytes){queue.push_back([&,ingress,bytes]{to.router().receive(ingress,bytes.data(),bytes.size(),ControlRouter::Clock::now());});return true;});
    };
    edge(a,"relay",relay,"authority");edge(relay,"authority",a,"relay");edge(relay,"node",b,"relay");edge(b,"relay",relay,"node");
    auto pending=a.submit({"show stats",{}},5,100,{control_route::Hop::port("relay"),control_route::Hop::port("node")});
    for(int i=0;i<1000 && !queue.empty();++i){auto f=std::move(queue.front());queue.pop_front();f();}
    auto result=pending.poll();
    require(result && result->success && result->body==std::string(1700,'s') && calls==1,"end-to-end auth through non-executing relay");
}
static void discovery() {
    namespace cr=control_route;
    Keys k;
    ControlRouter a("authority"), b("node-b"), c("node-c"), denied("denied");
    auto now=ControlRouter::Clock::now();
    auto origin_config=k.authority;origin_config.trusted=k.node.trusted;
    a.configure_auth(origin_config);b.configure_auth(k.node);c.configure_auth(k.node);
    auto no_read=k.node;no_read.trusted.begin()->second.caps=2;denied.configure_auth(no_read);
    struct Delivery {ControlRouter* node;std::string ingress;ca::Bytes bytes;};
    std::deque<Delivery> queue;
    std::vector<cr::Frame> answers,challenges;
    std::vector<Delivery> proofs;
    a.configure(true,[&](const cr::Frame& f) {
        if(f.kind==cr::Kind::challenge) {
            challenges.push_back(f);
            require(a.answer_discovery_challenge(f,now),"each branch accepts its own challenge");
            require(!a.answer_discovery_challenge(f,now),"duplicate challenge cannot reset sequence state");
        } else if(f.kind==cr::Kind::found || f.kind==cr::Kind::alt_path) {
            if(f.reply_path.empty())return; // Local root is already authorized by the socket.
            auto bad=f;bad.data+="tampered";
            require(!a.verify_discovery_answer(bad,now),"discovery result body authenticated");
            bad=f;bad.command+="/peer";
            require(!a.verify_discovery_answer(bad,now),"discovery path text authenticated");
            bad=f;bad.auth.clear();
            require(!a.verify_discovery_answer(bad,now),"unsigned discovery result cannot downgrade authority");
            require(a.verify_discovery_answer(f,now),"FOUND and ALT_PATH authenticated");
            require(!a.verify_discovery_answer(f,now),"discovery answer replay rejected");
            answers.push_back(f);
        }
    },true);
    for(auto* node:{&b,&c,&denied})node->configure(true,[](const auto&){throw std::runtime_error("unexpected discovery delivery");},true);
    auto connect=[&](ControlRouter& left,const char* lp,ControlRouter& right,const char* rp) {
        left.edge(lp,1,false,4096,[&,r=&right,rp](const auto& bytes){queue.push_back({r,rp,bytes});return true;});
        right.edge(rp,1,false,4096,[&,l=&left,lp](const auto& bytes){queue.push_back({l,lp,bytes});return true;});
    };
    connect(a,"b",b,"a");connect(a,"c",c,"a");connect(b,"c",c,"b");connect(b,"denied",denied,"b");
    const auto request=a.begin_discovery(now);
    // Both independent branches must challenge before returning anything or fanning out.
    for(int i=0;i<2;++i) {
        auto d=queue.front();queue.pop_front();d.node->receive(d.ingress,d.bytes.data(),d.bytes.size(),now);
    }
    require(queue.size()==2 && answers.empty(),"unsigned discovery only challenges, no disclosure or fanout");
    for(const auto& d:queue) {cr::Frame f;require(cr::decode(d.bytes.data(),d.bytes.size(),f)&&f.kind==cr::Kind::challenge,"only challenges emitted");}
    unsigned budget=500;
    while(!queue.empty()) {
        require(budget--!=0,"authenticated fanout cycle bounded");
        auto d=queue.front();queue.pop_front();cr::Frame f;
        require(cr::decode(d.bytes.data(),d.bytes.size(),f),"authenticated discovery codec");
        if(f.kind==cr::Kind::discover && !f.auth.empty() && f.destination.empty()) {
            auto bad=f;bad.auth.back()^=1;auto bytes=cr::encode(bad);
            const auto before=queue.size();d.node->receive(d.ingress,bytes.data(),bytes.size(),now);
            require(queue.size()==before,"bad discovery proof cannot disclose or fan out");
            proofs.push_back(d);
        }
        d.node->receive(d.ingress,d.bytes.data(),d.bytes.size(),now);
    }
    unsigned found=0,alt=0;
    for(const auto& f:answers) {
        require(f.request==request,"one request ID across fanout");
        require(f.data.find("denied")==std::string::npos,"read capability required on every node");
        found+=f.kind==cr::Kind::found;alt+=f.kind==cr::Kind::alt_path;
    }
    require(found==2 && alt==2,"diamond/cycle branches independently authenticated and deduplicated");
    require(challenges.size()>=4 && challenges[0].data.substr(0,32)!=challenges[1].data.substr(0,32),"same request has distinct node challenges");
    for(const auto& d:proofs)d.node->receive(d.ingress,d.bytes.data(),d.bytes.size(),now);
    require(queue.empty(),"proof replay never replies or repeats fanout");
    // An explicit unproven retransmission gets a new challenge, not cached N.
    cr::Frame retry;retry.kind=cr::Kind::discover;retry.origin=a.instance();retry.request=request;
    retry.command="port:b";auto bytes=cr::encode(retry);
    now+=std::chrono::seconds(1);b.receive("a",bytes.data(),bytes.size(),now);
    require(queue.size()==1,"unproven retransmission challenged again");
    cr::Frame fresh;require(cr::decode(queue.front().bytes.data(),queue.front().bytes.size(),fresh),"fresh challenge decode");
    for(const auto& old:challenges)require(fresh.data.substr(0,32)!=old.data.substr(0,32),"retransmission never reuses N");
    queue.clear();now+=std::chrono::seconds(31);b.tick(now);c.tick(now);a.tick(now);
    for(const auto& d:proofs)d.node->receive(d.ingress,d.bytes.data(),d.bytes.size(),now);
    require(queue.empty(),"expired discovery proofs rejected");
    a.release_discovery(request);
}
static void discovery_limits() {
    namespace cr=control_route;
    Keys k;ControlRouter node("node");node.configure_auth(k.node);
    node.configure(true,[](const auto&){throw std::runtime_error("unauthenticated discovery delivered");},true);
    std::vector<cr::Frame> replies;
    node.edge("ingress",1,false,4096,[&](const auto& bytes) {
        cr::Frame f;require(cr::decode(bytes.data(),bytes.size(),f),"bounded discovery response");
        replies.push_back(f);return true;
    });
    cr::Frame f;f.kind=cr::Kind::discover;f.request=rc::new_id();f.origin=rc::new_id();
    const auto bytes=cr::encode(f);auto now=ControlRouter::Clock::now();
    auto receive=[&]{node.receive("ingress",bytes.data(),bytes.size(),now);};
    for(unsigned epoch=0;epoch<4;++epoch) {
        for(unsigned i=0;i<40;++i)receive();
        require(replies.size()==(epoch+1)*32,"discovery challenge rate bound");
        now+=std::chrono::seconds(1);
    }
    receive();require(replies.size()==128,"discovery challenge memory bound");
    now+=std::chrono::seconds(31);node.tick(now);
    fail_random=true;receive();fail_random=false;
    require(replies.size()==128,"discovery entropy failure emits nothing");
    receive();require(replies.size()==129,"expired discovery contexts reclaimed");
    for(const auto& reply:replies)require(reply.kind==cr::Kind::challenge,"resource pressure cannot bypass authorization");
    RoutedControl origin("origin");origin.configure_auth(k.node);origin.configure(false,{});
    bool threw=false;try{origin.submit({"discover",{}},1,20,{});}catch(const std::runtime_error&){threw=true;}
    require(threw,"trusted discovery requires an origin authority key");
}
int main(){primitives();access_modes();transactions();routed();discovery();discovery_limits();std::cout<<"CONTROL authentication passed\n";}
