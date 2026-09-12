#pragma once

#include "../ipc/switch_protocol.hpp"
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>

namespace tuntom::mp {

// Ownership changes only while both endpoints are at the scheduler barrier.
template <class T> class Spsc {
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
    const std::size_t count_;
    std::unique_ptr<T *[]> slots_;

  public:
    explicit Spsc(std::size_t capacity) : count_(capacity + 1), slots_(new T *[count_]) {
        if (capacity == 0)
            throw std::invalid_argument("Empty SPSC queue");
        static_assert(std::atomic<std::size_t>::is_always_lock_free);
    }
    bool push(T *item) {
        const auto current = write_.load(std::memory_order_relaxed);
        const auto next = current + 1 == count_ ? 0 : current + 1;
        if (next == read_.load(std::memory_order_acquire))
            return false;
        slots_[current] = item;
        write_.store(next, std::memory_order_release);
        return true;
    }
    T *pop() {
        const auto current = read_.load(std::memory_order_relaxed);
        if (current == write_.load(std::memory_order_acquire))
            return nullptr;
        T *item = slots_[current];
        read_.store(current + 1 == count_ ? 0 : current + 1, std::memory_order_release);
        return item;
    }
};

constexpr std::size_t wire_capacity =
    switch_base_header_size + switch_max_labels * switch_label_size + 65535;

struct alignas(64) Buffer {
    std::atomic<bool> used{false}; // FREE/USED is deliberately the first member.
    std::size_t size = 0;
    std::uint8_t data[wire_capacity];
    Buffer() {} // Leave unused payload pages untouched.
    void release() { used.store(false, std::memory_order_release); }
};

// Per ingress port, so buffers and the circular cursor survive RX migration.
class Pool {
    std::unique_ptr<Buffer[]> buffers_;
    std::size_t count_, cursor_ = 0;

  public:
    explicit Pool(std::size_t count) : buffers_(new Buffer[count]), count_(count) {
        if (count == 0)
            throw std::invalid_argument("Empty buffer pool");
        static_assert(std::atomic<bool>::is_always_lock_free);
    }
    Buffer *acquire() {
        for (std::size_t n = 0; n < count_; ++n) {
            auto *buffer = &buffers_[cursor_];
            cursor_ = cursor_ + 1 == count_ ? 0 : cursor_ + 1;
            if (!buffer->used.load(std::memory_order_acquire)) {
                buffer->used.store(true, std::memory_order_relaxed);
                return buffer;
            }
        }
        return nullptr;
    }
    std::size_t in_use() const {
        std::size_t count = 0;
        for (std::size_t i = 0; i < count_; ++i)
            count += buffers_[i].used.load(std::memory_order_relaxed);
        return count;
    }
};

// One eventfd per generic worker. Packet publication wakes only its TX owner.
// arm -> recheck queues -> poll pairs with publish -> notify (acq_rel RMWs).
class Wake {
    alignas(64) std::atomic<unsigned> sleeping_{0};
    int fd_;

  public:
    Wake() : fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
        if (fd_ < 0)
            throw std::runtime_error("Cannot create worker eventfd");
    }
    ~Wake() { ::close(fd_); }
    Wake(const Wake &) = delete;
    Wake &operator=(const Wake &) = delete;
    int fd() const { return fd_; }
    void arm() { sleeping_.exchange(1, std::memory_order_acq_rel); }
    void cancel() { sleeping_.store(0, std::memory_order_release); }
    void poke() const {
        const std::uint64_t one = 1;
        while (::write(fd_, &one, sizeof(one)) < 0 && errno == EINTR) {
        }
        // EAGAIN means an event is already pending. The FD stays open until join.
    }
    bool notify() {
        if (!sleeping_.exchange(0, std::memory_order_acq_rel))
            return false;
        poke();
        return true;
    }
    void drain() const {
        std::uint64_t count;
        for (;;) {
            if (::read(fd_, &count, sizeof(count)) == sizeof(count))
                continue;
            if (errno != EINTR)
                return;
        }
    }
};

} // namespace tuntom::mp
