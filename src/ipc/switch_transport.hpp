#pragma once

#include "switch_mmap.hpp"
#include "retry_queue.hpp"
#include <atomic>
#include <ostream>

namespace tuntom::ipc {
struct FrameParts {
    const std::uint8_t *first = nullptr;
    std::size_t first_size = 0;
    const std::uint8_t *second = nullptr;
    std::size_t second_size = 0;
    std::size_t size() const { return first_size + second_size; }
};
struct alignas(64) TransportStats {
    std::atomic<std::uint64_t> calls{0}, records{0}, inline_frames{0}, mapped_frames{0}, batches{0},
        largest_batch{0}, pool_fallback{0}, invalid{0};
    static void add(std::atomic<std::uint64_t> &v, std::uint64_t n = 1) {
        v.store(v.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }
    void mapped(std::size_t n) {
        add(mapped_frames, n);
        if (n > largest_batch.load(std::memory_order_relaxed)) largest_batch.store(n, std::memory_order_relaxed);
    }
    void write(std::ostream &out, const std::string &prefix) const {
#define IPC_STAT(name) out << prefix << #name << '=' << name.load(std::memory_order_relaxed) << '\n';
        IPC_STAT(calls) IPC_STAT(records) IPC_STAT(inline_frames) IPC_STAT(mapped_frames)
        IPC_STAT(batches) IPC_STAT(largest_batch) IPC_STAT(pool_fallback) IPC_STAT(invalid)
#undef IPC_STAT
    }
};
// RX and TX each have one owner. Ownership can migrate only through the existing
// worker barrier. Neither direction touches the other's mutable fields.
class Transport {
    RetryQueue retry_;
    Parameters parameters_{};
    bool extended_ = false;
    alignas(64) Mapping input_;
    std::array<Ref, max_batch> received_{};
    std::size_t read_cursor_ = 0, read_count_ = 0;
    alignas(64) Mapping output_;
    std::array<Ref, max_batch> staged_{};
    std::size_t staged_count_ = 0;

