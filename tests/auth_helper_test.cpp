#include "../src/auth_helper.hpp"
#include <iostream>
using namespace tuntom;
void need(bool v,const char* s){if(!v)throw std::runtime_error(s);}
int main(int argc,char** argv){
    if(argc==2&&std::string(argv[1])=="verify"){
        auth_helper::Bytes input((std::istreambuf_iterator<char>(std::cin)),{});auth_helper::Verify request;
        if(!auth_helper::decode(input,request))return 2;
        auth_helper::Result result;result.status=request.response==auth_helper::Bytes{'s','e','c','r','e','t'}?auth_helper::Status::allow:auth_helper::Status::deny;
        if(result.status==auth_helper::Status::allow){result.principal=request.username;result.port_id="auth-session";result.ingress_stack={1001,7};}
        const auto output=auth_helper::encode(result);std::cout.write(reinterpret_cast<const char*>(output.data()),static_cast<std::streamsize>(output.size()));return 0;
    }
    auth_helper::Verify request;request.challenge_id=42;request.username="alice";request.peer="192.0.2.1:1234";request.challenge={1,2};request.response={'s','e','c','r','e','t'};
    auto wire=auth_helper::encode(request);auth_helper::Verify decoded;need(auth_helper::decode(wire,decoded)&&decoded.username=="alice","request codec");
    auto child=auth_helper::run(argv[0],wire,std::chrono::seconds(2));auth_helper::Result result;
    need(child.completed&&WIFEXITED(child.status)&&WEXITSTATUS(child.status)==0&&auth_helper::decode(child.output,result),"helper execution");
    need(result.status==auth_helper::Status::allow&&result.principal=="alice"&&result.ingress_stack.size()==2,"helper result");
    request.response={'n','o'};child=auth_helper::run(argv[0],auth_helper::encode(request),std::chrono::seconds(2));need(auth_helper::decode(child.output,result)&&result.status==auth_helper::Status::deny,"deny result");
    std::cout<<"PASS: external AUTH helper framing and safe spawn\n";
}
