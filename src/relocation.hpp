#pragma once

#include "ascon.hpp"
#include "v5_extension.hpp"
#include "wire.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace tuntom::relocation {
using Secret=ascon::key_type;
using Hash=std::array<std::uint8_t,32>;
using Binder=std::array<std::uint8_t,v5ext::relocate_binder_size>;
inline std::vector<std::uint8_t> input(const char* domain,const Hash& auth,std::uint64_t exchange,
                                      std::uint16_t port,std::uint64_t expiry,
                                      const std::array<std::uint8_t,32>& token,const std::string& peer){
    std::vector<std::uint8_t> b(domain,domain+std::strlen(domain)+1);b.insert(b.end(),auth.begin(),auth.end());
    const auto at=b.size();b.resize(at+8+2+8);store_be64(b.data()+at,exchange);store_be16(b.data()+at+8,port);store_be64(b.data()+at+10,expiry);
    b.insert(b.end(),token.begin(),token.end());b.insert(b.end(),peer.begin(),peer.end());return b;
}
inline Binder bind(const Secret& secret,const Hash& auth,std::uint64_t exchange,std::uint16_t port,
                   std::uint64_t expiry,const std::array<std::uint8_t,32>& token,const std::string& peer){
    Binder out{};for(unsigned half=0;half<2;++half){const char* domain=half?"TUNTOM-V5-RELOCATE-1":"TUNTOM-V5-RELOCATE-0";const auto bytes=input(domain,auth,exchange,port,expiry,token,peer);ascon::tag_type tag{};ascon::mac(secret,port,bytes.data(),bytes.size(),tag);std::copy(tag.begin(),tag.end(),out.begin()+half*tag.size());}return out;
}
class Ticket {
public:
    Ticket(const Secret& secret,const Hash& auth,std::uint64_t exchange,std::uint16_t port,
           std::uint64_t expiry,const std::array<std::uint8_t,32>& token,const std::string& peer)
        : secret_(secret),auth_(auth),exchange_(exchange),port_(port),expiry_(expiry),token_(token),peer_(peer),binder_(bind(secret,auth,exchange,port,expiry,token,peer)){}
    const Binder& binder()const{return binder_;}
    bool consume(std::uint64_t now,std::uint16_t port,const std::array<std::uint8_t,32>& token,const Binder& binder,const std::string& peer){
        if(used_||now>expiry_||port!=port_||token!=token_||peer!=peer_)return false;
        const auto expected=relocation::bind(secret_,auth_,exchange_,port,expiry_,token,peer);
        if(!ascon::constant_time_equal(expected.data(),binder.data(),binder.size()))return false;
        used_=true;return true;
    }
private:
    Secret secret_{};Hash auth_{};std::uint64_t exchange_=0;std::uint16_t port_=0;std::uint64_t expiry_=0;
    std::array<std::uint8_t,32> token_{};std::string peer_;Binder binder_{};bool used_=false;
};
}