    ssize_t bad_reference() { TransportStats::add(rx.invalid); read_cursor_ = read_count_ = 0; errno = EPROTO; return -1; }
    bool stage(const FrameParts &frame, Ref &ref) {
        if (!output_.reserve(static_cast<std::uint32_t>(frame.size()), ref)) return false;
        auto *destination = output_.data(ref.slot);
        if (frame.first_size) std::memcpy(destination, frame.first, frame.first_size);
        if (frame.second_size) std::memcpy(destination + frame.first_size, frame.second, frame.second_size);
        output_.publish(ref);
        return true;
    }
    ssize_t send_refs(int fd, const Ref *refs, std::size_t count) {
        const auto record = references(parameters_, output_.id(), refs, count);
        TransportStats::add(tx.calls);
        const auto sent = send_record(fd, record);
        if (sent != static_cast<ssize_t>(record.size)) {
            const int error = sent < 0 ? errno : EIO;
            for (std::size_t i = 0; i < count; ++i) output_.release(refs[i]);
            errno = error;
            return -1;
        }
        TransportStats::add(tx.records); tx.mapped(count);
        if (parameters_.caps & mmap_batch) TransportStats::add(tx.batches);
        return static_cast<ssize_t>(count);
    }
    ssize_t inline_send(int fd, const FrameParts &frame) {
        TransportStats::add(tx.calls);
        ssize_t sent;
        if (!frame.second_size) {
            sent = ::send(fd, frame.first, frame.first_size, MSG_DONTWAIT | MSG_NOSIGNAL);
        } else {
            iovec parts[]{{const_cast<std::uint8_t *>(frame.first), frame.first_size},
                          {const_cast<std::uint8_t *>(frame.second), frame.second_size}};
            msghdr msg{}; msg.msg_iov = parts; msg.msg_iovlen = 2;
            sent = ::sendmsg(fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
        }
        if (sent != static_cast<ssize_t>(frame.size())) { if (sent >= 0) errno = EIO; return -1; }
        TransportStats::add(tx.records); TransportStats::add(tx.inline_frames);
        return 1;
    }
public:
    TransportStats rx, tx;
    Transport() = default;
    Transport(const Transport &) = delete;
    Transport &operator=(const Transport &) = delete;
    void reset() {
        retry_.discard();
        input_.reset(); output_.reset(); read_count_ = read_cursor_ = staged_count_ = 0;
        parameters_ = {}; extended_ = false;
    }
    void configure(const Parameters &v, Mapping input = {}, Mapping output = {}) {
        reset(); parameters_ = v; extended_ = true;
        input_ = std::move(input); output_ = std::move(output);
    }
    void disable_mmap() {
        input_.reset(); output_.reset(); parameters_.inline_only();
    }
    void close_pool_fds() { input_.close_fd(); output_.close_fd(); }
    const Parameters &parameters() const { return parameters_; }
    bool extended() const { return extended_; }
    bool mapped() const { return parameters_.caps & mmap_ref; }
    std::size_t batch_limit() const { return mapped() ? parameters_.batch : 1; }
    bool receive_pending() const { return read_cursor_ < read_count_; }
    std::uint64_t mapping_bytes() const { return input_.size() + output_.size(); }
    int input_fd() const { return input_.fd(); }
    int output_fd() const { return output_.fd(); }

    // Submit an ordered prefix, returning logical frames accepted, not record
    // bytes. A positive prefix must never be replayed, even if inline fallback
    // prevented submitting the remainder. No TX pointer is retained here.
    ssize_t send_batch(int fd, const FrameParts *frames, std::size_t count) {
        if (!count) return 0;
        if (staged_count_ || !retry_.empty()) { errno = EBUSY; return -1; } // Do not bypass append()/flush() work.
        if (extended_ && (frames[0].size() < min_frame || frames[0].size() > parameters_.frame_limit)) {
            errno = EMSGSIZE; return -1;
        }
        if (!mapped() || frames[0].size() > parameters_.capacity) return inline_send(fd, frames[0]);
        std::array<Ref, max_batch> refs{};
        std::size_t n = 0;
        for (; n < std::min(count, batch_limit()); ++n) {
            if (frames[n].size() < min_frame || frames[n].size() > parameters_.capacity) break;
            if (!stage(frames[n], refs[n])) {
                if (errno == EOVERFLOW) {
                    for (std::size_t i = 0; i < n; ++i) output_.release(refs[i]);
                    return -1;
                }
                TransportStats::add(tx.pool_fallback);
                break;
            }
        }
        return n ? send_refs(fd, refs.data(), n) : inline_send(fd, frames[0]);
    }
    ssize_t send(int fd, const FrameParts &frame) {
        return send_batch(fd, &frame, 1) == 1 ? static_cast<ssize_t>(frame.size()) : -1;
    }

    // Client producers already process bounded RR slices. They may copy directly
    // into shared slots during that slice and flush once at its end, without
    // an extra private payload copy or a timer. A full batch flushes immediately.
    // Outcomes count actual successful socket submissions and drops, never merely
    // staged frames. Caller MUST flush before sleeping or leaving the data slice.
    using Outcome = RetryQueue::Outcome;
    bool retry_writable(RetryQueue::Time now) const { return retry_.writable(now); }
    int retry_timeout(RetryQueue::Time now, int limit) const { return retry_.timeout(now, limit); }
    Outcome flush(int fd) {
        auto out = retry_.flush(RetryQueue::Clock::now(), [&](const std::uint8_t* data, std::size_t size) -> ssize_t {
            return inline_send(fd, {data,size}) == 1 ? static_cast<ssize_t>(size) : -1;
        });
        if (!staged_count_ || out.error) return out;
        const auto count = staged_count_;
        staged_count_ = 0;
        if (send_refs(fd, staged_.data(), count) < 0) {
            if (retry_error()) {
                const auto now = RetryQueue::Clock::now();
                // send_refs rolled back ownership, but no new reservation has
                // overwritten these payloads. Copy before another append.
                for (std::size_t i=0; i<count; ++i)
                    out.add(retry_.enqueue(output_.data(staged_[i].slot), staged_[i].length, nullptr, 0, now));
                retry_.blocked(now);
            } else { out.drops += count; out.error = errno; }
        } else {
            out.frames += count;
            for (std::size_t i=0; i<count; ++i) out.bytes += staged_[i].length;
        }
        return out;
    }
    Outcome append(int fd, const FrameParts &frame) {
        Outcome out;
        if (extended_ && (frame.size() < min_frame || frame.size() > parameters_.frame_limit)) {
            out.drops = 1; out.error = EMSGSIZE; return out;
        }
        out.add(retry_.expire(RetryQueue::Clock::now()));
        if (!retry_.empty()) {
            out.add(retry_.enqueue(frame.first, frame.first_size, frame.second, frame.second_size, RetryQueue::Clock::now()));
            return out;
        }
        if (mapped() && frame.size() <= parameters_.capacity) {
            if (stage(frame, staged_[staged_count_])) {
                if (++staged_count_ == batch_limit()) out.add(flush(fd));
                return out;
            }
            if (errno == EOVERFLOW) { out.drops = 1; out.error = errno; return out; }
            TransportStats::add(tx.pool_fallback);
        }
        out.add(flush(fd)); // Earlier mapped frames must precede this inline frame.
        if (out.error) { ++out.drops; return out; }
        if (!retry_.empty()) {
            out.add(retry_.enqueue(frame.first, frame.first_size, frame.second, frame.second_size, RetryQueue::Clock::now()));
            return out;
        }
        if (inline_send(fd, frame) == 1) { ++out.frames; out.bytes += frame.size(); }
        else if (retry_error()) {
            const auto now = RetryQueue::Clock::now();
            out.add(retry_.enqueue(frame.first,frame.first_size,frame.second,frame.second_size,now));
            retry_.blocked(now);
        } else { ++out.drops; out.error = errno; }
        return out;
    }
    Outcome discard_staged() {
        Outcome out = retry_.discard(); out.drops += staged_count_;
        for (std::size_t i = 0; i < staged_count_; ++i) output_.release(staged_[i]);
        staged_count_ = 0;
        return out;
    }
    ssize_t receive(int fd, std::uint8_t *buffer, std::size_t capacity) {
        if (!receive_pending()) {
            read_count_ = read_cursor_ = 0;
            TransportStats::add(rx.calls);
            if (!extended_) {
                const auto n = ::recv(fd, buffer, capacity, MSG_DONTWAIT | MSG_TRUNC);
                if (n > 0) { TransportStats::add(rx.records); TransportStats::add(rx.inline_frames); }
                return n;
            }
            // Small caller buffers still need room for the whole reference record.
            std::array<std::uint8_t, max_record> scratch;
            auto *record = capacity >= max_record ? buffer : scratch.data();
            const auto limit = std::max(capacity, max_record);
            const auto result = receive_record(fd, record, limit);
            if (result.size < 0) return -1;
            if (!result.valid || result.count) return bad_reference();
            if (!result.size) return 0;
            const auto n = static_cast<std::size_t>(result.size);
            TransportStats::add(rx.records);
            // Logical inline records include routed CONTROL v2 as well as DATA v1.
            // Their codecs validate the body after transport framing is removed.
            if (record[0] == switch_protocol_version || record[0] == 2) {
                if (n > parameters_.frame_limit) return bad_reference();
                if (record != buffer) std::memcpy(buffer, record, std::min(n, capacity));
                TransportStats::add(rx.inline_frames);
                return result.size; // Caller applies normal V1 decode / MSG_TRUNC handling.
            }
            if (n > limit || (result.flags & MSG_TRUNC) ||
                !decode_references(record, n, parameters_, input_.id(), received_, read_count_))
                return bad_reference();
            // Validate every reference before reading or releasing ANY slot.
            for (std::size_t i = 0; i < read_count_; ++i)
                if (input_.token(received_[i].slot) != received_[i].token) return bad_reference();
            if (record[4] == static_cast<std::uint8_t>(Type::batch)) TransportStats::add(rx.batches);
            rx.mapped(read_count_);
        }
        const auto ref = received_[read_cursor_++];
        if (input_.token(ref.slot) != ref.token) return bad_reference();
        std::memcpy(buffer, input_.data(ref.slot), std::min<std::size_t>(ref.length, capacity));
        input_.release(ref); // No shared payload pointers escape receive().
        return static_cast<ssize_t>(ref.length);
    }
    void write_stats(std::ostream &out, const std::string &prefix) const {
        out << prefix << "version=" << (extended_ ? 2 : 1) << '\n'
            << prefix << "mmap=" << (mapped() ? 1 : 0) << '\n'
            << prefix << "batch_limit=" << batch_limit() << '\n'
            << prefix << "mapping_bytes=" << mapping_bytes() << '\n';
        retry_.stats(out, prefix + "retry_");
        rx.write(out, prefix + "rx_"); tx.write(out, prefix + "tx_");
    }
};
} // namespace tuntom::ipc
