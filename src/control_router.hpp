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
    bool allowed_=false, discovery_required_=false;
    control_auth::Config auth_config_;
    struct DiscoverySession {
        control_auth::Auth auth;
        Frame request;
        std::string ingress;
        Time expires;
    };
    // A single discovery ID fans out to many independent challenges.
    std::map<control_auth::Key,std::unique_ptr<DiscoverySession>> discovery_in_, discovery_out_;
    Time discovery_rate_epoch_{};
    unsigned discovery_challenges_=0;
    static constexpr std::size_t discovery_sessions=128;
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
        if (frame.kind==Kind::challenge || frame.kind==Kind::route_error || frame.kind==Kind::reply || frame.kind==Kind::confirmed ||
            frame.kind==Kind::found || frame.kind==Kind::alt_path) { ++dropped_; return; }
        route(control_route::response(frame,Kind::route_error,reason),now);
    }
    void route(Frame frame, Time now) {
        if(!allowed_){++dropped_;return;}
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
        DiscoverySession* session=nullptr;
        if (!local && (discovery_required_ || !frame.auth.empty())) {
            if(frame.auth.empty()) {
                // Rate and memory bounds apply across origins and request IDs.
                if(now>=discovery_rate_epoch_) {discovery_rate_epoch_=now+std::chrono::seconds(1);discovery_challenges_=0;}
                if(discovery_challenges_>=32 || discovery_in_.size()>=discovery_sessions)return;
                ++discovery_challenges_;
                auto entry=std::make_unique<DiscoverySession>();
                entry->auth.configure(auth_config_,frame.origin);
                auto bytes=entry->auth.challenge(frame.request,1,now); // read capability
                if(bytes.empty())return;
                control_auth::Challenge c;
                if(!control_auth::Challenge::decode(bytes,c) || discovery_in_.count(c.n))return;
                entry->request=frame;entry->ingress=ingress;entry->expires=now+std::chrono::seconds(30);
                discovery_in_.emplace(c.n,std::move(entry));
                auto challenge=control_route::response(frame,Kind::challenge,{bytes.begin(),bytes.end()});
                route(std::move(challenge),now);
                return; // No seen entry, disclosure or fanout before proof.
            }
            control_auth::Key n{},principal{};
            std::copy_n(frame.auth.begin(),32,n.begin());
            auto i=discovery_in_.find(n);
            if(i==discovery_in_.end())return;
            session=i->second.get();
            const auto& original=session->request;
            if(frame.origin!=original.origin || frame.request!=original.request || frame.command!=original.command ||
               !session->auth.verify(frame.request,control_route::discovery_canonical(frame),frame.auth,1,principal))return;
            // Continue the original branch, not a proof packet's mutable routing fields.
            frame=original;
        }
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
        if(session)answer.auth=session->auth.protect(frame.request,control_route::discovery_canonical(answer),true);
        route(std::move(answer),now);
        if (duplicate) return;
        // Each branch retains its own return stack; replies are not broadcast.
        for (const auto& item:edges_) {
            const auto& e=item.second;
            if (item.first==(session?session->ingress:ingress)) continue;
            Frame next=frame;
            next.auth.clear(); // Every next node must issue its own challenge.
            next.destination={e.peer?Hop::peer():Hop::port(e.name)};
            next.command+=(next.command.empty()?"":"/")+std::string(e.peer?"peer":"port:"+escape(e.name));
            if (next.command.size()>256) { reject(frame,"discovery_path_limit",now); continue; }
            route(std::move(next),now);
        }
    }
