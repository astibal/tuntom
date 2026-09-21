#pragma once
#include "control_router.hpp"
#include "control_socket.hpp"
#include <memory>
#include <sstream>

namespace tuntom {
class RoutedControl {
public:
    using Id=control_route::Id;
    using Frame=control_route::Frame;
    using Kind=control_route::Kind;
    using Path=control_route::Path;
    using Clock=ControlRouter::Clock;
private:
    struct Receiver { RemoteControl transactions; std::map<Id,Path> replies; std::set<Id> confirmed; Clock::time_point used; };
    struct Pending { Path path; bool discovery=false, acknowledged=false; Clock::time_point deadline; std::vector<Frame> answers; std::size_t answer_bytes=0; std::optional<ControlResponse> error; };
    ControlRouter router_;
    RemoteControl client_;
    std::map<Id,Pending> pending_;
    std::map<Id,std::unique_ptr<Receiver>> receivers_;
    ControlDispatcher dispatcher_;
    bool allowed_=false;
    std::optional<std::pair<Id,Id>> exclusive_;
    void delivered(const Frame& frame) {
        const auto now=Clock::now();
        if (frame.origin==router_.instance()) {
            auto p=pending_.find(frame.request); if(p==pending_.end())return;
            if(frame.kind==Kind::route_error) {
                const bool uncertain=p->second.acknowledged;
                p->second.error=ControlResponse{false,frame.data+(uncertain?
                    "; request was acknowledged earlier; query status before retrying\n":"\n"),!uncertain};
                return;
            }
            if(frame.kind==Kind::found || frame.kind==Kind::alt_path) {
                const auto cost=frame.data.size()+frame.command.size()+80;
                if(p->second.discovery && now<=p->second.deadline && p->second.answers.size()<1024 &&
                   cost<=control_max_body-p->second.answer_bytes) {
                    p->second.answers.push_back(frame); p->second.answer_bytes+=cost;
                }
                return;
            }
            if(frame.kind==Kind::reply || frame.kind==Kind::confirmed) {
                // Pin retries to the connection identities in the first reply.
                p->second.acknowledged = true;
                p->second.path = frame.reply_path;
                client_.receive(remote_control::encode(control_route::unwrap(frame)),now);
            }
            return;
        }
        if(static_cast<unsigned>(frame.kind)>5 || frame.kind==Kind::reply || frame.kind==Kind::confirmed) return;
        auto receiver=receivers_.find(frame.origin);
        if(receiver==receivers_.end()) {
            if(receivers_.size()>=8) {router_.send(control_route::response(frame,Kind::route_error,"control_busy"),now);return;}
            auto entry=std::make_unique<Receiver>(); auto* r=entry.get(); const auto origin=frame.origin;
            r->transactions.payload_limit(352);
            r->transactions.configure(allowed_?ControlAccess::all():ControlAccess{},
                [this,r,origin](const auto& bytes) {
                    remote_control::Frame legacy; if(!remote_control::decode(bytes,legacy))return;
                    if(remote_control::terminal(legacy.state) && exclusive_==std::make_optional(std::make_pair(origin,legacy.id))) exclusive_.reset();
                    if(legacy.kind==remote_control::Kind::confirmed)r->confirmed.insert(legacy.id);
                    const auto route=r->replies.find(legacy.id); if(route==r->replies.end())return;
                    router_.send(control_route::wrap(legacy,origin,route->second),Clock::now());
                },[this](const ControlRequest& request){return dispatcher_.execute(request,allowed_?ControlAccess::all():ControlAccess{});});
            receiver=receivers_.emplace(origin,std::move(entry)).first;
        }
        auto& r=*receiver->second; r.used=now;
        if(!r.replies.count(frame.request) && r.replies.size()>=128) {
            if(r.confirmed.empty()) {router_.send(control_route::response(frame,Kind::route_error,"control_busy"),now);return;}
            r.replies.erase(*r.confirmed.begin());r.confirmed.erase(r.confirmed.begin());
        }
        if(frame.kind==Kind::put && !r.transactions.received_state(frame.request)) {
            try {
                if(!ControlDispatcher::parse(frame.command).cheap) {
                    const auto key=std::make_pair(frame.origin,frame.request);
                    if(exclusive_ && *exclusive_!=key){router_.send(control_route::response(frame,Kind::route_error,"control_busy"),now);return;}
                    exclusive_=key;
                }
            }catch(const std::runtime_error&){} // The dispatcher returns the syntax error.
        }
        r.replies[frame.request]=frame.reply_path;
        r.transactions.receive(remote_control::encode(control_route::unwrap(frame)),now);
    }
    static std::string discovery_text(const Pending& pending) {
        // FOUND/ALT_PATH rows describe actual replies. The deadline is explicit:
        // silence is never represented as a successful empty subtree.
        std::string result="path\tstate\tinstance\tcomponent\tcapabilities\n";
        std::map<std::string,Frame> unique;
        for(const auto& answer:pending.answers) {
            auto i=unique.find(answer.command);
            if(i==unique.end() || answer.kind==Kind::found) unique[answer.command]=answer;
        }
        for(const auto& item:unique) {
            const auto& f=item.second;
            auto data=f.data;
            if(data.find('\n')!=data.size()-1 || std::count(data.begin(),data.end(),'\t')!=2) continue;
            result+=(item.first.empty()?"self":item.first)+"\t"+(f.kind==Kind::found?"FOUND":"ALT_PATH")+"\t"+data;
            if(result.size()>control_max_body) throw std::runtime_error("discovery result exceeds limit");
        }
        if(unique.empty()) result+="peer\tNO_RESPONSE\t-\t-\t-\n";
        return result;
    }
public:
    explicit RoutedControl(std::string component):router_(std::move(component)) {
        client_.payload_limit(352);
        client_.configure({},[this](const auto& bytes) {
            remote_control::Frame legacy;if(!remote_control::decode(bytes,legacy))return;
            auto p=pending_.find(legacy.id);
            if(p==pending_.end())return;
            router_.send(control_route::wrap(legacy,router_.instance(),p->second.path),Clock::now());
        },[](const auto&){return ControlResponse{false,"unexpected local execution\n",true};});
    }
    ControlRouter& router(){return router_;}
    ControlSocket::RemoteSubmit local_submit() {
        return [this](ControlRequest request,unsigned retries,unsigned wait) {
            const auto split=request.command.find(' ');
            if(split==std::string::npos)throw std::runtime_error("missing routed command");
            auto path=control_route::parse_path(request.command.substr(0,split));
            request.command.erase(0,split+1);
            return submit(std::move(request),retries,wait,std::move(path));
        };
    }
    void configure(bool allowed,ControlDispatcher dispatcher) {
        allowed_=allowed;dispatcher_=std::move(dispatcher);
        router_.configure(allowed,[this](const Frame& f){delivered(f);});
    }
    void tick(Clock::time_point now=Clock::now()) {
        router_.tick(now);client_.tick(now);
        for(auto i=receivers_.begin();i!=receivers_.end();) {
            i->second->transactions.tick(now);
            if(exclusive_ && exclusive_->first==i->first) {
                const auto state=i->second->transactions.received_state(exclusive_->second);
                if(!state || remote_control::terminal(*state))exclusive_.reset();
            }
            if(now-i->second->used>std::chrono::minutes(5) && !i->second->transactions.has_unconfirmed()) i=receivers_.erase(i);
            else ++i;
        }
        for(auto i=pending_.begin();i!=pending_.end();) {
            if(now>i->second.deadline+std::chrono::minutes(5)){client_.release(i->first);i=pending_.erase(i);}else ++i;
        }
    }
    bool active()const {
        if (!pending_.empty() || client_.active()) return true;
        for (const auto& receiver:receivers_) if (receiver.second->transactions.active()) return true;
        return false;
    }
    ControlSocket::RemotePending submit(ControlRequest request,unsigned retries,unsigned wait_ms,Path path) {
        if(pending_.size()>=8)throw std::runtime_error("routed control busy");
        const auto now=Clock::now();const bool discovery=request.command=="discover";
        const bool status=request.command.compare(0,15,"request status ")==0;
        const Id id=status?remote_control::parse_id(request.command.substr(15)):remote_control::new_id();
        if (pending_.count(id)) throw std::runtime_error("request already pending");
        Pending p;p.path=std::move(path);p.discovery=discovery;p.deadline=now+(discovery?std::chrono::seconds(2):std::chrono::seconds(7200));
        pending_.emplace(id,std::move(p));
        if(!discovery) {
            try {
                if(status) client_.query(id,retries,std::chrono::milliseconds(wait_ms),now);
                else client_.submit(std::move(request),retries,std::chrono::milliseconds(wait_ms),now,id);
            }
            catch(...){pending_.erase(id);throw;}
        }
        if(discovery) {
            Frame f;f.kind=Kind::discover;f.origin=router_.instance();f.request=id;f.destination=pending_.at(id).path;
            if(f.destination.empty())router_.local_discovery(f,now);else router_.send(f,now);
        }
        return {remote_control::hex(id),[this,id]() -> std::optional<ControlResponse> {
            auto i=pending_.find(id);if(i==pending_.end())return ControlResponse{false,"request expired\n"};
            std::optional<ControlResponse> result;
            if(i->second.error)result=i->second.error;
            else if(i->second.discovery) {if(Clock::now()>=i->second.deadline)result=ControlResponse{true,discovery_text(i->second)};}
            else {auto r=client_.result(id);if(r)result=ControlResponse{r->exit==0,r->body,r->exit==255};}
            if(result){client_.release(id);pending_.erase(i);}return result;
        }};
    }
};
} // namespace tuntom
