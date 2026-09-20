#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <linux/futex.h>
#include <memory>
#include <stdexcept>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace switch_mt {

// One producer owns write, one consumer owns read. Slots contain plain
// pointers. Allocation and construction must precede publication to the worker
// threads.
template <class T> class Spsc {
  alignas(64) std::atomic<std::size_t> write_{0};
  alignas(64) std::atomic<std::size_t> read_{0};
  const std::size_t count_;
  std::unique_ptr<T *[]> slots_;

public:
  explicit Spsc(std::size_t capacity)
      : count_(capacity + 1), slots_(new T *[count_]) {
    if (capacity == 0)
      throw std::invalid_argument("empty SPSC");
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
  }
  bool push(T *item) {
    const auto w = write_.load(std::memory_order_relaxed);
    const auto next = w + 1 == count_ ? 0 : w + 1;
    if (next == read_.load(std::memory_order_acquire))
      return false;
    slots_[w] = item;
    write_.store(next, std::memory_order_release);
    return true;
  }
  T *pop() {
    const auto r = read_.load(std::memory_order_relaxed);
    if (r == write_.load(std::memory_order_acquire))
      return nullptr;
    T *item = slots_[r];
    read_.store(r + 1 == count_ ? 0 : r + 1, std::memory_order_release);
    return item;
  }
};

// The queue itself needs no RMW. Blocking TX workers do need a notification
// handshake: explicitly measure this additional per-publication exchange.
// Consumer: arm -> recheck queues -> wait. Producer: publish -> notify.
// The acq_rel exchanges order publication against the consumer's recheck.
class WakeGate {
  alignas(64) std::atomic<std::uint32_t> sleeping_{0};

public:
  WakeGate() { static_assert(std::atomic<std::uint32_t>::is_always_lock_free); }
  void arm() { sleeping_.exchange(1, std::memory_order_acq_rel); }
  void cancel() { sleeping_.store(0, std::memory_order_release); }
  bool notify() {
    if (sleeping_.exchange(0, std::memory_order_acq_rel) == 0)
      return false;
    ::syscall(SYS_futex, &sleeping_, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr,
              0);
    return true;
  }
  void wait() {
    const timespec timeout{0, 20000000}; // bounded shutdown latency
    ::syscall(SYS_futex, &sleeping_, FUTEX_WAIT_PRIVATE, 1, &timeout, nullptr,
              0);
    cancel();
  }
};

constexpr std::size_t wire_capacity = 65535 + 8 + 8 * 8;
struct alignas(64) Buffer {
  std::atomic<bool> used{false}; // first member, per user's slot design
  std::size_t size = 0;
  std::uint8_t data[wire_capacity];
  Buffer() {} // do not zero 64 KiB of payload for every unused slot
  void release() { used.store(false, std::memory_order_release); }
};

class Pool {
  std::unique_ptr<Buffer[]> buffers_;
  std::size_t count_, cursor_ = 0;

public:
  explicit Pool(std::size_t count)
      : buffers_(new Buffer[count]), count_(count) {
    if (!count)
      throw std::invalid_argument("empty pool");
    static_assert(std::atomic<bool>::is_always_lock_free);
  }
  // Only the owning RX calls acquire. Any final TX owner may release.
  Buffer *acquire(std::uint64_t &probes) {
    for (std::size_t n = 0; n < count_; ++n) {
      Buffer *b = &buffers_[cursor_];
      cursor_ = cursor_ + 1 == count_ ? 0 : cursor_ + 1;
      ++probes;
      if (!b->used.load(std::memory_order_acquire)) {
        b->used.store(true, std::memory_order_relaxed);
        return b;
      }
    }
    return nullptr;
  }
  std::size_t in_use() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < count_; ++i)
      n += buffers_[i].used.load(std::memory_order_acquire);
    return n;
  }
};
} // namespace switch_mt
