#include "../src/child_supervisor.hpp"
#include <iostream>
using namespace tuntom;
void need(bool v,const char* s){if(!v)throw std::runtime_error(s);}
int main(int argc,char** argv){
    if(argc==2&&std::string(argv[1])=="worker-fixture"){
        std::array<std::uint8_t,65536> data{};const auto n=::recv(child::bootstrap_fd,data.data(),data.size(),0);child::Bootstrap profile;v5ext::Bytes bytes(data.begin(),data.begin()+std::max<ssize_t>(n,0));
        sockaddr_storage a{};socklen_t size=sizeof(a);if(n<=0||!child::decode(bytes,profile)||::getsockname(child::udp_fd,reinterpret_cast<sockaddr*>(&a),&size))return 2;
        ::send(child::bootstrap_fd,"READY",5,MSG_NOSIGNAL);pollfd p{child::bootstrap_fd,POLLIN,0};::poll(&p,1,5000);return 0;
    }
    child::Bootstrap profile;profile.tunnel_id=42;profile.exchange=9;profile.expiry=100;profile.peer="192.0.2.1";profile.port_id="auth-session";profile.switch_socket="/tmp/switch.sock";profile.ingress_stack={1001,7};profile.token[0]=3;
    auto encoded=child::encode(profile);child::Bootstrap decoded;need(child::decode(encoded,decoded)&&decoded.ingress_stack==profile.ingress_stack,"bootstrap codec");
    auto worker=child::spawn(argv[0],"worker-fixture",profile,std::chrono::seconds(2));need(worker.pid>0&&worker.control>=0&&worker.port,"spawn/readiness");child::stop(worker);
    std::cout<<"PASS: pre-bound UDP child spawn and readiness bootstrap\n";
}
