#include "../src/v5_extension.hpp"
#include "../src/v5_auth_state.hpp"
#include "../src/protocol.hpp"
#include <iostream>
using namespace tuntom;
void need(bool v,const char* s){if(!v)throw std::runtime_error(s);}
int main(){
    v5ext::AuthChallenge c;c.id=7;c.lifetime_seconds=10;c.challenge={1,2,3};auto b=v5ext::encode(c);v5ext::AuthChallenge cd;need(v5ext::decode(b,cd)&&cd.id==7&&cd.challenge==c.challenge,"challenge");
    v5ext::AuthResponse r;r.id=7;r.username="alice";r.response={4,5};b=v5ext::encode(r);v5ext::AuthResponse rd;need(v5ext::decode(b,rd)&&rd.username=="alice"&&rd.response==r.response,"response");
    v5ext::AuthOk ok{7},okd;need(v5ext::decode(v5ext::encode(ok),okd)&&okd.id==7,"ok");
    v5ext::AuthFailed fail{7,v5ext::AuthFailure::rejected,3},faild;need(v5ext::decode(v5ext::encode(fail),faild)&&faild.retry_after_seconds==3,"failed");
    v5ext::Relocate rel;rel.port=5555;rel.lifetime_seconds=10;rel.family=4;rel.address[0]=127;rel.token[0]=9;rel.binder[31]=8;v5ext::Relocate reld;need(v5ext::decode(v5ext::encode(rel),reld)&&reld.port==5555&&reld.token[0]==9&&reld.binder[31]==8,"relocate");
    v5ext::RelocateBind bind;bind.token=rel.token;bind.binder=rel.binder;v5ext::RelocateBind bindd;need(v5ext::decode(v5ext::encode(bind),bindd)&&bindd.token==rel.token&&bindd.binder==rel.binder,"relocate bind");
    v5ext::RelocateOk rok{rel.token},rokd;need(v5ext::decode(v5ext::encode(rok),rokd)&&rokd.token==rel.token,"relocate ok");
    v5ext::Config cfg;cfg.id=11;cfg.items.push_back({v5ext::ConfigType::mtu,true,{0x05,0xdc}});v5ext::Config cfgd;need(v5ext::decode(v5ext::encode(cfg),cfgd)&&cfgd.items.size()==1&&cfgd.items[0].required,"config");
    ascon::key_type key{};ProtocolV5 tx(42,key,false),rx(42,key,true);Packet p;p.type=PacketType::auth_challenge;p.sequence=1;p.payload=v5ext::encode(c);auto wire=tx.encode(p),tampered=wire;Packet out;need((wire[0]&0x1f)==16&&rx.decode(wire.data(),wire.size(),out)&&out.type==PacketType::auth_challenge,"extension namespace");tampered[0]|=0x20;need(!rx.decode(tampered.data(),tampered.size(),out),"reserved bit");
    v5ext::AuthState server(v5ext::AuthState::Role::server,true),client(v5ext::AuthState::Role::client);
    need(!server.allows_data()&&server.send_challenge(7)&&client.receive_challenge(c),"challenge state");
    need(client.send_response(7)&&server.receive_response(r)&&server.verified(true),"verification state");
    need(client.receive_ok(ok)&&client.allows_data()&&server.allows_data(),"mandatory result state");
    std::cout<<"PASS: V5 AUTH/RELOCATE/CONFIG extension codecs\n";
}
