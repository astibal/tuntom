#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>

// One wake object per TX worker. Unlike a futex-only gate, this can be polled
// together with several blocked output sockets. Producer: publish -> notify.
// Consumer: arm -> recheck every eligible queue -> poll. The acq_rel exchanges
// preserve the original gate's publication/sleep handshake.
class PollWake {
  alignas(64) std::atomic<std::uint32_t> sleeping_{0};
  int fd_;

public:
  PollWake() : fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (fd_ < 0)
      throw std::runtime_error("eventfd");
  }
  ~PollWake() { ::close(fd_); }
  PollWake(const PollWake &) = delete;
  PollWake &operator=(const PollWake &) = delete;
  int fd() const { return fd_; }
  void arm() { sleeping_.exchange(1, std::memory_order_acq_rel); }
  void cancel() { sleeping_.store(0, std::memory_order_release); }
  bool notify() {
    if (!sleeping_.exchange(0, std::memory_order_acq_rel))
      return false;
    const std::uint64_t one = 1;
    while (::write(fd_, &one, sizeof(one)) < 0 && errno == EINTR) {
    }
    // EAGAIN means a notification is already pending in the eventfd.
    return true;
  }
  void drain() {
    std::uint64_t count;
    for (;;) {
      if (::read(fd_, &count, sizeof(count)) == sizeof(count))
        continue;
      if (errno == EINTR)
        continue;
      return;
    }
  }
};
