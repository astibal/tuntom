#pragma once

#include "wire.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tuntom::v5ext {

using Bytes = std::vector<std::uint8_t>;
inline constexpr std::uint8_t format = 1;
inline constexpr std::size_t max_auth_field = 4096;
inline constexpr std::size_t relocate_token_size = 32;
inline constexpr std::size_t relocate_binder_size = 32;
inline constexpr std::size_t max_config_size = 16384;

enum class AuthMethod : std::uint8_t { password_challenge = 1 };
enum class AuthFailure : std::uint16_t {
    rejected = 1, expired = 2, unsupported = 3, malformed = 4,
    temporary_failure = 5, server_error = 6
};
enum class ConfigType : std::uint16_t {
    address4 = 1, address6 = 2, route4 = 3, route6 = 4,
    exclude4 = 5, exclude6 = 6, dns4 = 7, dns6 = 8,
    search_domain = 9, mtu = 10
};

struct AuthChallenge {
    AuthMethod method = AuthMethod::password_challenge;
    std::uint64_t id = 0;
    std::uint32_t lifetime_seconds = 0;
    Bytes challenge;
};
struct AuthResponse {
    AuthMethod method = AuthMethod::password_challenge;
    std::uint64_t id = 0;
    std::string username;
    Bytes response;
};
struct AuthOk { std::uint64_t id = 0; };
struct AuthFailed {
    std::uint64_t id = 0;
    AuthFailure reason = AuthFailure::rejected;
    std::uint32_t retry_after_seconds = 0;
};
struct Relocate {
    std::uint16_t port = 0;
    std::uint32_t lifetime_seconds = 0;
    // family 0 means the source address of RELOCATE, 4/6 carries an address.
    std::uint8_t family = 0;
    std::array<std::uint8_t, 16> address{};
    std::array<std::uint8_t, relocate_token_size> token{};
    std::array<std::uint8_t, relocate_binder_size> binder{};
};
struct RelocateBind {
    std::array<std::uint8_t, relocate_token_size> token{};
    std::array<std::uint8_t, relocate_binder_size> binder{};
};
struct RelocateOk { std::array<std::uint8_t, relocate_token_size> token{}; };
struct ConfigItem { ConfigType type{}; bool required = false; Bytes value; };
struct Config { std::uint64_t id = 0; std::vector<ConfigItem> items; };

inline void append16(Bytes& b, std::uint16_t v) { const auto n=b.size();b.resize(n+2);store_be16(b.data()+n,v); }
inline void append32(Bytes& b, std::uint32_t v) { const auto n=b.size();b.resize(n+4);store_be32(b.data()+n,v); }
inline void append64(Bytes& b, std::uint64_t v) { const auto n=b.size();b.resize(n+8);store_be64(b.data()+n,v); }
inline bool method(std::uint8_t value, AuthMethod& out) {
    if (value != static_cast<std::uint8_t>(AuthMethod::password_challenge)) return false;
    out=static_cast<AuthMethod>(value);return true;
}

inline Bytes encode(const AuthChallenge& v) {
    if (!v.id || !v.lifetime_seconds || v.challenge.size()>max_auth_field) return {};
    Bytes b{format,static_cast<std::uint8_t>(v.method)};append16(b,0);append64(b,v.id);
    append32(b,v.lifetime_seconds);append32(b,static_cast<std::uint32_t>(v.challenge.size()));
    b.insert(b.end(),v.challenge.begin(),v.challenge.end());return b;
}
inline bool decode(const Bytes& b, AuthChallenge& v) {
    if(b.size()<20||b[0]!=format||load_be16(b.data()+2)||!method(b[1],v.method))return false;
    v.id=load_be64(b.data()+4);v.lifetime_seconds=load_be32(b.data()+12);const auto n=load_be32(b.data()+16);
    if(!v.id||!v.lifetime_seconds||n>max_auth_field||b.size()!=20+n)return false;
    v.challenge.assign(b.begin()+20,b.end());return true;
}
inline Bytes encode(const AuthResponse& v) {
    if(!v.id||v.username.empty()||v.username.size()>255||v.response.size()>max_auth_field)return {};
    Bytes b{format,static_cast<std::uint8_t>(v.method),static_cast<std::uint8_t>(v.username.size()),0};
    append64(b,v.id);append32(b,static_cast<std::uint32_t>(v.response.size()));
    b.insert(b.end(),v.username.begin(),v.username.end());b.insert(b.end(),v.response.begin(),v.response.end());return b;
}
inline bool decode(const Bytes& b, AuthResponse& v) {
    if(b.size()<16||b[0]!=format||b[3]||!method(b[1],v.method))return false;
    const auto un=static_cast<std::size_t>(b[2]);
    const auto rn=load_be32(b.data()+12);v.id=load_be64(b.data()+4);
    if(!v.id||!un||rn>max_auth_field||b.size()!=16+un+rn)return false;
    for(std::size_t i=0;i<un;++i)if(b[16+i]<0x21||b[16+i]>0x7e)return false;
    v.username.assign(reinterpret_cast<const char*>(b.data()+16),un);
    v.response.assign(b.begin()+16+un,b.end());return true;
}
inline Bytes encode(const AuthOk& v) { Bytes b{format,0,0,0};if(!v.id)return {};append64(b,v.id);return b; }
inline bool decode(const Bytes& b,AuthOk& v){if(b.size()!=12||b[0]!=format||b[1]||load_be16(b.data()+2))return false;v.id=load_be64(b.data()+4);return v.id!=0;}
inline Bytes encode(const AuthFailed& v){Bytes b{format,0};append16(b,static_cast<std::uint16_t>(v.reason));append64(b,v.id);append32(b,v.retry_after_seconds);return v.id?b:Bytes{};}
inline bool decode(const Bytes& b,AuthFailed& v){if(b.size()!=16||b[0]!=format||b[1])return false;v.reason=static_cast<AuthFailure>(load_be16(b.data()+2));v.id=load_be64(b.data()+4);v.retry_after_seconds=load_be32(b.data()+12);return v.id&&static_cast<unsigned>(v.reason)>=1&&static_cast<unsigned>(v.reason)<=6;}

