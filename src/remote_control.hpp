#pragma once
#include "control_dispatcher.hpp"
#include "wire.hpp"
#include <array>
#include <chrono>
#include <map>
#include <vector>
#include <optional>
#include <algorithm>
#include <sys/random.h>
#include <cerrno>
#include <ostream>

namespace tuntom {
// CONTROL v1: version, kind, state, reserved, id[16], offset:u32,
// total:u32, command_length:u16, reserved:u16, command, opaque body bytes.
namespace remote_control {
using Id = std::array<std::uint8_t, 16>;
enum class Kind : std::uint8_t { put = 1, status = 2, reply = 3, finish = 4, confirmed = 5 };
enum class State : std::uint8_t { receiving = 1, ready = 2, running = 3, succeeded = 4, failed = 5, rejected = 6, expired = 7, not_found = 8 };
inline bool terminal(State s) { return s >= State::succeeded && s <= State::expired; }
inline const char* name(State s) {
    switch (s) {
    case State::receiving: return "RECEIVING"; case State::ready: return "READY";
    case State::running: return "RUNNING"; case State::succeeded: return "SUCCEEDED";
    case State::failed: return "FAILED"; case State::rejected: return "REJECTED";
    case State::expired: return "EXPIRED"; default: return "NOT_FOUND";
    }
}
inline std::string hex(const Id& id) {
    std::string out; for (auto b : id) { out += "0123456789abcdef"[b >> 4]; out += "0123456789abcdef"[b & 15]; } return out;
}
inline Id parse_id(const std::string& text) {
    if (text.size() != 32) throw std::runtime_error("request ID must contain 32 hexadecimal digits");
    Id id{};
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto digit = std::string("0123456789abcdef").find(text[i]);
        if (digit == std::string::npos) throw std::runtime_error("invalid request ID");
        id[i / 2] = static_cast<std::uint8_t>((id[i / 2] << 4) | digit);
    }
    if (id == Id{}) throw std::runtime_error("invalid zero request ID");
    return id;
}
inline Id new_id() {
    Id id{}; std::size_t n = 0;
    while (n < id.size()) {
        auto r = ::getrandom(id.data() + n, id.size() - n, GRND_NONBLOCK);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) throw std::runtime_error("cannot generate control request ID");
        n += static_cast<std::size_t>(r);
    }
    return id;
}
struct Frame {
    Kind kind = Kind::status;
    State state = State::receiving;
    Id id{};
    std::uint32_t offset = 0, total = 0;
    std::string command, data;
};
inline std::vector<std::uint8_t> encode(const Frame& f) {
    if (f.command.size() > 256) throw std::runtime_error("control command too long");
    std::vector<std::uint8_t> bytes(32 + f.command.size() + f.data.size());
    bytes[0] = 1; bytes[1] = static_cast<std::uint8_t>(f.kind); bytes[2] = static_cast<std::uint8_t>(f.state);
    std::copy(f.id.begin(), f.id.end(), bytes.begin() + 4);
    store_be32(bytes.data() + 20, f.offset); store_be32(bytes.data() + 24, f.total);
    store_be16(bytes.data() + 28, static_cast<std::uint16_t>(f.command.size()));
    std::copy(f.command.begin(), f.command.end(), bytes.begin() + 32);
    std::copy(f.data.begin(), f.data.end(), bytes.begin() + 32 + static_cast<std::ptrdiff_t>(f.command.size()));
    return bytes;
}
inline bool decode(const std::vector<std::uint8_t>& b, Frame& f) {
    if (b.size() < 32 || b[0] != 1 || b[1] < 1 || b[1] > 5 || b[2] < 1 || b[2] > 8 || b[3] || b[30] || b[31]) return false;
    auto n = load_be16(b.data() + 28);
    if (n > 256 || b.size() < 32u + n) return false;
    f.kind = static_cast<Kind>(b[1]); f.state = static_cast<State>(b[2]);
    std::copy_n(b.begin() + 4, 16, f.id.begin());
    f.offset = load_be32(b.data() + 20); f.total = load_be32(b.data() + 24);
    f.command.assign(b.begin() + 32, b.begin() + 32 + n); f.data.assign(b.begin() + 32 + n, b.end());
    if (f.kind != Kind::put && !f.command.empty()) return false;
    if ((f.kind == Kind::status || f.kind == Kind::finish || f.kind == Kind::confirmed) && !f.data.empty()) return false;
    return f.id != Id{};
}
} // namespace remote_control

