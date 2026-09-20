#pragma once

#include "udp_endpoint.hpp"
#include <algorithm>
#include <chrono>
#include <ostream>

namespace tuntom {

// DATA/IPC only. Control and PMTUD probes keep their immediate-send semantics.
// A bounded FIFO of already encoded datagrams: never re-encrypt a retry.
class UdpTxQueue {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr std::size_t capacity = 64;
    static constexpr std::size_t byte_limit = 256 * 1024;
    static constexpr auto max_age = std::chrono::milliseconds(100);
    static constexpr auto retry_delay = std::chrono::milliseconds(1);

    bool fits(std::size_t count, std::size_t bytes) const {
        return count <= capacity - size_ && bytes <= byte_limit - bytes_;
    }
    void reject(std::size_t count) { capacity_drops_ += count; }

    bool empty() const { return size_ == 0; }
    bool writable_interest(Time now) const { return !empty() && now >= retry_at_; }
    int poll_timeout(Time now, int fallback) const {
        if (empty()) return fallback;
        auto deadline = entries_[head_].created + max_age;
        if (retry_at_ > now) deadline = std::min(deadline, retry_at_);
        const auto ms = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
        return static_cast<int>(std::min<long long>(fallback, std::max<long long>(0, ms)));
    }

    // Admit the entire unsent suffix or none of it. Partial admission would
    // waste space on a logical packet that can never be reassembled.
    bool append(const std::vector<std::uint8_t>* packets, std::size_t count, Time now) {
        expire(now);
        std::size_t bytes = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (packets[i].size() > 65535 || packets[i].size() > byte_limit - bytes) {
                capacity_drops_ += count;
                return false;
            }
            bytes += packets[i].size();
        }
        if (count > capacity - size_ || bytes > byte_limit - bytes_) {
            capacity_drops_ += count;
            return false;
        }
        // Prepare copies before publishing slots (strong exception guarantee).
        for (std::size_t i = 0; i < count; ++i) {
            auto& e = entries_[(head_ + size_ + i) % capacity];
            e.wire = packets[i];
            e.created = now;
        }
        size_ += count;
        bytes_ += bytes;
        enqueued_ += count;
        high_water_ = std::max(high_water_, size_);
        return true;
    }

    void blocked(Time now) { ++eagain_; retry_at_ = now + retry_delay; }
    void expire(Time now) {
        while (!empty() && now - entries_[head_].created >= max_age) {
            pop(); ++expired_;
        }
    }
    void discard() {
        discarded_ += size_;
        while (!empty()) pop();
    }

    // At most 64 syscalls per readiness event. An unexpected ready/EAGAIN
    // combination is gated for 1 ms to avoid spinning on a writable socket.
    template<class Send>
    UdpEndpoint::BatchResult flush(Time now, Send&& send) {
        expire(now);
        UdpEndpoint::BatchResult result;
        if (!writable_interest(now)) return result;
        for (std::size_t n = 0; n < capacity && !empty(); ++n) {
            const auto& wire = entries_[head_].wire;
            const auto sent = send(wire.data(), wire.size());
            if (sent < 0) {
                const int error = errno;
                if (error == EAGAIN || error == EWOULDBLOCK) { blocked(now); break; }
                if (error == EINTR) { retry_at_ = now + retry_delay; break; }
                result.error = error;
                pop(); ++error_drops_;
                break;
            }
            if (static_cast<std::size_t>(sent) != wire.size()) {
                result.error = EIO;
                pop(); ++error_drops_;
                break;
            }
            ++result.packets;
            result.bytes += static_cast<std::size_t>(sent);
            ++sent_;
            pop();
        }
        return result;
    }

    void stats(std::ostream& out) const {
        out << "udp_tx_queue_packets=" << size_ << "\nudp_tx_queue_bytes=" << bytes_
            << "\nudp_tx_queue_capacity=" << capacity << "\nudp_tx_queue_byte_limit=" << byte_limit
            << "\nudp_tx_queue_max_age_ms=" << max_age.count()
            << "\nudp_tx_queue_high_water=" << high_water_ << "\nudp_tx_queue_enqueued=" << enqueued_
            << "\nudp_tx_queue_sent=" << sent_ << "\nudp_tx_queue_eagain=" << eagain_
            << "\nudp_tx_queue_capacity_drops=" << capacity_drops_ << "\nudp_tx_queue_expired=" << expired_
            << "\nudp_tx_queue_discarded=" << discarded_ << "\nudp_tx_queue_error_drops=" << error_drops_ << '\n';
    }

private:
    struct Entry { std::vector<std::uint8_t> wire; Time created {}; };
    void pop() {
        bytes_ -= entries_[head_].wire.size();
        entries_[head_].wire.clear();
        head_ = (head_ + 1) % capacity;
        --size_;
        if (empty()) retry_at_ = Time{};
    }
    std::array<Entry, capacity> entries_;
    std::size_t head_ = 0, size_ = 0, bytes_ = 0, high_water_ = 0;
    Time retry_at_ {};
    std::uint64_t enqueued_ = 0, sent_ = 0, eagain_ = 0, capacity_drops_ = 0;
    std::uint64_t expired_ = 0, discarded_ = 0, error_drops_ = 0;
};
} // namespace tuntom