inline Bytes encode(const Relocate& v) {
    const auto an=v.family==0?0U:v.family==4?4U:v.family==6?16U:99U;
    if(!v.port||!v.lifetime_seconds||an==99)return {};
    Bytes b{format,v.family};append16(b,v.port);append32(b,v.lifetime_seconds);append16(b,relocate_token_size);append16(b,relocate_binder_size);
    b.insert(b.end(),v.token.begin(),v.token.end());b.insert(b.end(),v.binder.begin(),v.binder.end());
    b.insert(b.end(),v.address.begin(),v.address.begin()+an);return b;
}
inline bool decode(const Bytes& b,Relocate& v) {
    if(b.size()<12||b[0]!=format)return false;
    v.family=b[1];const auto an=v.family==0?0U:v.family==4?4U:v.family==6?16U:99U;
    if(an==99||b.size()!=12+relocate_token_size+relocate_binder_size+an||load_be16(b.data()+8)!=relocate_token_size||load_be16(b.data()+10)!=relocate_binder_size)return false;
    v.port=load_be16(b.data()+2);v.lifetime_seconds=load_be32(b.data()+4);if(!v.port||!v.lifetime_seconds)return false;
    std::copy_n(b.begin()+12,relocate_token_size,v.token.begin());std::copy_n(b.begin()+12+relocate_token_size,relocate_binder_size,v.binder.begin());
    v.address.fill(0);std::copy_n(b.begin()+12+relocate_token_size+relocate_binder_size,an,v.address.begin());return true;
}
inline Bytes encode(const RelocateBind& v){Bytes b{format,0,0,0};b.insert(b.end(),v.token.begin(),v.token.end());b.insert(b.end(),v.binder.begin(),v.binder.end());return b;}
inline bool decode(const Bytes& b,RelocateBind& v){if(b.size()!=4+relocate_token_size+relocate_binder_size||b[0]!=format||b[1]||load_be16(b.data()+2))return false;std::copy_n(b.begin()+4,relocate_token_size,v.token.begin());std::copy_n(b.begin()+4+relocate_token_size,relocate_binder_size,v.binder.begin());return true;}
inline Bytes encode(const RelocateOk& v){Bytes b{format,0,0,0};b.insert(b.end(),v.token.begin(),v.token.end());return b;}
inline bool decode(const Bytes& b,RelocateOk& v){if(b.size()!=4+relocate_token_size||b[0]!=format||b[1]||load_be16(b.data()+2))return false;std::copy_n(b.begin()+4,relocate_token_size,v.token.begin());return true;}

inline Bytes encode(const Config& v) {
    if(!v.id)return {};
    Bytes b{format,0,0,0};append64(b,v.id);append16(b,static_cast<std::uint16_t>(v.items.size()));append16(b,0);
    for(const auto& i:v.items){if(i.value.size()>UINT16_MAX)return {};append16(b,static_cast<std::uint16_t>(i.type));append16(b,i.required?1:0);append16(b,static_cast<std::uint16_t>(i.value.size()));append16(b,0);b.insert(b.end(),i.value.begin(),i.value.end());if(b.size()>max_config_size)return {};}
    return b;
}
inline bool decode(const Bytes& b,Config& v) {
    if(b.size()<16||b.size()>max_config_size||b[0]!=format||b[1]||load_be16(b.data()+2)||load_be16(b.data()+14))return false;
    v.id=load_be64(b.data()+4);const auto count=load_be16(b.data()+12);if(!v.id)return false;v.items.clear();std::size_t at=16;
    for(unsigned n=0;n<count;++n){if(at+8>b.size())return false;const auto flags=load_be16(b.data()+at+2),size=load_be16(b.data()+at+4);if(flags>1||load_be16(b.data()+at+6)||at+8+size>b.size())return false;ConfigItem item;item.type=static_cast<ConfigType>(load_be16(b.data()+at));item.required=flags;item.value.assign(b.begin()+at+8,b.begin()+at+8+size);v.items.push_back(std::move(item));at+=8+size;}
    return at==b.size();
}

} // namespace tuntom::v5ext
