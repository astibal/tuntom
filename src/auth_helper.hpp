#pragma once

#include "v5_extension.hpp"
#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace tuntom::auth_helper {

using Bytes=std::vector<std::uint8_t>;
inline constexpr std::size_t max_frame=64*1024,max_stack=8;
enum class Status:std::uint8_t { allow=1,deny=2,temporary_failure=3 };
struct Verify {
    v5ext::AuthMethod method=v5ext::AuthMethod::password_challenge;
    std::uint64_t challenge_id=0;
    std::string username,peer;
    Bytes challenge,response;
};
struct Result {
    Status status=Status::deny;
    std::string principal,port_id;
    std::vector<std::uint64_t> ingress_stack;
    Bytes config;
};
inline void a16(Bytes& b,std::size_t n){const auto p=b.size();b.resize(p+2);store_be16(b.data()+p,static_cast<std::uint16_t>(n));}
inline void a32(Bytes& b,std::size_t n){const auto p=b.size();b.resize(p+4);store_be32(b.data()+p,static_cast<std::uint32_t>(n));}
inline void a64(Bytes& b,std::uint64_t n){const auto p=b.size();b.resize(p+8);store_be64(b.data()+p,n);}
inline Bytes encode(const Verify& v){
    if(!v.challenge_id||v.username.empty()||v.username.size()>255||v.peer.size()>1024||v.challenge.size()>4096||v.response.size()>4096)return {};
    Bytes b{'T','T','A',1,1,static_cast<std::uint8_t>(v.method),0,0};a64(b,v.challenge_id);
    a16(b,v.username.size());a16(b,v.peer.size());a32(b,v.challenge.size());a32(b,v.response.size());
    b.insert(b.end(),v.username.begin(),v.username.end());b.insert(b.end(),v.peer.begin(),v.peer.end());
    b.insert(b.end(),v.challenge.begin(),v.challenge.end());b.insert(b.end(),v.response.begin(),v.response.end());return b;
}
inline bool decode(const Bytes& b,Verify& v){
    if(b.size()<28||std::memcmp(b.data(),"TTA\1",4)||b[4]!=1||b[6]||b[7]||b[5]!=1)return false;
    v.method=static_cast<v5ext::AuthMethod>(b[5]);v.challenge_id=load_be64(b.data()+8);
    const auto un=load_be16(b.data()+16);const auto pn=load_be16(b.data()+18);
    const auto cn=load_be32(b.data()+20);const auto rn=load_be32(b.data()+24);
    if(!v.challenge_id||!un||pn>1024||cn>4096||rn>4096||b.size()!=28ULL+un+pn+cn+rn)return false;
    std::size_t at=28;v.username.assign(reinterpret_cast<const char*>(b.data()+at),un);at+=un;
    v.peer.assign(reinterpret_cast<const char*>(b.data()+at),pn);at+=pn;
    v.challenge.assign(b.begin()+at,b.begin()+at+cn);at+=cn;v.response.assign(b.begin()+at,b.end());return true;
}
inline Bytes encode(const Result& v){
    if(v.principal.size()>255||v.port_id.size()>63||v.ingress_stack.size()>max_stack||v.config.size()>v5ext::max_config_size)return {};
    Bytes b{'T','T','R',1,static_cast<std::uint8_t>(v.status),static_cast<std::uint8_t>(v.principal.size()),static_cast<std::uint8_t>(v.port_id.size()),static_cast<std::uint8_t>(v.ingress_stack.size())};a32(b,v.config.size());
    b.insert(b.end(),v.principal.begin(),v.principal.end());b.insert(b.end(),v.port_id.begin(),v.port_id.end());
    for(auto label:v.ingress_stack)a64(b,label);
    b.insert(b.end(),v.config.begin(),v.config.end());return b;
}
inline bool decode(const Bytes& b,Result& v){
    if(b.size()<12||std::memcmp(b.data(),"TTR\1",4)||b[4]<1||b[4]>3||b[7]>max_stack)return false;
    const auto principal=static_cast<std::size_t>(b[5]);const auto port=static_cast<std::size_t>(b[6]);
    const auto stack=static_cast<std::size_t>(b[7]);const auto config=load_be32(b.data()+8);
    if(port>63||config>v5ext::max_config_size||b.size()!=12ULL+principal+port+8ULL*stack+config)return false;
    v.status=static_cast<Status>(b[4]);std::size_t at=12;
    v.principal.assign(reinterpret_cast<const char*>(b.data()+at),principal);at+=principal;
    v.port_id.assign(reinterpret_cast<const char*>(b.data()+at),port);at+=port;v.ingress_stack.clear();
    for(unsigned i=0;i<stack;++i){v.ingress_stack.push_back(load_be64(b.data()+at));at+=8;}
    v.config.assign(b.begin()+at,b.end());return v.status!=Status::allow||(!v.principal.empty()&&!v.port_id.empty()&&!v.ingress_stack.empty());
}

