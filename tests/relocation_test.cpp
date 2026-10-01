#include "../src/relocation.hpp"
#include <iostream>
using namespace tuntom;
void need(bool v,const char* s){if(!v)throw std::runtime_error(s);}
int main(){
    relocation::Secret secret{};secret[0]=1;relocation::Hash auth{};auth[0]=2;std::array<std::uint8_t,32> token{};token[0]=3;
    relocation::Ticket ticket(secret,auth,77,53000,110,token,"192.0.2.1");auto binder=ticket.binder();
    auto wrong=binder;wrong[0]^=1;need(!ticket.consume(100,53000,token,wrong,"192.0.2.1"),"bad binder");
    need(!ticket.consume(100,53001,token,binder,"192.0.2.1"),"wrong port");need(!ticket.consume(100,53000,token,binder,"192.0.2.2"),"wrong peer");
    need(ticket.consume(100,53000,token,binder,"192.0.2.1"),"valid binder");need(!ticket.consume(100,53000,token,binder,"192.0.2.1"),"binder replay");
    relocation::Ticket expired(secret,auth,78,53000,99,token,"peer");need(!expired.consume(100,53000,token,expired.binder(),"peer"),"expired binder");
    std::cout<<"PASS: relocation binder binding, expiry and one-use replay defense\n";
}
