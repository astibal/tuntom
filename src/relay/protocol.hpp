#pragma once
#include "../common.hpp"
#include "../ipc/switch_protocol.hpp"
#include <map>
#include <cstring>
#include <chrono>

namespace tuntom::relay {
constexpr std::size_t header_size = 32, frame_limit = 65535 + 72, max_record = frame_limit + header_size;
static_assert(max_record == max_ipc_packet_size);
constexpr std::size_t max_channels = 128;
enum class Type : std::uint8_t { reset = 1, snapshot = 2, acknowledged = 3, data = 4 };
struct View {
    Type type{};
    std::uint32_t channel = 0;
    std::uint64_t epoch = 0, owner = 0;
    const std::uint8_t* payload = nullptr;
    std::size_t size = 0;
};
inline bool marked(const std::uint8_t* p, std::size_t n) {
    return n >= 4 && p[0] == 'T' && p[1] == 'T' && p[2] == 'R' && p[3] == 1;
}
inline bool decode(const std::uint8_t* p, std::size_t n, View& v) {
    if (!marked(p,n) || n < header_size || n > max_record || p[5] || p[6] || p[7] ||
        p[4] < 1 || p[4] > 4 || load_be32(p + 12) != n) return false;
    v = {static_cast<Type>(p[4]), load_be32(p+8), load_be64(p+16), load_be64(p+24), p+header_size, n-header_size};
    if (v.type == Type::reset) return !v.channel && !v.epoch && !v.owner && !v.size;
    if (!v.epoch) return false;
    if (v.type == Type::data) { SwitchFrameView frame; return v.channel && !v.owner && decode_switch_frame(v.payload,v.size,frame) && frame.payload_size <= 65535; }
    return v.channel && !v.owner && (v.type == Type::snapshot ? v.size >= 2 : !v.size);
}
inline void header(std::uint8_t* p, Type type, std::uint32_t channel, std::uint64_t epoch, std::size_t size) {
    p[0]='T'; p[1]='T'; p[2]='R'; p[3]=1; p[4]=static_cast<std::uint8_t>(type); p[5]=p[6]=p[7]=0;
    store_be32(p+8,channel); store_be32(p+12,static_cast<std::uint32_t>(header_size+size));
    store_be64(p+16,epoch); store_be64(p+24,0);
}
inline std::vector<std::uint8_t> encode(Type type, std::uint32_t channel, std::uint64_t epoch,
                                       const std::uint8_t* data=nullptr, std::size_t n=0) {
    if (n > frame_limit) throw std::runtime_error("relay record too large");
    std::vector<std::uint8_t> out(header_size+n);
    header(out.data(),type,channel,epoch,n);
    if(n) std::copy_n(data,n,out.data()+header_size);
    return out;
}
inline bool wrap(std::uint8_t* p, std::size_t& n, std::size_t capacity, std::uint32_t channel, std::uint64_t epoch) {
    if (n > frame_limit || n + header_size > capacity) return false;
    std::memmove(p+header_size,p,n); header(p,Type::data,channel,epoch,n); n += header_size; return true;
}
struct Channel {
    std::uint32_t id = 0;
    std::uint64_t owner = 0;
    std::string name;
};
inline bool snapshot(const View& view, std::map<std::uint32_t,Channel>& out) {
    if(view.type != Type::snapshot || view.size < 2) return false;
    const auto count=load_be16(view.payload);
    if(count>max_channels) return false;
    std::map<std::uint32_t,Channel> result;
    std::map<std::string,bool> names;
    std::size_t at=2;
    for(unsigned i=0;i<count;++i) {
        if(at+13>view.size) return false;
        Channel c{load_be32(view.payload+at),load_be64(view.payload+at+4),{}};
        const auto n=view.payload[at+12]; at+=13;
        if(!c.id || !c.owner || !n || n>switch_max_port_id_size || n>view.size-at) return false;
        c.name.assign(reinterpret_cast<const char*>(view.payload+at),n); at+=n;
        try { (void)encode_switch_registration(c.name); } catch(const std::runtime_error&) { return false; }
        if(!names.emplace(c.name,true).second || !result.emplace(c.id,std::move(c)).second) return false;
    }
    if(at!=view.size) return false;
    out=std::move(result); return true;
}
inline std::vector<std::uint8_t> snapshot(std::uint64_t epoch,std::uint32_t revision,const std::map<std::uint32_t,Channel>& channels) {
    std::vector<std::uint8_t> payload(2); store_be16(payload.data(),static_cast<std::uint16_t>(channels.size()));
    for(const auto& item:channels) {
        const auto& c=item.second; auto at=payload.size(); payload.resize(at+13+c.name.size());
        store_be32(payload.data()+at,c.id); store_be64(payload.data()+at+4,c.owner);
        payload[at+12]=static_cast<std::uint8_t>(c.name.size()); std::copy(c.name.begin(),c.name.end(),payload.begin()+static_cast<std::ptrdiff_t>(at+13));
    }
    return encode(Type::snapshot,revision,epoch,payload.data(),payload.size());
}
struct Directory {
    std::uint64_t epoch=0;
    std::uint32_t revision=0;
    std::map<std::uint32_t,Channel> channels;
};
} // namespace tuntom::relay