struct CommandResult { bool completed=false;int status=-1;Bytes output; };
inline CommandResult run(const std::string& path,const Bytes& input,std::chrono::milliseconds timeout,const char* operation="verify",const std::string& argument={}){
    if(path.empty()||input.size()>max_frame)throw std::runtime_error("invalid auth helper invocation");
    int in[2]{-1,-1},out[2]{-1,-1};if(::pipe2(in,O_CLOEXEC)||::pipe2(out,O_CLOEXEC))throw std::runtime_error("auth helper pipe failed");
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,in[0],STDIN_FILENO);posix_spawn_file_actions_adddup2(&actions,out[1],STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions,in[1]);posix_spawn_file_actions_addclose(&actions,out[0]);
    char* argv[]{const_cast<char*>(path.c_str()),const_cast<char*>(operation),
        argument.empty()?nullptr:const_cast<char*>(argument.c_str()),nullptr};pid_t pid=-1;
    const int rc=::posix_spawn(&pid,path.c_str(),&actions,nullptr,argv,environ);posix_spawn_file_actions_destroy(&actions);::close(in[0]);::close(out[1]);
    if(rc){::close(in[1]);::close(out[0]);return {};}
    std::size_t sent=0;Bytes output;const auto deadline=std::chrono::steady_clock::now()+timeout;bool input_open=true,output_open=true;
    while(input_open||output_open){
        const auto now=std::chrono::steady_clock::now();if(now>=deadline)break;
        const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count();pollfd p[2]{{input_open?in[1]:-1,POLLOUT,0},{output_open?out[0]:-1,POLLIN,0}};
        if(::poll(p,2,static_cast<int>(std::max<std::int64_t>(1,left)))<0){if(errno==EINTR)continue;break;}
        if(input_open&&(p[0].revents&(POLLOUT|POLLERR|POLLHUP))){const auto n=::write(in[1],input.data()+sent,input.size()-sent);if(n>0)sent+=static_cast<std::size_t>(n);if(sent==input.size()||n<0){::close(in[1]);input_open=false;}}
        if(output_open&&(p[1].revents&(POLLIN|POLLERR|POLLHUP))){std::array<std::uint8_t,4096> buf{};const auto n=::read(out[0],buf.data(),buf.size());if(n>0&&output.size()+static_cast<std::size_t>(n)<=max_frame)output.insert(output.end(),buf.begin(),buf.begin()+n);else if(n<=0||output.size()+static_cast<std::size_t>(std::max<ssize_t>(n,0))>max_frame){::close(out[0]);output_open=false;}}
    }
    if(input_open)::close(in[1]);
    if(output_open)::close(out[0]);
    int status=0;pid_t waited=::waitpid(pid,&status,WNOHANG);
    while(waited==0&&std::chrono::steady_clock::now()<deadline){poll(nullptr,0,1);waited=::waitpid(pid,&status,WNOHANG);}
    if(waited==0){::kill(pid,SIGKILL);::waitpid(pid,&status,0);return {};}
    return {true,status,std::move(output)};
}

} // namespace tuntom::auth_helper