public:
    explicit ControlRouter(std::string component):component_(std::move(component)) {}
    const Id& instance() { if(instance_==Id{}) instance_=remote_control::new_id(); return instance_; }
    void configure(bool allowed, Deliver deliver, bool authenticated=false) { allowed_=allowed; discovery_required_=authenticated; deliver_=std::move(deliver); }
    void configure_auth(const control_auth::Config& config) { auth_config_=config; }
    bool answer_discovery_challenge(const Frame& challenge, Time now) {
        tick(now);
        if(!allowed_ || auth_config_.signing.empty() || challenge.origin!=instance() ||
           challenge.kind!=Kind::challenge || discovery_out_.size()>=discovery_sessions)return false;
        control_auth::Challenge c;
        if(!control_auth::Challenge::decode({challenge.data.begin(),challenge.data.end()},c) || discovery_out_.count(c.n))return false;
        auto entry=std::make_unique<DiscoverySession>();
        entry->auth.configure(auth_config_,challenge.origin);
        if(!entry->auth.accept_challenge(challenge.request,{challenge.data.begin(),challenge.data.end()},now))return false;
        Frame proof;proof.kind=Kind::discover;proof.origin=challenge.origin;proof.request=challenge.request;
        proof.command=challenge.command;proof.destination=challenge.reply_path;
        proof.auth=entry->auth.protect(proof.request,control_route::discovery_canonical(proof),false);
        entry->request=proof;entry->expires=now+std::chrono::seconds(30);
        discovery_out_.emplace(c.n,std::move(entry));
        route(std::move(proof),now);
        return true;
    }
    bool verify_discovery_answer(const Frame& frame, Time now) {
        tick(now);
        if(!allowed_ || (frame.kind!=Kind::found && frame.kind!=Kind::alt_path))return false;
        if(frame.auth.empty())return !discovery_required_ && auth_config_.signing.empty();
        if(frame.auth.size()!=control_auth::proof_size)return false;
        control_auth::Key n{};std::copy_n(frame.auth.begin(),32,n.begin());
        auto i=discovery_out_.find(n);if(i==discovery_out_.end())return false;
        const auto& request=i->second->request;
        return frame.origin==request.origin && frame.request==request.request && frame.command==request.command &&
            i->second->auth.verify_reply(frame.request,control_route::discovery_canonical(frame),frame.auth);
    }
    void release_discovery(const Id& request) {
        for(auto i=discovery_out_.begin();i!=discovery_out_.end();) {
            if(i->second->request.request==request)i=discovery_out_.erase(i);else ++i;
        }
    }
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
        for(auto* sessions:{&discovery_in_,&discovery_out_}) {
            for(auto i=sessions->begin();i!=sessions->end();) {
                if(now>=i->second->expires)i=sessions->erase(i);else ++i;
            }
        }
        for (auto i=seen_.begin();i!=seen_.end();) {
            if (now>=i->second) { alternatives_.erase(i->first); i=seen_.erase(i); } else ++i;
        }
    }
    // Ingress is provided by the actual transport, never by the frame itself.
    void receive(const std::string& ingress, const std::uint8_t* p, std::size_t n, Time now) {
        if(!allowed_){++dropped_;return;}
        Frame frame; if (!control_route::decode(p,n,frame)) { ++dropped_; return; }
        const auto edge=edges_.find(ingress); if (edge==edges_.end()) { ++dropped_; return; }
        if (frame.reply_path.size()>=control_route::max_hops) { reject(frame,"reply_path_limit",now); return; }
        if (edge->second.token==Id{}) edge->second.token=remote_control::new_id();
        frame.reply_path.insert(frame.reply_path.begin(),Hop::link(edge->second.token));
        tick(now);
        if (frame.kind==Kind::discover && frame.destination.empty()) { discover(std::move(frame),ingress,now,false); return; }
        // Enabled relays forward; destinations enforce authority policy.
        route(std::move(frame),now);
    }
    void send(Frame frame, Time now) { tick(now); route(std::move(frame),now); }
    Id begin_discovery(Time now, Path path={}) {
        if(!allowed_)throw std::runtime_error("network CONTROL is disabled");
        tick(now); Frame frame; frame.kind=Kind::discover; frame.origin=instance(); frame.request=remote_control::new_id();
        const auto id=frame.request;
        if (path.empty()) discover(std::move(frame),{},now,true);
        else { frame.destination=std::move(path); send(std::move(frame),now); }
        return id;
    }
    void local_discovery(Frame frame, Time now) { if(!allowed_)throw std::runtime_error("network CONTROL is disabled"); tick(now); discover(std::move(frame),{},now,true); }
};
} // namespace tuntom
