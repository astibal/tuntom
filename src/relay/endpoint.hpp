#pragma once
#include "protocol.hpp"
#include "../ipc/switch_handshake.hpp"
#include "../via/registration.hpp"
#include <sys/un.h>
#include <sys/random.h>
#include <functional>
#include <ostream>
#include <poll.h>

namespace tuntom::relay {
// One physical switch connection multiplexes all remote adapter sockets. Local
// adapters negotiate ordinary IPC; the tunnel transports canonical inline frames.
class Endpoint {
    using Clock = std::chrono::steady_clock;
    struct Client {
        int fd = -1;
        std::uint32_t channel = 0;
        std::uint64_t owner = 0;
        ipc::ServerHandshake handshake;
        bool active = false, output_failed = false;
        Clock::time_point deadline;
        ~Client() { if (fd >= 0) ::close(fd); }
    };
    std::string path_, port_;
    int listener_ = -1, upstream_ = -1;
    bool registered_ = false;
    std::uint64_t epoch_ = 0, ipc_epoch_ = 0, next_owner_ = 0;
    std::uint32_t revision_ = 1, acknowledged_ = 0, next_channel_ = 0;
    std::vector<std::unique_ptr<Client>> clients_;
    std::vector<std::uint8_t> buffer_ = std::vector<std::uint8_t>(max_record);
    Clock::time_point next_connect_{}, next_snapshot_{}, ack_deadline_{};
    ipc::Options ipc_options_;
    ipc::RetryQueue upstream_queue_;
    ipc::RetryQueue::Outcome output_stats_;
    void account(const ipc::RetryQueue::Outcome& out) { output_stats_.add(out); }
    static ssize_t raw_send(int fd, const std::uint8_t* data, std::size_t size) {
        return ::send(fd,data,size,MSG_DONTWAIT|MSG_NOSIGNAL);
    }
    static sockaddr_un address(const std::string& path) {
        sockaddr_un a{}; a.sun_family = AF_UNIX;
        if (path.empty() || path.size() >= sizeof(a.sun_path)) throw std::runtime_error("invalid relay socket path");
        std::copy(path.begin(), path.end(), a.sun_path); return a;
    }
    static int socket() { return ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); }
    void changed() {
        if (++revision_ == 0) throw std::runtime_error("relay revision exhausted");
        for (auto& c : clients_) if (c->handshake.transport) account(c->handshake.transport->discard_staged());
        acknowledged_ = 0; next_snapshot_ = {};
    }
    void close_upstream() { account(upstream_queue_.discard()); if (upstream_ >= 0) ::close(upstream_); upstream_ = -1; registered_ = false; }
public:
    using Send = std::function<void(const std::uint8_t*, std::size_t)>;
    Endpoint(const std::string& connect, const std::string& listen, const std::string& port)
        : path_(connect.empty() ? listen : connect), port_(port) {
        ipc_options_.mode = ipc::Mode::inline_only;
        if (connect.empty()) {
            auto a = address(path_); listener_ = socket();
            if (listener_ < 0 || ::bind(listener_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) || ::listen(listener_, 128)) {
                if (listener_ >= 0) ::close(listener_);
                listener_ = -1; throw std::runtime_error("cannot bind relay listener: " + path_);
            }
        }
        session();
    }
    ~Endpoint() { close_upstream(); if (listener_ >= 0) { ::close(listener_); ::unlink(path_.c_str()); } }
    bool listening() const { return listener_ >= 0; }
    void write_stats(std::ostream& out) const {
        out << "relay_ipc_tx_frames=" << output_stats_.frames << "\nrelay_ipc_tx_bytes=" << output_stats_.bytes
            << "\nrelay_ipc_drops=" << output_stats_.drops << "\nrelay_ipc_backpressure_drops=" << output_stats_.backpressure << '\n';
        upstream_queue_.stats(out,"relay_upstream_retry_");
        for (const auto& c : clients_) if (c->active) c->handshake.transport->write_stats(out,"relay_channel_" + std::to_string(c->channel) + "_");
        std::size_t active = 0;
        for (const auto& c : clients_) active += c->active;
        out << "relay_mode=" << (listening() ? "listen" : "connect") << '\n'
            << "relay_local_connected=" << (listening() ? active != 0 : registered_) << '\n'
            << "relay_channels=" << active << '\n';
        if (listening()) out << "relay_epoch=" << epoch_ << '\n'
            << "relay_revision=" << revision_ << '\n'
            << "relay_acknowledged=" << (acknowledged_ == revision_ && Clock::now() < ack_deadline_) << '\n';
    }

