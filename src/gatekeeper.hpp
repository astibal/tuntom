#pragma once

#include "auth_helper.hpp"
#include <arpa/inet.h>
#include <fstream>
#include <map>
#include <sstream>

namespace tuntom::gatekeeper {

struct User { std::string port_id; std::uint64_t label=0; v5ext::Config config; };
struct Policy { std::string auth_command; unsigned auth_timeout=10; std::vector<std::uint64_t> label_prefix; std::map<std::string,User> users; };

inline std::string trim(std::string v) {
    const auto a=v.find_first_not_of(" \t");if(a==std::string::npos)return {};
    const auto b=v.find_last_not_of(" \t");return v.substr(a,b-a+1);
}
inline std::pair<std::string,unsigned> prefix(const std::string& text,unsigned maximum) {
    const auto slash=text.find('/');if(slash==std::string::npos)throw std::runtime_error("address requires /prefix: "+text);
    std::size_t used=0;const auto n=std::stoul(text.substr(slash+1),&used);
    if(used!=text.size()-slash-1||n>maximum)throw std::runtime_error("invalid prefix: "+text);
    return {text.substr(0,slash),static_cast<unsigned>(n)};
}
inline v5ext::ConfigItem network(v5ext::ConfigType type,const std::string& text,bool) {
    const bool v6=type==v5ext::ConfigType::address6||type==v5ext::ConfigType::route6||type==v5ext::ConfigType::exclude6;
    const auto p=prefix(text,v6?128:32);v5ext::ConfigItem item{type,false,{}};item.value.resize(v6?17:5);
    if(::inet_pton(v6?AF_INET6:AF_INET,p.first.c_str(),item.value.data())!=1)throw std::runtime_error("invalid address: "+text);
    item.value.back()=static_cast<std::uint8_t>(p.second);if(type==v5ext::ConfigType::address4||type==v5ext::ConfigType::address6)item.required=true;return item;
}
inline v5ext::ConfigItem scalar(v5ext::ConfigType type,const std::string& text) {
    v5ext::ConfigItem item{type,false,{}};
    if(type==v5ext::ConfigType::dns4||type==v5ext::ConfigType::dns6){item.value.resize(type==v5ext::ConfigType::dns4?4:16);if(::inet_pton(type==v5ext::ConfigType::dns4?AF_INET:AF_INET6,text.c_str(),item.value.data())!=1)throw std::runtime_error("invalid DNS address: "+text);}
    else if(type==v5ext::ConfigType::mtu){std::size_t used=0;const auto n=std::stoul(text,&used);if(used!=text.size()||n<576||n>65535)throw std::runtime_error("invalid MTU");item.value.resize(2);store_be16(item.value.data(),static_cast<std::uint16_t>(n));}
    else {if(text.empty()||text.size()>253)item.value.clear();else item.value.assign(text.begin(),text.end());if(item.value.empty())throw std::runtime_error("invalid search domain");}
    return item;
}
inline Policy load(const std::string& path) {
    std::ifstream in(path);if(!in)throw std::runtime_error("cannot open gatekeeper config: "+path);
    Policy out;User* user=nullptr;bool users_section=false;std::string line;unsigned number=0;
    while(std::getline(in,line)){++number;line=trim(line);if(line.empty()||line[0]=='#')continue;
        if(line.front()=='['&&line.back()==']'){const auto head=trim(line.substr(1,line.size()-2));user=nullptr;users_section=head=="users";if(users_section)continue;if(head.rfind("user ",0)!=0||trim(head.substr(5)).empty())throw std::runtime_error("invalid section at line "+std::to_string(number));const auto name=trim(head.substr(5));if(!out.users.emplace(name,User{}).second)throw std::runtime_error("duplicate user: "+name);user=&out.users.at(name);continue;}
        const auto equal=line.find('=');if(equal==std::string::npos)throw std::runtime_error("missing = at line "+std::to_string(number));const auto key=trim(line.substr(0,equal)),value=trim(line.substr(equal+1));
        if(users_section){if(key!="label_prefix"||!out.label_prefix.empty())throw std::runtime_error("invalid [users] key: "+key);std::size_t at=0;while(at<value.size()){const auto comma=value.find(',',at);const auto part=trim(value.substr(at,comma==std::string::npos?comma:comma-at));std::size_t used=0;const auto label=std::stoull(part,&used,0);if(!label||used!=part.size())throw std::runtime_error("invalid label_prefix");out.label_prefix.push_back(label);if(out.label_prefix.size()>=auth_helper::max_stack)throw std::runtime_error("label_prefix leaves no room for user label");if(comma==std::string::npos)break;at=comma+1;}continue;}
        if(!user){if(key=="auth-command")out.auth_command=value;else if(key=="auth-timeout"){const auto n=std::stoul(value);if(n<1||n>60)throw std::runtime_error("auth-timeout outside 1..60");out.auth_timeout=static_cast<unsigned>(n);}else throw std::runtime_error("unknown global key: "+key);continue;}
        if(key=="label"){std::size_t used=0;user->label=std::stoull(value,&used,0);if(used!=value.size()||!user->label)throw std::runtime_error("invalid label");}
        else if(key=="port-id")user->port_id=value;
        else if(key=="address4")user->config.items.push_back(network(v5ext::ConfigType::address4,value,false));
        else if(key=="address6")user->config.items.push_back(network(v5ext::ConfigType::address6,value,false));
        else if(key=="route4")user->config.items.push_back(network(v5ext::ConfigType::route4,value,true));
        else if(key=="route6")user->config.items.push_back(network(v5ext::ConfigType::route6,value,true));
        else if(key=="exclude4")user->config.items.push_back(network(v5ext::ConfigType::exclude4,value,true));
        else if(key=="exclude6")user->config.items.push_back(network(v5ext::ConfigType::exclude6,value,true));
        else if(key=="dns4")user->config.items.push_back(scalar(v5ext::ConfigType::dns4,value));
        else if(key=="dns6")user->config.items.push_back(scalar(v5ext::ConfigType::dns6,value));
        else if(key=="search-domain")user->config.items.push_back(scalar(v5ext::ConfigType::search_domain,value));
        else if(key=="mtu")user->config.items.push_back(scalar(v5ext::ConfigType::mtu,value));
        else throw std::runtime_error("unknown user key: "+key);
    }
    if(out.auth_command.empty())throw std::runtime_error("auth-command is required");
    for(auto& entry:out.users){auto& u=entry.second;if(!u.label)throw std::runtime_error("label is required for user "+entry.first);if(u.port_id.empty())u.port_id=entry.first;if(u.port_id.size()>63)throw std::runtime_error("port-id too long");bool address=false;for(const auto& item:u.config.items)address|=item.type==v5ext::ConfigType::address4||item.type==v5ext::ConfigType::address6;if(!address)throw std::runtime_error("address4 or address6 is required for user "+entry.first);u.config.id=1;}
    return out;
}

inline auth_helper::Result verify(const Policy& policy,const auth_helper::Verify& request) {
    auth_helper::Result result;const auto found=policy.users.find(request.username);if(found==policy.users.end())return result;
    const auto wire=auth_helper::encode(request);const auto child=auth_helper::run(policy.auth_command,wire,std::chrono::seconds(policy.auth_timeout));
    if(!child.completed||!WIFEXITED(child.status)||WEXITSTATUS(child.status)!=0)return result;
    result.status=auth_helper::Status::allow;result.principal=request.username;result.port_id=found->second.port_id;result.ingress_stack=policy.label_prefix;result.ingress_stack.push_back(found->second.label);result.config=v5ext::encode(found->second.config);return result;
}

} // namespace tuntom::gatekeeper
