#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <string_view>
#include <type_traits>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace tuntom {

// One producer (the event loop), one detached consumer. Never log from a signal
// handler or the writer. The process-global instance has no destructor: its
// storage remains valid until exit_group, even if the writer is stuck in I/O.
class AsyncLogger {
public:
    static constexpr std::size_t record_size = 1024;
    static constexpr std::size_t capacity = 64;
    static constexpr std::size_t stack_size = 64 * 1024;
    static constexpr off_t file_limit = 16 * 1024 * 1024;
    static constexpr unsigned rate_per_second = 20;
    static constexpr auto refill_interval = std::chrono::milliseconds(1000 / rate_per_second);
    using Clock = std::chrono::steady_clock;

    void ignore_sigpipe() noexcept {
        const int saved = errno;
        struct sigaction action {};
        action.sa_handler = SIG_IGN;
        ::sigemptyset(&action.sa_mask);
        // A dead log reader must not kill packet flow. Ignoring (not masking)
        // SIGPIPE turns broken-pipe writes into ordinary EPIPE errors.
        signals_ready_ = ::sigaction(SIGPIPE, &action, nullptr) == 0;
        if (not signals_ready_) ++start_errors_;
        // Check before sockets/TUN are opened: an inherited closed fd 2 may
        // otherwise be reused by packet I/O. Daemons keep stderr dedicated for
        // their entire lifetime; never send log text to a replacement endpoint.
        sink_available_ = ::fcntl(STDERR_FILENO, F_GETFD) >= 0;
        if (not sink_available_) ++start_errors_;
        errno = saved;
    }

    // Call once, after any privilege drop and before entering the event loop.
    // Thread creation failure disables logging; it never becomes a fatal error
    // or a synchronous fallback. No retries, replacement threads or join.
    void start() noexcept {
        const int saved = errno;
        if (attempted_) return;
        attempted_ = true;
        if (not signals_ready_ or not sink_available_) { discard_pending(); return; }
        pthread_attr_t attr;
        int error = ::pthread_attr_init(&attr);
        if (error == 0) {
            error = ::pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            if (error == 0) error = ::pthread_attr_setstacksize(&attr, stack_size);
            sigset_t blocked, previous;
            ::sigfillset(&blocked);
            if (error == 0) error = ::pthread_sigmask(SIG_BLOCK, &blocked, &previous);
            if (error == 0) {
                pthread_t thread;
                error = ::pthread_create(&thread, &attr, run, this);
                // The writer inherits blocked signals; daemon signal handlers
                // continue to execute only on the main thread.
                ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
            }
            ::pthread_attr_destroy(&attr);
        }
        if (error == 0) started_ = true;
        else { ++start_errors_; discard_pending(); }
        errno = saved;
    }

    bool started() const noexcept { return started_; }
    bool attempted() const noexcept { return attempted_; }
    bool sink_available() const noexcept { return sink_available_; }

    void submit(const char* data, std::size_t size, bool truncated,
                Clock::time_point now = Clock::now()) noexcept {
        if (truncated) ++truncated_;
        // Token bucket: bounded diagnostic work/output under per-packet errors
        // or --debug floods (DoS protection). No sleeps in the producer.
        if (now >= refill_at_) {
            const auto steps = (now - refill_at_) / refill_interval;
            tokens_ = static_cast<unsigned>(std::min<std::int64_t>(
                capacity, tokens_ + steps + 1));
            refill_at_ = now + refill_interval;
        }
        if (tokens_ == 0) { ++rate_limited_; ++dropped_; return; }
        --tokens_;
        const auto write = write_.load(std::memory_order_relaxed);
        if ((attempted_ and not started_) or
            write - read_.load(std::memory_order_acquire) == capacity) {
            ++dropped_;
            return;
        }
        auto& record = records_[write % capacity];
        record.size = std::min(size, record_size);
        std::memcpy(record.data.data(), data, record.size);
        write_.store(write + 1, std::memory_order_release);
    }