    void session() {
        for (auto& c : clients_) if (c->handshake.transport) account(c->handshake.transport->discard_staged());
        close_upstream(); acknowledged_ = 0; next_snapshot_ = {}; revision_ = 1;
        do {
            ssize_t n;
            do { n = ::getrandom(&epoch_, sizeof(epoch_), 0); } while (n < 0 && errno == EINTR);
            if (n != sizeof(epoch_)) throw std::runtime_error("relay epoch generation failed");
        } while (!epoch_);
    }
    int poll_timeout(Clock::time_point now, int limit) const {
        limit = upstream_queue_.timeout(now,limit);
        for (const auto& c : clients_) if (c->active) limit = c->handshake.transport->retry_timeout(now,limit);
        return limit;
    }
    void descriptors(std::vector<pollfd>& out) const {
        if (listener_ >= 0) out.push_back({listener_, POLLIN, 0});
        if (upstream_ >= 0) out.push_back({upstream_, static_cast<short>(registered_ ? POLLIN | (upstream_queue_.writable(Clock::now()) ? POLLOUT : 0) : POLLOUT), 0});
        for (const auto& c : clients_) out.push_back({c->fd, static_cast<short>(c->active ? POLLIN | (c->handshake.transport->retry_writable(Clock::now()) ? POLLOUT : 0) : c->handshake.events()), 0});
    }
    void receive(const std::uint8_t* data, std::size_t size) {
        View v; if (!decode(data,size,v)) return;
        if (!listening()) {
            if (registered_ && (v.type == Type::snapshot || v.type == Type::data)) {
                const auto result = upstream_queue_.submit(data,size,Clock::now(),[&](const std::uint8_t* p,std::size_t n) { return raw_send(upstream_,p,n); });
                account(result);
                if (result.error) close_upstream();
            }
            return;
        }
        if (v.epoch != epoch_) return;
        if (v.type == Type::acknowledged && v.channel == revision_) {
            acknowledged_ = revision_; ack_deadline_ = Clock::now() + std::chrono::seconds(3); return;
        }
        if (v.type != Type::data || acknowledged_ != revision_ || Clock::now() >= ack_deadline_) return;
        for (auto& c : clients_) if (c->active && c->channel == v.channel) {
            if (c->output_failed) return;
            const auto result = c->handshake.transport->append(c->fd, {v.payload, v.size, nullptr, 0});
            account(result); c->output_failed = result.error != 0; return;
        }
    }
    void step(const Send& send) {
        const auto now = Clock::now();
        if (!listening()) {
            if (upstream_ < 0 && now >= next_connect_) {
                next_connect_ = now + std::chrono::seconds(1);
                auto a = address(path_); upstream_ = socket();
                if (upstream_ >= 0 && ::connect(upstream_,reinterpret_cast<sockaddr*>(&a),sizeof(a))) close_upstream();
            }
            if (upstream_ < 0) return;
            if (!registered_) {
                const auto registration = encode_switch_registration(port_);
                const auto n = ::send(upstream_,registration.data(),registration.size(),MSG_DONTWAIT|MSG_NOSIGNAL);
                if (n == static_cast<ssize_t>(registration.size())) registered_ = true;
                else if (n >= 0 || !ipc::retry_error()) close_upstream();
                return;
            }
            const auto result = upstream_queue_.flush(now,[&](const std::uint8_t* p,std::size_t n) { return raw_send(upstream_,p,n); });
            account(result);
            if (result.error) { close_upstream(); return; }
            for (unsigned i=0;i<32;++i) {
                const auto n = ::recv(upstream_,buffer_.data(),buffer_.size(),MSG_DONTWAIT|MSG_TRUNC);
                if (n < 0 && ipc::retry_error()) break;
                if (n <= 0 || static_cast<std::size_t>(n)>buffer_.size()) { close_upstream(); break; }
                View v; if (decode(buffer_.data(),n,v) && (v.type==Type::acknowledged || v.type==Type::data)) send(buffer_.data(),n);
            }
            return;
        }
        for (unsigned i=0;i<16;++i) {
            int fd = ::accept4(listener_,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);
            if(fd<0) break;
            if(clients_.size()>=max_channels || next_channel_==UINT32_MAX || next_owner_==UINT64_MAX) { ::close(fd); continue; }
            std::unique_ptr<Client> c;
            try { c = std::make_unique<Client>(); }
            catch (...) { ::close(fd); throw; }
            c->fd=fd; c->channel=++next_channel_; c->owner=++next_owner_;
            for(const auto& other:clients_) if(via::same_owner(fd,other->fd)) {c->owner=other->owner; break;}
            c->deadline=now+std::chrono::seconds(5); clients_.push_back(std::move(c));
        }
        for(auto it=clients_.begin();it!=clients_.end();) {
            auto& c=**it; bool dead=c.output_failed;
            if(!c.active) {
                c.handshake.step(c.fd,ipc_options_,ipc_epoch_);
                if(c.handshake.identified()) {
                    via::Registration parsed; bool valid=via::registration(c.handshake.id,parsed);
                    for(const auto& other:clients_) if(other.get()!=&c && !other->handshake.id.empty() &&
                        !via::compatible(c.handshake.id,c.fd,other->handshake.id,other->fd)) valid=false;
                    if(valid) c.handshake.prepare(0); else dead=true;
                }
                if(!dead && c.handshake.activation_ready()) {
                    const auto record=ipc::state(ipc::Type::active,c.handshake.parameters());
                    const auto sent=c.handshake.legacy ? static_cast<ssize_t>(record.size) : ipc::send_record(c.fd,record);
                    if(sent==static_cast<ssize_t>(record.size)) { c.active=true; changed(); }
                    else if(sent>=0 || !ipc::retry_error()) dead=true;
                }
                dead |= c.handshake.failed() || (!c.active && now>=c.deadline);
            } else if (!dead) for(unsigned i=0;i<8;++i) {
                const auto n=c.handshake.transport->receive(c.fd,buffer_.data()+header_size,frame_limit);
                if(n<0 && ipc::retry_error()) break;
                if(n<=0 || static_cast<std::size_t>(n)>frame_limit) { dead=true; break; }
                SwitchFrameView frame;
                if(!decode_switch_frame(buffer_.data()+header_size,n,frame) || frame.payload_size > 65535) {dead=true;break;}
                if(acknowledged_==revision_ && now<ack_deadline_) {
                    header(buffer_.data(),Type::data,c.channel,epoch_,n); send(buffer_.data(),header_size+n);
                }
            }
            if (c.active && !dead) {
                const auto result = c.handshake.transport->flush(c.fd);
                account(result); dead = result.error != 0;
            }
            if(dead) { if (c.handshake.transport) account(c.handshake.transport->discard_staged()); if(c.active) changed(); it=clients_.erase(it); } else ++it;
        }
        if(now>=next_snapshot_) {
            std::map<std::uint32_t,Channel> channels;
            for(const auto& c:clients_) if(c->active) channels.emplace(c->channel,Channel{c->channel,c->owner,c->handshake.id});
            auto record=snapshot(epoch_,revision_,channels); send(record.data(),record.size());
            next_snapshot_=now+std::chrono::seconds(1);
        }
    }
};
} // namespace tuntom::relay