class RemoteControl {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    using Id = remote_control::Id;
    using Frame = remote_control::Frame;
    using Kind = remote_control::Kind;
    using State = remote_control::State;
    using Sender = std::function<void(const std::vector<std::uint8_t>&)>;
    using Executor = std::function<ControlResponse(const ControlRequest&)>;
    struct Result { int exit = 1; std::string body; };
private:
    struct Incoming {
        ControlRequest request;
        std::size_t total = 0;
        State state = State::receiving;
        std::string response;
        Time started{}, progress{};
        bool acknowledged = false, cheap = false;
        std::uint64_t order = 0;
    };
    struct Outgoing {
        ControlRequest request;
        std::size_t sent = 0, total = 0;
        std::string response;
        State state = State::receiving;
        bool known = false, seen = false, finishing = false, query_only = false;
        unsigned retries = 5, attempts = 0;
        std::chrono::milliseconds wait{250};
        Time next{}, completed{};
        std::optional<Result> result;
    };
    std::map<Id, Incoming> incoming_;
    std::map<Id, Outgoing> outgoing_;
    std::uint64_t order_ = 0;
    static constexpr std::size_t history = 128, active_limit = 8;
    // Reserve response capacity before accepting an operation; never evict an
    // unacknowledged result to admit a new request.
    static constexpr std::size_t memory_limit = 16 * control_max_body;
    std::size_t chunk_ = 512;
    ControlAccess access_;
    Sender send_;
    Executor execute_;
    bool slot_busy() const {
        for (const auto& p : incoming_) if (!p.second.cheap && !remote_control::terminal(p.second.state)) return true;
        return false;
    }
    void admit_outgoing() {
        std::size_t active = 0, reserved = 0;
        for (const auto& p : outgoing_) {
            if (!p.second.result) { ++active; reserved += 2 * control_max_body; }
            else reserved += p.second.request.body.size() + p.second.response.size() + p.second.result->body.size();
        }
        if (active >= active_limit) throw std::runtime_error("local remote control busy");
        while (outgoing_.size() >= history || reserved + 2 * control_max_body > memory_limit) {
            auto oldest = outgoing_.end();
            for (auto i = outgoing_.begin(); i != outgoing_.end(); ++i)
                if (i->second.result && (oldest == outgoing_.end() || i->second.completed < oldest->second.completed)) oldest = i;
            if (oldest == outgoing_.end()) throw std::runtime_error("local remote control memory limit");
            reserved -= oldest->second.request.body.size() + oldest->second.response.size() + oldest->second.result->body.size();
            outgoing_.erase(oldest);
        }
    }
    bool admit() {
        std::size_t active = 0, reserved = 0;
        for (const auto& p : incoming_) {
            const auto& r = p.second;
            if (!remote_control::terminal(r.state)) { ++active; reserved += control_max_body + r.total; }
            else reserved += r.response.size() + r.request.body.size();
        }
        if (active >= active_limit) return false;
        while (incoming_.size() >= history || reserved + 2 * control_max_body > memory_limit) {
            auto oldest = incoming_.end();
            for (auto i = incoming_.begin(); i != incoming_.end(); ++i)
                if (i->second.acknowledged && (oldest == incoming_.end() || i->second.order < oldest->second.order)) oldest = i;
            if (oldest == incoming_.end()) return false;
            reserved -= oldest->second.response.size() + oldest->second.request.body.size();
            incoming_.erase(oldest);
        }
        return true;
    }
    void emit(Frame f) { send_(remote_control::encode(f)); }
    void respond(const Id& id, const Incoming& r, std::size_t offset = 0, Kind kind = Kind::reply) {
        Frame f; f.kind = kind; f.id = id; f.state = r.state;
        if (!remote_control::terminal(r.state)) f.offset = static_cast<std::uint32_t>(r.request.body.size());
        else {
            f.total = static_cast<std::uint32_t>(r.response.size());
            f.offset = static_cast<std::uint32_t>(std::min(offset, r.response.size()));
            if (kind == Kind::reply) f.data = r.response.substr(f.offset, chunk_);
        }
        emit(std::move(f));
    }
    void refuse(const Id& id, const std::string& why) {
        Incoming r; r.state = State::rejected; r.response = why + "\n"; respond(id, r);
    }
    void terminal(Incoming& r, State state, std::string body) {
        r.state = state; r.response = std::move(body); r.order = ++order_;
        // Keep request bytes until eviction to detect changed duplicate uploads.
    }
    void send_next(const Id& id, Outgoing& r, Time now, bool retry = false) {
        Frame f; f.id = id;
        if (r.finishing) { f.kind = Kind::finish; f.state = r.state; f.total = static_cast<std::uint32_t>(r.response.size()); }
        else if (retry || r.known || r.sent == r.request.body.size()) {
            // The very first, empty-body request still needs PUT.
            if (!r.known && !retry) { f.kind = Kind::put; f.command = r.request.command; }
            else { f.kind = Kind::status; f.offset = static_cast<std::uint32_t>(r.response.size()); }
        } else f.kind = Kind::put;
        if (f.kind == Kind::put) {
            f.command = r.request.command; f.offset = static_cast<std::uint32_t>(r.sent);
            f.total = static_cast<std::uint32_t>(r.request.body.size()); f.data = r.request.body.substr(r.sent, chunk_);
        }
        emit(std::move(f)); r.next = now + r.wait;
    }
public:
    RemoteControl() = default;
    void configure(ControlAccess access, Sender sender, Executor executor) {
        access_ = access; send_ = std::move(sender); execute_ = std::move(executor);
    }
    void payload_limit(std::size_t bytes) { chunk_ = bytes > 288 ? bytes - 288 : 1; }
    Id submit(ControlRequest request, unsigned retries, std::chrono::milliseconds wait, Time now) {
        admit_outgoing();
        auto op = ControlDispatcher::parse(request.command);
        if (request.command.size() > 256 || request.body.size() != op.length) throw std::runtime_error("invalid remote request");
        Id id; do { id = remote_control::new_id(); } while (outgoing_.count(id));
        Outgoing r; r.request = std::move(request); r.retries = retries; r.wait = wait;
        auto i = outgoing_.emplace(id, std::move(r)).first; send_next(id, i->second, now); return id;
    }
    std::optional<Result> result(const Id& id) const {
        auto i = outgoing_.find(id); return i == outgoing_.end() ? std::optional<Result>(Result{1, "request no longer available\n"}) : i->second.result;
    }
    void release(const Id& id) { outgoing_.erase(id); }
    void receive(const std::vector<std::uint8_t>& bytes, Time now) {
        Frame f; if (!remote_control::decode(bytes, f)) return;
        if (f.kind == Kind::reply || f.kind == Kind::confirmed) {
            auto i = outgoing_.find(f.id); if (i == outgoing_.end() || i->second.result) return;
            auto& r = i->second;
            if (f.kind == Kind::confirmed) {
                if (!r.finishing || f.state != r.state || f.total != r.response.size()) return;
                r.result = Result{r.state == State::succeeded ? 0 : r.state == State::rejected ? 255 : 1, std::move(r.response)};
                r.completed = now; std::string{}.swap(r.request.body); return;
            }
            if (r.finishing) return;
            if (f.state == State::not_found) {
                if (r.seen || r.known) { r.result = Result{1, "remote request no longer available (peer restarted or history evicted)\n"}; r.completed = now; }
                else { r.sent = 0; send_next(f.id, r, now); }
                return;
            }
            r.seen = true;
            if (r.query_only && !remote_control::terminal(f.state)) {
                r.state = f.state; return;
            }
            if (f.state == State::receiving) {
                if (f.offset > r.request.body.size() || f.offset < r.sent) return;
                if (f.offset > r.sent) r.attempts = 0;
                r.sent = f.offset;
                // known means the peer has the complete input and only STATUS is needed.
                r.known = r.sent == r.request.body.size();
                send_next(f.id, r, now); return;
            }
            r.known = true;
            if (!remote_control::terminal(f.state)) { r.state = f.state; return; }
            if (f.total > control_max_body || f.offset != r.response.size() || f.data.size() > f.total - std::min(f.total, f.offset)) return;
            if (!r.response.empty() && (r.state != f.state || r.total != f.total)) return;
            r.state = f.state; r.total = f.total; r.response += f.data;
            if (!f.data.empty()) r.attempts = 0;
            if (r.response.size() == f.total) r.finishing = true;
            send_next(f.id, r, now); return;
        }
        auto i = incoming_.find(f.id);
        if (f.kind == Kind::status || f.kind == Kind::finish) {
            if (i == incoming_.end()) {
                Frame answer; answer.id = f.id; answer.kind = Kind::reply; answer.state = State::not_found;
                // Capacity refusals have no history slot. They never execute and
                // may therefore be confirmed statelessly.
                if (f.kind == Kind::finish && f.state == State::rejected) {
                    answer.kind = Kind::confirmed; answer.state = State::rejected; answer.total = f.total;
                }
                emit(answer); return;
            }
            auto& r = i->second;
            if (f.kind == Kind::finish && remote_control::terminal(r.state) && f.state == r.state && f.total == r.response.size()) {
                r.acknowledged = true; respond(f.id, r, 0, Kind::confirmed);
            } else respond(f.id, r, f.offset);
            return;
        }
        if (i == incoming_.end()) {
            if (f.offset != 0) { Frame answer; answer.id=f.id; answer.kind=Kind::reply; answer.state=State::not_found; emit(answer); return; }
            if (!admit()) { refuse(f.id, "remote control busy"); return; }
            Incoming r; r.request.command = f.command; r.total = f.total; r.started = r.progress = now;
            i = incoming_.emplace(f.id, std::move(r)).first;
            try {
                auto op = ControlDispatcher::parse(f.command);
                i->second.cheap = op.cheap;
                if (f.total != op.length || f.total > control_max_body) throw std::runtime_error("invalid request length");
                if (!access_.allows(op.permission)) throw std::runtime_error("control access denied");
                // Ignore our newly inserted entry when testing the exclusive slot.
                i->second.state = State::rejected;
                const bool busy = !op.cheap && slot_busy();
                i->second.state = State::receiving;
                if (busy) throw std::runtime_error("remote control busy");
            } catch (const std::exception& e) { terminal(i->second, State::rejected, std::string(e.what()) + "\n"); }
        }
        auto& r = i->second;
        if (r.request.command != f.command || r.total != f.total) { refuse(f.id, "request ID conflicts with another request"); return; }
        if (f.offset > r.total || f.data.size() > r.total - f.offset) { refuse(f.id, "invalid block bounds"); return; }
        if (f.offset < r.request.body.size()) {
            if (f.data.size() > r.request.body.size() - f.offset || r.request.body.compare(f.offset, f.data.size(), f.data) != 0) { refuse(f.id, "request ID conflicts with another body"); return; }
        } else if (f.offset == r.request.body.size() && r.state == State::receiving) {
            r.request.body += f.data; if (!f.data.empty()) r.progress = now;
        }
        if (r.state == State::receiving && r.request.body.size() == r.total) {
            r.state = State::ready;
            // Execution is separate from reassembly. Only cheap operations bypass
            // the bounded exclusive slot; response transfers never hold that slot.
            r.state = State::running;
            ControlResponse response;
            try { response = execute_(r.request); }
            catch (const std::bad_alloc&) {
                // Never leave an operation eligible for re-execution after an
                // allocation failure, including failures after a handler commit.
                r.state = State::failed; r.order = ++order_; r.response.clear();
                throw;
            }
            if (response.body.size() > control_max_body) response = {false, "control response exceeds 1 MiB; operation may already have completed\n"};
            terminal(r, response.rejected ? State::rejected : response.success ? State::succeeded : State::failed, std::move(response.body));
        }
        respond(f.id, r);
    }
    void tick(Time now) {
        for (auto& p : incoming_) {
            auto& r = p.second;
            if (r.state == State::receiving && (now - r.progress >= std::chrono::seconds(2) || now - r.started >= std::chrono::seconds(10))) {
                terminal(r, State::expired, "request upload expired before execution\n");
                std::string{}.swap(r.request.body);
            }
        }
        for (auto i = outgoing_.begin(); i != outgoing_.end();) {
            auto& r = i->second;
            if (r.result) { if (now - r.completed > std::chrono::minutes(5)) { i = outgoing_.erase(i); continue; } }
            else if (now >= r.next) {
                if (r.attempts++ >= r.retries) {
                    r.result = Result{1, "request " + remote_control::hex(i->first) + " timed out; last state=" + remote_control::name(r.state) + "; operation may still complete\n"}; r.completed = now;
                } else send_next(i->first, r, now, true);
            }
            ++i;
        }
    }
    Id query(const Id& id, unsigned retries, std::chrono::milliseconds wait, Time now) {
        auto existing = outgoing_.find(id);
        if (existing != outgoing_.end()) {
            if (!existing->second.result || existing->second.result->exit != 1) return id;
            outgoing_.erase(existing);
        }
        admit_outgoing();
        Outgoing r; r.known = r.seen = r.query_only = true; r.retries = retries; r.wait = wait;
        auto i = outgoing_.emplace(id, std::move(r)).first;
        send_next(id, i->second, now, true); return id;
    }
    void session_changed(Time now) {
        for (auto& p : outgoing_) if (!p.second.result) {
            p.second.result = Result{1, "request " + remote_control::hex(p.first) + " interrupted by session change; query status before retrying\n"};
            p.second.completed = now;
        }
        for (auto& p : incoming_) if (p.second.state == State::receiving)
            terminal(p.second, State::expired, "request upload interrupted by session change\n");
    }
    bool active() const {
        for (const auto& p : outgoing_) if (!p.second.result) return true;
        for (const auto& p : incoming_) if (!remote_control::terminal(p.second.state)) return true;
        return false;
    }
    void write_stats(std::ostream& out) const {
        std::size_t receiving = 0, outgoing = 0, unconfirmed = 0;
        for (const auto& p : incoming_) {
            if (p.second.state == State::receiving) ++receiving;
            if (remote_control::terminal(p.second.state) && !p.second.acknowledged) ++unconfirmed;
        }
        for (const auto& p : outgoing_) if (!p.second.result) ++outgoing;
        out << "control_remote_receiving=" << receiving << "\n"
            << "control_remote_outgoing=" << outgoing << "\n"
            << "control_remote_history=" << incoming_.size() << "\n"
            << "control_remote_unconfirmed=" << unconfirmed << "\n";
    }
    bool pending_results() const { return !outgoing_.empty(); }
};
} // namespace tuntom
