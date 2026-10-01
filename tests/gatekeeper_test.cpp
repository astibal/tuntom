#include "../src/gatekeeper.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace tuntom;
void need(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv){
    if(argc==2&&std::string(argv[1])=="verify"){auth_helper::Bytes input((std::istreambuf_iterator<char>(std::cin)),{});auth_helper::Verify request;return auth_helper::decode(input,request)&&request.response==auth_helper::Bytes{'o','k'}?0:1;}
    const std::string path="/tmp/tuntom-gatekeeper-"+std::to_string(::getpid())+".conf";
    {std::ofstream out(path);out<<"auth-command="<<argv[0]<<"\nauth-timeout=2\n[users]\nlabel_prefix=1001,13\n[user alice]\nlabel=77\nport-id=vpn-alice\naddress4=10.8.0.2/24\ndns4=10.8.0.1\nroute4=0.0.0.0/0\nmtu=1400\n";}
    try {const auto policy=gatekeeper::load(path);need(policy.label_prefix==std::vector<std::uint64_t>({1001,13})&&policy.users.at("alice").label==77,"admin label namespace");
        auth_helper::Verify request;request.challenge_id=7;request.username="alice";request.challenge={1};request.response={'o','k'};
        const auto result=gatekeeper::verify(policy,request);need(result.status==auth_helper::Status::allow&&result.ingress_stack==std::vector<std::uint64_t>({1001,13,77}),"allow and namespaced label");
        v5ext::Config config;need(v5ext::decode(result.config,config)&&config.items.size()==4,"CONFIG items");request.username="unknown";need(gatekeeper::verify(policy,request).status==auth_helper::Status::deny,"unknown denied");
    } catch(...){::unlink(path.c_str());throw;}::unlink(path.c_str());std::cout<<"PASS: gatekeeper assigns admin label and CONFIG after external AUTH\n";
}