    void write_stats(std::ostream& out) const {
        out << "log_worker_started=" << (started_ ? 1 : 0) << "\n"
            << "log_start_errors=" << start_errors_ << "\n"
            << "log_dropped=" << dropped_.load(std::memory_order_relaxed) << "\n"
            << "log_rate_limited=" << rate_limited_ << "\n"
            << "log_truncated=" << truncated_ << "\n"
            << "log_write_errors=" << write_errors_.load(std::memory_order_relaxed) << "\n"
            << "log_last_errno=" << last_errno_.load(std::memory_order_relaxed) << "\n"
            << "log_file_limit_drops=" << file_limit_drops_.load(std::memory_order_relaxed) << "\n";
    }

private:
    struct Record { std::array<char, record_size> data {}; std::size_t size = 0; };
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<int>::is_always_lock_free);
    std::array<Record, capacity> records_ {};
    std::atomic<std::uint64_t> write_ {0}, read_ {0};
    std::atomic<std::uint64_t> dropped_ {0}, write_errors_ {0}, file_limit_drops_ {0};
    std::atomic<int> last_errno_ {0};
    std::uint64_t rate_limited_ = 0, truncated_ = 0, start_errors_ = 0;
    bool signals_ready_ = false, sink_available_ = false, attempted_ = false, started_ = false;
    unsigned tokens_ = capacity;
    Clock::time_point refill_at_ {};

    // Only called when creation failed: there cannot be a consumer yet.
    void discard_pending() noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        dropped_.fetch_add(write - read_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        read_.store(write, std::memory_order_relaxed);
    }

    static void pause(long milliseconds) noexcept {
        const timespec delay {milliseconds / 1000, (milliseconds % 1000) * 1000000};
        ::nanosleep(&delay, nullptr);
    }

    bool write_record(const Record& record) noexcept {
        struct stat status {};
        if (::fstat(STDERR_FILENO, &status) != 0) return false;
        if (S_ISREG(status.st_mode)) {
            // Bound inherited file logs without deleting/truncating someone
            // else's file. External copytruncate permits logging to resume;
            // SEEK_END avoids a sparse hole at the old offset after truncation.
            const off_t end = ::lseek(STDERR_FILENO, 0, SEEK_END);
            if (end < 0) return false;
            if (end > file_limit - static_cast<off_t>(record.size)) {
                ++file_limit_drops_;
                errno = EFBIG;
                return false;
            }
        }
        // Bounded attempts even for pathological one-byte writes/EINTR. A
        // partial record may be lost; logging must not retry forever or spin.
        std::size_t offset = 0;
        for (unsigned attempt = 0; attempt < 4 and offset < record.size; ++attempt) {
            const auto count = ::write(STDERR_FILENO, record.data.data() + offset,
                                       record.size - offset);
            if (count < 0) return false;
            if (count == 0) { errno = EIO; return false; }
            offset += static_cast<std::size_t>(count);
        }
        if (offset == record.size) return true;
        errno = EIO;
        return false;
    }

    static void* run(void* context) noexcept {
        auto& self = *static_cast<AsyncLogger*>(context);
        for (;;) {
            const auto read = self.read_.load(std::memory_order_relaxed);
            if (read == self.write_.load(std::memory_order_acquire)) {
                pause(100);
                continue;
            }
            const bool ok = self.write_record(self.records_[read % capacity]);
            if (not ok) {
                self.last_errno_.store(errno, std::memory_order_relaxed);
                ++self.write_errors_;
                ++self.dropped_;
            }
            self.read_.store(read + 1, std::memory_order_release);
            if (not ok) pause(1000); // EPIPE/ENOSPC/EAGAIN/etc.: no recursive logging.
        }
    }
};

static_assert(std::is_trivially_destructible_v<AsyncLogger>);
inline AsyncLogger logger;

// Bounded stack formatting: no iostream locks, locale machinery, heap storage
// or sink I/O on the packet path. Each full expression publishes one record.
class LogLine {
public:
    explicit LogLine(bool enabled = true) noexcept : saved_errno_(errno), enabled_(enabled) {}
    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;
    ~LogLine() noexcept {
        if (enabled_) {
            if (truncated_) {
                constexpr std::string_view suffix = " [truncated]";
                size_ = std::min(size_, AsyncLogger::record_size - 1 - suffix.size());
                append(suffix);
            }
            if (size_ == 0 or data_[size_ - 1] != '\n') data_[size_++] = '\n';
            logger.submit(data_.data(), size_, truncated_);
        }
        errno = saved_errno_;
    }
    LogLine& operator<<(std::string_view value) noexcept { append(value); return *this; }
    LogLine& operator<<(const char* value) noexcept {
        if (enabled_) {
            if (not value) value = "(null)";
            // Copy only up to the terminator or remaining space. Keeping the
            // source bound in this loop also avoids GCC's fortified memcpy
            // warning when differently sized string literals are inlined.
            while (size_ < AsyncLogger::record_size - 1 and *value != '\0')
                data_[size_++] = *value++;
            truncated_ = truncated_ or *value != '\0';
        }
        return *this;
    }
    LogLine& operator<<(bool value) noexcept { return *this << (value ? "1" : "0"); }
    template<class T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
    LogLine& operator<<(T value) noexcept {
        if (enabled_) {
            char number[32];
            const auto result = std::to_chars(number, number + sizeof(number), value);
            append({number, static_cast<std::size_t>(result.ptr - number)});
        }
        return *this;
    }
    void hex_byte(std::uint8_t value) noexcept {
        constexpr char digits[] = "0123456789abcdef";
        const char text[] {' ', digits[value >> 4], digits[value & 15]};
        append({text, sizeof(text)});
    }
private:
    void append(std::string_view value) noexcept {
        if (not enabled_) return;
        const auto count = std::min(value.size(), AsyncLogger::record_size - 1 - size_);
        if (count != 0) std::memcpy(data_.data() + size_, value.data(), count);
        size_ += count;
        truncated_ = truncated_ or count != value.size();
    }
    std::array<char, AsyncLogger::record_size> data_;
    std::size_t size_ = 0;
    int saved_errno_;
    bool enabled_, truncated_ = false;
};

} // namespace tuntom
