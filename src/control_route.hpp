#pragma once
#include "remote_control.hpp"
#include <cstring>

namespace tuntom::control_route {
using Id = remote_control::Id;
constexpr std::size_t header_size = 52, max_stack_bytes = 1024, max_hops = 16, max_frame = 65535;
enum class Kind : std::uint8_t { put=1, status=2, reply=3, finish=4, confirmed=5, discover=6, found=7, alt_path=8, route_error=9, challenge=10 };
enum class HopType : std::uint8_t { peer=1, port=2, link=3 };
struct Hop {
    HopType type = HopType::peer;
    std::string value;
    static Hop peer() { return {}; }
    static Hop port(std::string name) { return {HopType::port, std::move(name)}; }
    static Hop link(const Id& id) { return {HopType::link, std::string(reinterpret_cast<const char*>(id.data()), id.size())}; }
    bool operator==(const Hop& other) const { return type == other.type && value == other.value; }
};
using Path = std::vector<Hop>; // Top is at the front.
struct Frame {
    Kind kind = Kind::status;
    std::uint8_t state = 0, remaining = max_hops;
    Id request{}, origin{};
    std::uint32_t offset = 0, total = 0;
    Path destination, reply_path;
    std::string command, data;
    std::vector<std::uint8_t> auth;
};
inline bool marked(const std::uint8_t* p, std::size_t n) { return n && p[0] == 2; }
inline bool valid(const Hop& hop) {
    if (hop.type == HopType::peer) return hop.value.empty();
    if (hop.type == HopType::link) return hop.value.size() == 16 && hop.value != std::string(16, '\0');
    return hop.type == HopType::port && !hop.value.empty() && hop.value.size() <= 63 &&
        std::all_of(hop.value.begin(), hop.value.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
}
inline std::vector<std::uint8_t> encode_path(const Path& path) {
    if (path.size() > max_hops) throw std::runtime_error("control path too deep");
    std::vector<std::uint8_t> out;
    for (const auto& hop : path) {
        if (!valid(hop)) throw std::runtime_error("invalid control hop");
        out.push_back(static_cast<std::uint8_t>(hop.type)); out.push_back(static_cast<std::uint8_t>(hop.value.size()));
        out.insert(out.end(), hop.value.begin(), hop.value.end());
    }
    if (out.size() > max_stack_bytes) throw std::runtime_error("control path too large");
    return out;
}
inline bool decode_path(const std::uint8_t* p, std::size_t n, Path& path) {
    if (n > max_stack_bytes) return false;
    Path result;
    for (std::size_t at = 0; at < n;) {
        if (n - at < 2 || result.size() == max_hops) return false;
        auto type = static_cast<HopType>(p[at]); std::size_t size = p[at + 1]; at += 2;
        if (size > n - at) return false;
        Hop hop{type, std::string(reinterpret_cast<const char*>(p + at), size)};
        if (!valid(hop)) return false;
        result.push_back(std::move(hop)); at += size;
    }
    path = std::move(result); return true;
}
inline std::string path_text(const Path& path) {
    auto bytes=encode_path(path); if(bytes.empty())return "-";
    std::string out;for(auto b:bytes){out+="0123456789abcdef"[b>>4];out+="0123456789abcdef"[b&15];}return out;
}
inline Path parse_path(const std::string& text) {
    if(text=="-")return {};
    if(text.empty() || text.size()%2 || text.size()>2*max_stack_bytes)throw std::runtime_error("invalid encoded control path");
    std::vector<std::uint8_t> bytes(text.size()/2);
    for(std::size_t i=0;i<text.size();++i){auto n=std::string("0123456789abcdef").find(text[i]);if(n==std::string::npos)throw std::runtime_error("invalid encoded control path");bytes[i/2]=static_cast<std::uint8_t>((bytes[i/2]<<4)|n);}
    Path path;if(!decode_path(bytes.data(),bytes.size(),path))throw std::runtime_error("invalid encoded control path");return path;
}
inline bool fields_valid(const Frame& f) {
    const auto kind = static_cast<unsigned>(f.kind);
    if (kind < 1 || kind > 10 || !f.remaining || f.remaining > max_hops || f.request == Id{} || f.origin == Id{} || f.command.size() > 256) return false;
    if (!f.auth.empty() && (kind>5 || f.auth.size()!=control_auth::proof_size))return false;
    if (f.kind==Kind::challenge)return f.state==0 && !f.offset && !f.total && f.command.empty() && f.data.size()==control_auth::challenge_size;
    if (kind <= 5) {
        if (f.state < 1 || f.state > 8) return false;
        if (f.kind != Kind::put && !f.command.empty()) return false;
        if ((f.kind == Kind::status || f.kind == Kind::finish || f.kind == Kind::confirmed) && !f.data.empty()) return false;
        if (f.kind == Kind::put || (f.kind == Kind::reply && f.state >= 4 && f.state <= 7))
            return f.total <= control_max_body && f.offset <= f.total && f.data.size() <= f.total - f.offset;
        return true;
    }
    return f.state == 0 && f.offset == 0 && f.total == 0 &&
        (f.kind != Kind::discover || f.data.empty());
}
inline std::vector<std::uint8_t> encode(const Frame& f) {
    if (!fields_valid(f)) throw std::runtime_error("invalid CONTROL v2 fields");
    const auto destination = encode_path(f.destination), reply = encode_path(f.reply_path);
    const auto size = header_size + f.auth.size() + destination.size() + reply.size() + f.command.size() + f.data.size();
    if (size > max_frame) throw std::runtime_error("CONTROL v2 frame too large");
    std::vector<std::uint8_t> out(size);
    out[0]=2; out[1]=static_cast<std::uint8_t>(f.kind); out[2]=f.state; out[3]=f.remaining;
    std::copy(f.request.begin(),f.request.end(),out.begin()+4); std::copy(f.origin.begin(),f.origin.end(),out.begin()+20);
    store_be32(out.data()+36,f.offset); store_be32(out.data()+40,f.total);
    store_be16(out.data()+44,static_cast<std::uint16_t>(f.command.size()));
    store_be16(out.data()+46,static_cast<std::uint16_t>(destination.size()));
    store_be16(out.data()+48,static_cast<std::uint16_t>(reply.size()));
    store_be16(out.data()+50,static_cast<std::uint16_t>(f.auth.size()));
    auto at = out.begin()+header_size;
    at=std::copy(f.auth.begin(),f.auth.end(),at);
    at=std::copy(destination.begin(),destination.end(),at); at=std::copy(reply.begin(),reply.end(),at);
    at=std::copy(f.command.begin(),f.command.end(),at); std::copy(f.data.begin(),f.data.end(),at);
    return out;
}
inline bool decode(const std::uint8_t* p, std::size_t n, Frame& output) {
    if (n < header_size || n > max_frame || p[0] != 2) return false;
    Frame f; f.kind=static_cast<Kind>(p[1]); f.state=p[2]; f.remaining=p[3];
    std::copy_n(p+4,16,f.request.begin()); std::copy_n(p+20,16,f.origin.begin());
    f.offset=load_be32(p+36); f.total=load_be32(p+40);
    const std::size_t auth=load_be16(p+50), command=load_be16(p+44), destination=load_be16(p+46), reply=load_be16(p+48);
    if ((auth && auth!=control_auth::proof_size) || command > 256 || header_size+auth+command+destination+reply > n) return false;
    if (!decode_path(p+header_size+auth,destination,f.destination) || !decode_path(p+header_size+auth+destination,reply,f.reply_path)) return false;
    f.auth.assign(p+header_size,p+header_size+auth);
    const auto at=header_size+auth+destination+reply;
    f.command.assign(reinterpret_cast<const char*>(p+at),command);
    f.data.assign(reinterpret_cast<const char*>(p+at+command),n-at-command);
    if (!fields_valid(f)) return false;
    output=std::move(f); return true;
}
inline Frame wrap(const remote_control::Frame& f, const Id& origin, Path path) {
    Frame out; out.kind=f.kind==remote_control::Kind::challenge?Kind::challenge:static_cast<Kind>(f.kind); out.state=f.kind==remote_control::Kind::challenge?0:static_cast<std::uint8_t>(f.state);
    out.request=f.id; out.origin=origin; out.destination=std::move(path);
    out.offset=f.offset; out.total=f.total; out.command=f.command; out.data=f.data; out.auth=f.auth; return out;
}
inline remote_control::Frame unwrap(const Frame& f) {
    if (static_cast<unsigned>(f.kind)>5 && f.kind!=Kind::challenge) throw std::runtime_error("not a control transaction");
    remote_control::Frame out; out.kind=f.kind==Kind::challenge?remote_control::Kind::challenge:static_cast<remote_control::Kind>(f.kind); out.state=f.kind==Kind::challenge?remote_control::State::receiving:static_cast<remote_control::State>(f.state);
    out.id=f.request; out.offset=f.offset; out.total=f.total; out.command=f.command; out.data=f.data; out.auth=f.auth; return out;
}
inline Frame response(const Frame& request, Kind kind, std::string data) {
    Frame result; result.kind=kind; result.request=request.request; result.origin=request.origin;
    result.destination=request.reply_path; result.command=request.command; result.data=std::move(data); return result;
}
} // namespace tuntom::control_route
