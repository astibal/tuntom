#pragma once

#include "relocation.hpp"
#include "v5_extension.hpp"
#include "wire.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>

extern char **environ;
namespace tuntom::child {
inline constexpr int udp_fd=3,bootstrap_fd=4;
inline bool notify_bound(){return ::send(bootstrap_fd,"BOUND",5,MSG_NOSIGNAL)==5;}
struct Bootstrap {
    relocation::Secret secret{};relocation::Hash auth_hash{};std::uint16_t tunnel_id=0;std::uint64_t exchange=0,expiry=0;
    std::array<std::uint8_t,32> token{};std::string peer,port_id,switch_socket;
    std::vector<std::uint64_t> ingress_stack;v5ext::Bytes config;
};
inline void a16(v5ext::Bytes& b,std::size_t v){const auto n=b.size();b.resize(n+2);store_be16(b.data()+n,static_cast<std::uint16_t>(v));}
inline void a32(v5ext::Bytes& b,std::size_t v){const auto n=b.size();b.resize(n+4);store_be32(b.data()+n,static_cast<std::uint32_t>(v));}
inline void a64(v5ext::Bytes& b,std::uint64_t v){const auto n=b.size();b.resize(n+8);store_be64(b.data()+n,v);}
inline v5ext::Bytes encode(const Bootstrap& v){
    if(!v.tunnel_id||!v.exchange||!v.expiry||v.peer.size()>1024||v.port_id.empty()||v.port_id.size()>63||v.switch_socket.size()>107||v.ingress_stack.empty()||v.ingress_stack.size()>8||v.config.size()>v5ext::max_config_size)return {};
    v5ext::Bytes b{'T','T','B',1,static_cast<std::uint8_t>(v.ingress_stack.size()),static_cast<std::uint8_t>(v.port_id.size()),static_cast<std::uint8_t>(v.switch_socket.size()),0};
    a16(b,v.peer.size());a16(b,v.tunnel_id);a32(b,v.config.size());a64(b,v.exchange);a64(b,v.expiry);
    b.insert(b.end(),v.secret.begin(),v.secret.end());b.insert(b.end(),v.auth_hash.begin(),v.auth_hash.end());b.insert(b.end(),v.token.begin(),v.token.end());
    b.insert(b.end(),v.peer.begin(),v.peer.end());b.insert(b.end(),v.port_id.begin(),v.port_id.end());b.insert(b.end(),v.switch_socket.begin(),v.switch_socket.end());
    for(auto label:v.ingress_stack)a64(b,label);
    b.insert(b.end(),v.config.begin(),v.config.end());return b;
}
inline bool decode(const v5ext::Bytes& b,Bootstrap& v){
    if(b.size()<112||std::memcmp(b.data(),"TTB\1",4)||!b[4]||b[4]>8||!b[5]||b[5]>63||b[6]>107||b[7])return false;
    const auto stack=static_cast<std::size_t>(b[4]);const auto port=static_cast<std::size_t>(b[5]);
    const auto socket=static_cast<std::size_t>(b[6]);const auto peer=load_be16(b.data()+8);const auto config=load_be32(b.data()+12);
    v.tunnel_id=load_be16(b.data()+10);v.exchange=load_be64(b.data()+16);v.expiry=load_be64(b.data()+24);
    if(peer>1024||config>v5ext::max_config_size||!v.tunnel_id||!v.exchange||!v.expiry||b.size()!=112ULL+peer+port+socket+8ULL*stack+config)return false;
    std::copy_n(b.begin()+32,16,v.secret.begin());std::copy_n(b.begin()+48,32,v.auth_hash.begin());std::copy_n(b.begin()+80,32,v.token.begin());std::size_t at=112;
    v.peer.assign(reinterpret_cast<const char*>(b.data()+at),peer);at+=peer;v.port_id.assign(reinterpret_cast<const char*>(b.data()+at),port);at+=port;v.switch_socket.assign(reinterpret_cast<const char*>(b.data()+at),socket);at+=socket;v.ingress_stack.clear();
    for(unsigned i=0;i<stack;++i){v.ingress_stack.push_back(load_be64(b.data()+at));at+=8;}v.config.assign(b.begin()+at,b.end());return true;
}
struct Worker {pid_t pid=-1;int control=-1;std::uint16_t port=0;};
inline int bound_udp(std::uint16_t& port){
    int fd=::socket(AF_INET6,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(fd>=0){int off=0;::setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,&off,sizeof(off));sockaddr_in6 a{};a.sin6_family=AF_INET6;a.sin6_addr=in6addr_any;socklen_t n=sizeof(a);
        if(::bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0&&::getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n)==0){port=ntohs(a.sin6_port);return fd;}::close(fd);}
    fd=::socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return -1;sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_ANY);socklen_t n=sizeof(a);
    if(::bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))<0||::getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n)<0){::close(fd);return -1;}port=ntohs(a.sin_port);return fd;
}
inline Worker spawn(const std::string& executable,const char* operation,const Bootstrap& profile,std::chrono::milliseconds timeout){
    auto bytes=encode(profile);if(bytes.empty())throw std::runtime_error("invalid child bootstrap");std::uint16_t port=0;const int udp=bound_udp(port);if(udp<0)throw std::runtime_error("cannot bind child UDP socket");int pair[2];if(::socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)){::close(udp);throw std::runtime_error("cannot create child bootstrap socket");}
    const int child_udp=::fcntl(udp,F_DUPFD_CLOEXEC,10),child_bootstrap=::fcntl(pair[1],F_DUPFD_CLOEXEC,10);
    if(child_udp<0||child_bootstrap<0){if(child_udp>=0)::close(child_udp);if(child_bootstrap>=0)::close(child_bootstrap);::close(udp);::close(pair[0]);::close(pair[1]);throw std::runtime_error("cannot duplicate child descriptors");}
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);posix_spawn_file_actions_adddup2(&actions,child_udp,udp_fd);posix_spawn_file_actions_adddup2(&actions,child_bootstrap,bootstrap_fd);posix_spawn_file_actions_addclose(&actions,child_udp);posix_spawn_file_actions_addclose(&actions,child_bootstrap);
    char* argv[]{const_cast<char*>(executable.c_str()),const_cast<char*>(operation),nullptr};pid_t pid=-1;const int rc=::posix_spawn(&pid,executable.c_str(),&actions,nullptr,argv,environ);posix_spawn_file_actions_destroy(&actions);::close(child_udp);::close(child_bootstrap);::close(udp);::close(pair[1]);
    if(rc){::close(pair[0]);throw std::runtime_error("cannot spawn child worker");}if(::send(pair[0],bytes.data(),bytes.size(),MSG_NOSIGNAL)!=static_cast<ssize_t>(bytes.size())){::kill(pid,SIGKILL);::waitpid(pid,nullptr,0);::close(pair[0]);throw std::runtime_error("cannot bootstrap child worker");}
    pollfd p{pair[0],POLLIN,0};std::array<char,5> ready{};if(::poll(&p,1,static_cast<int>(timeout.count()))<=0||::recv(pair[0],ready.data(),ready.size(),0)!=5||std::memcmp(ready.data(),"READY",5)){::kill(pid,SIGKILL);::waitpid(pid,nullptr,0);::close(pair[0]);throw std::runtime_error("child worker not ready");}
    return {pid,pair[0],port};
}
inline void stop(Worker& worker){if(worker.control>=0)::close(worker.control);worker.control=-1;if(worker.pid>0){::kill(worker.pid,SIGTERM);::waitpid(worker.pid,nullptr,0);}worker.pid=-1;}
}
