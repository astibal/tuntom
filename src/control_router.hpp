#pragma once
#include "control_route.hpp"
#include <set>
#include <tuple>
#include <sys/socket.h>

namespace tuntom {
// Linux socket cookies survive fd reuse and uniquely bind a route to one
// connection. Never infer a return route from an untrusted port name.
inline std::uint64_t control_socket_generation(int fd) {
    std::uint64_t cookie=0; socklen_t length=sizeof(cookie);
    if (fd < 0 || ::getsockopt(fd,SOL_SOCKET,SO_COOKIE,&cookie,&length) || length!=sizeof(cookie)) return 0;
    return cookie;
}
class ControlRouter {
public:
    using Id=control_route::Id;
    using Frame=control_route::Frame;
    using Path=control_route::Path;
    using Hop=control_route::Hop;
    using Kind=control_route::Kind;
    using Clock=std::chrono::steady_clock;
    using Time=Clock::time_point;
    using Sender=std::function<bool(const std::vector<std::uint8_t>&)>;
    using Deliver=std::function<void(const Frame&)>;
private:
    struct Edge { std::string name; std::uint64_t generation=0; Id token{}; bool peer=false; std::size_t limit=0; Sender send; };
    struct Key {
        Id origin{}, request{};
        bool operator<(const Key& b) const { return std::tie(origin,request)<std::tie(b.origin,b.request); }
    };
    Id instance_{};
    std::string component_;
    bool allowed_=false;
    std::map<std::string,Edge> edges_;
    std::map<Key,Time> seen_;
    std::map<Key,std::pair<Time,std::size_t>> alternatives_;
    Deliver deliver_;
    std::uint64_t dropped_=0;
    Edge* resolve(const Hop& hop) {
        for (auto& item:edges_) {
            auto& e=item.second;
            if ((hop.type==control_route::HopType::peer && e.peer) ||
                (hop.type==control_route::HopType::port && !e.peer && e.name==hop.value) ||
                (hop.type==control_route::HopType::link && Hop::link(e.token)==hop)) return &e;
        }
        return nullptr;
    }
    void reject(const Frame& frame, const std::string& reason, Time now) {
        // A broken return path must not recursively generate more route errors.
        if (frame.kind==Kind::route_error || frame.kind==Kind::reply || frame.kind==Kind::confirmed ||
            frame.kind==Kind::found || frame.kind==Kind::alt_path) { ++dropped_; return; }
        route(control_route::response(frame,Kind::route_error,reason),now);
    }
    void route(Frame frame, Time now) {
        if (frame.destination.empty()) { if (deliver_) deliver_(frame); return; }
        auto* edge=resolve(frame.destination.front());
        if (!edge) { reject(frame,"target_not_found",now); return; }
        if (frame.remaining<=1) { reject(frame,"hop_limit",now); return; }
        frame.destination.erase(frame.destination.begin()); --frame.remaining;
        const auto bytes=control_route::encode(frame);
        if (bytes.size()>edge->limit) { reject(frame,"path_mtu_exceeded",now); return; }
        // A transient failure is a lost datagram. Transaction retries retain IDs.
        if (!edge->send(bytes)) ++dropped_;
    }
    static std::string escape(const std::string& value) {
        std::string out;
        for (unsigned char c:value) {
            if (c=='%' || c=='/' || c=='\t') { out+='%'; out+="0123456789ABCDEF"[c>>4]; out+="0123456789ABCDEF"[c&15]; }
            else out+=static_cast<char>(c);
        }
        return out;
    }
    void discover(Frame frame, const std::string& ingress, Time now, bool local) {
        if (!local && !allowed_) return;
        const Key key{frame.origin,frame.request};
        const bool duplicate=seen_.count(key)!=0;
        if (!duplicate) {
            if (seen_.size()>=256) { reject(frame,"discovery_busy",now); return; }
            seen_.emplace(key,now+std::chrono::seconds(30)); // Reserve BEFORE replying or fanout.
        } else {
            auto& quota=alternatives_[key];
            if (now>=quota.first) { quota.first=now+std::chrono::seconds(1); quota.second=0; }
            if (++quota.second>32) { ++dropped_; return; }
        }
        const auto body=remote_control::hex(instance())+"\t"+component_+"\t"+(allowed_?"control,discover":"local")+"\n";
        auto answer=control_route::response(frame,duplicate?Kind::alt_path:Kind::found,body);
        route(std::move(answer),now);
        if (duplicate) return;
        // Each branch retains its own return stack; replies are not broadcast.
        for (const auto& item:edges_) {
            const auto& e=item.second;
            if (item.first==ingress) continue;
            Frame next=frame;
            next.destination={e.peer?Hop::peer():Hop::port(e.name)};
            next.command+=(next.command.empty()?"":"/")+std::string(e.peer?"peer":"port:"+escape(e.name));
            if (next.command.size()>256) { reject(frame,"discovery_path_limit",now); continue; }
            route(std::move(next),now);
        }
    }
public:
    explicit ControlRouter(std::string component):component_(std::move(component)) {}
    const Id& instance() { if(instance_==Id{}) instance_=remote_control::new_id(); return instance_; }
    void configure(bool allowed, Deliver deliver) { allowed_=allowed; deliver_=std::move(deliver); }
    void edge(const std::string& name, std::uint64_t generation, bool peer, std::size_t limit, Sender send) {
        if (!generation) { edges_.erase(name); return; }
        auto i=edges_.find(name);
        if (i==edges_.end() || i->second.generation!=generation) {
            Edge e; e.name=name; e.generation=generation; e.peer=peer;
            i=edges_.insert_or_assign(name,std::move(e)).first;
        }
        i->second.limit=limit; i->second.send=std::move(send);
    }
    void remove(const std::string& name) { edges_.erase(name); }
    void retain(const std::set<std::string>& names) {
        for (auto i=edges_.begin();i!=edges_.end();) if (!names.count(i->first)) i=edges_.erase(i); else ++i;
    }
    void tick(Time now) {
        for (auto i=seen_.begin();i!=seen_.end();) {
            if (now>=i->second) { alternatives_.erase(i->first); i=seen_.erase(i); } else ++i;
        }
    }
    // Ingress is provided by the actual transport, never by the frame itself.
    void receive(const std::string& ingress, const std::uint8_t* p, std::size_t n, Time now) {
        Frame frame; if (!control_route::decode(p,n,frame)) { ++dropped_; return; }
        const auto edge=edges_.find(ingress); if (edge==edges_.end()) { ++dropped_; return; }
        if (frame.reply_path.size()>=control_route::max_hops) { reject(frame,"reply_path_limit",now); return; }
        if (edge->second.token==Id{}) edge->second.token=remote_control::new_id();
        frame.reply_path.insert(frame.reply_path.begin(),Hop::link(edge->second.token));
        tick(now);
        if (frame.kind==Kind::discover && frame.destination.empty()) { discover(std::move(frame),ingress,now,false); return; }
        const bool response=frame.kind==Kind::reply || frame.kind==Kind::confirmed || frame.kind==Kind::found || frame.kind==Kind::alt_path || frame.kind==Kind::route_error;
        if (!allowed_ && !(response && frame.destination.empty() && frame.origin==instance_)) {
            reject(frame,"control_denied",now); return;
        }
        route(std::move(frame),now);
    }
    void send(Frame frame, Time now) { tick(now); route(std::move(frame),now); }
    Id begin_discovery(Time now, Path path={}) {
        tick(now); Frame frame; frame.kind=Kind::discover; frame.origin=instance(); frame.request=remote_control::new_id();
        const auto id=frame.request;
        if (path.empty()) discover(std::move(frame),{},now,true);
        else { frame.destination=std::move(path); send(std::move(frame),now); }
        return id;
    }
    void local_discovery(Frame frame, Time now) { tick(now); discover(std::move(frame),{},now,true); }
};
} // namespace tuntom
