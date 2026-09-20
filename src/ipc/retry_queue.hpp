#pragma once
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <string>
#include <vector>

namespace tuntom::ipc {
// Owned canonical records, never mmap references. One owner per queue.
class RetryQueue {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr std::size_t capacity = 128, byte_limit = 256 * 1024, record_limit = 128 * 1024;
    static constexpr auto max_age = std::chrono::milliseconds(100);
    struct Outcome {
        std::uint64_t frames = 0, bytes = 0, drops = 0, backpressure = 0;
        int error = 0;
        void add(const Outcome& o) {
            frames += o.frames; bytes += o.bytes; drops += o.drops; backpressure += o.backpressure;
            if (o.error) error = o.error;
        }
    };
    bool empty() const { return !size_; }
    bool writable(Time now) const { return size_ && now >= retry_at_; }
    int timeout(Time now, int limit) const {
        if (empty()) return limit;
        auto end = entries_[head_].created + max_age;
        if (retry_at_ > now) end = std::min(end, retry_at_);
        return static_cast<int>(std::min<long long>(limit, std::max<long long>(0,
            std::chrono::ceil<std::chrono::milliseconds>(end - now).count())));
    }
    Outcome expire(Time now) {
        Outcome out;
        while (size_ && now - entries_[head_].created >= max_age) {
            pop(); ++expired_; ++out.drops; ++out.backpressure;
        }
        return out;
    }
    Outcome discard() {
        Outcome out; out.drops = size_; discarded_ += size_;
        while (size_) pop();
        return out;
    }
    void blocked(Time now) { ++eagain_; retry_at_ = now + std::chrono::milliseconds(1); }
    Outcome enqueue(const std::uint8_t* a, std::size_t an, const std::uint8_t* b,
                    std::size_t bn, Time now) {
        auto out = expire(now);
        if (an > record_limit || bn > record_limit - an || size_ == capacity || an + bn > byte_limit - bytes_) {
            ++out.drops; ++out.backpressure; ++full_; return out;
        }
        auto& e = entries_[(head_ + size_) % capacity];
        e.wire.resize(an + bn);
        if (an) std::memcpy(e.wire.data(), a, an);
        if (bn) std::memcpy(e.wire.data() + an, b, bn);
        e.created = now; ++size_; bytes_ += an + bn; ++enqueued_;
        high_water_ = std::max(high_water_, size_);
        return out;
    }
    template<class Send> Outcome flush(Time now, Send&& send) {
        auto out = expire(now);
        if (!writable(now)) return out;
        for (std::size_t i = 0; i < capacity && size_; ++i) {
            out.add(expire(Clock::now()));
            if (!size_) break;
            auto& wire = entries_[head_].wire;
            const auto n = send(wire.data(), wire.size());
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                blocked(Clock::now()); break;
            }
            if (n < 0 || static_cast<std::size_t>(n) != wire.size()) {
                out.error = n < 0 ? errno : EIO;
                ++out.drops; ++errors_; pop(); break;
            }
            ++out.frames; out.bytes += wire.size(); ++sent_; pop();
        }
        return out;
    }
    template<class Send> Outcome submit(const std::uint8_t* data, std::size_t size, Time now, Send&& send) {
        auto out = expire(now);
        if (!empty()) { out.add(enqueue(data,size,nullptr,0,now)); return out; }
        const auto n = send(data,size);
        if (n == static_cast<decltype(n)>(size)) { ++out.frames; out.bytes += size; return out; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            out.add(enqueue(data,size,nullptr,0,now)); blocked(now); return out;
        }
        out.error = n < 0 ? errno : EIO; ++out.drops; ++errors_; return out;
    }
    void stats(std::ostream& out, const std::string& p) const {
        out << p << "packets=" << size_ << '\n' << p << "bytes=" << bytes_ << '\n'
            << p << "capacity=" << capacity << '\n' << p << "byte_limit=" << byte_limit << '\n'
            << p << "max_age_ms=" << max_age.count() << '\n' << p << "high_water=" << high_water_ << '\n'
            << p << "enqueued=" << enqueued_ << '\n' << p << "sent=" << sent_ << '\n'
            << p << "eagain=" << eagain_ << '\n' << p << "capacity_drops=" << full_ << '\n'
            << p << "expired=" << expired_ << '\n' << p << "discarded=" << discarded_ << '\n'
            << p << "error_drops=" << errors_ << '\n';
    }
private:
    struct Entry { std::vector<std::uint8_t> wire; Time created{}; };
    std::array<Entry,capacity> entries_{};
    std::size_t head_=0, size_=0, bytes_=0, high_water_=0;
    Time retry_at_{};
    std::uint64_t enqueued_=0, sent_=0, eagain_=0, full_=0, expired_=0, discarded_=0, errors_=0;
    void pop() {
        bytes_ -= entries_[head_].wire.size(); entries_[head_].wire.clear();
        head_ = (head_ + 1) % capacity; --size_;
        if (!size_) retry_at_ = {};
    }
};
}
