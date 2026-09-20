#pragma once

#include "info_message.hpp"
#include <array>
#include <cerrno>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <system_error>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace tuntom::info {

// One collector thread, asleep between confirmed sessions. The only handoff is
// a bounded ASCII buffer tagged with the requesting TX generation; no session,
// socket, peer state or logger is accessed by the collector.
class Worker {
public:
    using Collector = std::function<std::vector<std::uint8_t>()>;
    struct Result {
        std::array<std::uint8_t, max_payload> text {};
        std::size_t size = 0;
        std::uint64_t generation = 0;
        bool valid = false;
    };

    explicit Worker(Collector collect = [] { return encode_access(loopback_addresses()); })
        : collect_(std::move(collect)) {
        fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "INFO eventfd");
        pthread_attr_t attr;
        int error = ::pthread_attr_init(&attr);
        if (!error) {
            error = ::pthread_attr_setstacksize(&attr, 128 * 1024);
            sigset_t blocked, previous;
            ::sigfillset(&blocked);
            if (!error) error = ::pthread_sigmask(SIG_BLOCK, &blocked, &previous);
            if (!error) {
                error = ::pthread_create(&thread_, &attr, run, this);
                ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
            }
            ::pthread_attr_destroy(&attr);
        }
        if (error) {
            ::close(fd_);
            throw std::system_error(error, std::generic_category(), "INFO worker");
        }
    }

    ~Worker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        ::pthread_join(thread_, nullptr);
        ::close(fd_);
    }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    int fd() const { return fd_; }

    void request(std::uint64_t generation) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            generation_ = generation;
            pending_ = true;
            ready_ = false;
        }
        wake_.notify_one();
    }

    bool take(Result& result) {
        std::uint64_t count;
        while (::read(fd_, &count, sizeof(count)) < 0 && errno == EINTR) {}
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_) return false;
        result = result_;
        ready_ = false;
        return true;
    }

private:
    static void* run(void* context) {
        static_cast<Worker*>(context)->work();
        return nullptr;
    }

    void work() {
        for (;;) {
            std::uint64_t generation;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return stopping_ || pending_; });
                if (stopping_) return;
                generation = generation_;
                pending_ = false;
            }
            Result result;
            result.generation = generation;
            try {
                const auto payload = collect_();
                Fields fields;
                if (decode(payload, fields)) {
                    std::copy(payload.begin(), payload.end(), result.text.begin());
                    result.size = payload.size();
                    result.valid = true;
                }
            } catch (...) {
                // Publish failure without reusing a stale/partial snapshot.
                // Reporting belongs to the event loop (logger has one producer).
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) return;
                if (pending_ || generation != generation_) continue;
                result_ = result;
                ready_ = true;
            }
            const std::uint64_t one = 1;
            while (::write(fd_, &one, sizeof(one)) < 0 && errno == EINTR) {}
        }
    }

    Collector collect_;
    int fd_ = -1;
    pthread_t thread_ {};
    std::mutex mutex_;
    std::condition_variable wake_;
    bool pending_ = false, ready_ = false, stopping_ = false;
    std::uint64_t generation_ = 0;
    Result result_;
};
} // namespace tuntom::info
