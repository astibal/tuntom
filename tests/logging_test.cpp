#include "../src/common.hpp"
#include "../src/packet.hpp"
#include <cassert>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <new>
#include <sstream>
#include <sys/wait.h>

namespace {
enum Fault { healthy, blocked, io_error, interrupted, would_block, no_space, zero_write, short_write };
std::atomic<int> fault {healthy}, calls {0}, entered {0};
bool fail_create = false;
unsigned creates = 0;
thread_local bool deny_allocation = false;

template<class Check> void until(Check check) {
    const auto deadline = tuntom::AsyncLogger::Clock::now() + std::chrono::seconds(4);
    while (not check()) {
        assert(tuntom::AsyncLogger::Clock::now() < deadline);
        ::usleep(10000);
    }
}

std::uint64_t metric(const char* key) {
    std::ostringstream out;
    tuntom::logger.write_stats(out);
    const auto text = out.str();
    const auto position = text.find(std::string(key) + "=");
    assert(position != std::string::npos);
    return std::stoull(text.substr(position + std::strlen(key) + 1));
}

int pipe_sink(bool close_reader = false) {
    int pipe[2];
    assert(::pipe2(pipe, O_CLOEXEC) == 0);
    assert(::dup2(pipe[1], STDERR_FILENO) == STDERR_FILENO);
    ::close(pipe[1]);
    if (close_reader) ::close(pipe[0]);
    else assert(::fcntl(pipe[0], F_SETFL, O_NONBLOCK) == 0);
    return pipe[0];
}

std::string read_line(int fd) {
    std::string result;
    until([&] {
        char buffer[4096];
        const auto count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0) result.append(buffer, static_cast<std::size_t>(count));
        return result.find('\n') != std::string::npos;
    });
    return result;
}

void formatting() {
    const int reader = pipe_sink();
    tuntom::logger.start();
    errno = EACCES;
    deny_allocation = true;
    tuntom::log_info("number=", std::numeric_limits<std::int64_t>::min(), " ",
                      std::numeric_limits<std::uint64_t>::max(), " bool=", true);
    deny_allocation = false;
    assert(errno == EACCES);
    assert(read_line(reader) == "number=-9223372036854775808 18446744073709551615 bool=1\n");
    const std::string long_message(3000, 'x');
    deny_allocation = true;
    tuntom::log_info(long_message);
    deny_allocation = false;
    const auto truncated = read_line(reader);
    assert(truncated.size() == tuntom::AsyncLogger::record_size);
    assert(truncated.substr(truncated.size() - 13) == " [truncated]\n");
    assert(metric("log_truncated") == 1);
    // Exercise the bounded C-string path as well as the string_view path.
    deny_allocation = true;
    tuntom::log_info(long_message.c_str());
    deny_allocation = false;
    assert(read_line(reader) == truncated);
    assert(metric("log_truncated") == 2);
    tuntom::log_level = tuntom::LogLevel::debug;
    const std::uint8_t packet[] {0, 0x0f, 0x80, 0xff};
    deny_allocation = true;
    tuntom::dump_bytes("packet", packet, sizeof(packet));
    deny_allocation = false;
    assert(read_line(reader) == "packet 4 bytes: 00 0f 80 ff\n");
}

void broken_pipe() {
    pipe_sink(true);
    tuntom::logger.start();
    tuntom::log_info("closed reader");
    until([] { return metric("log_write_errors") == 1; });
    assert(metric("log_last_errno") == EPIPE);
    assert(::kill(::getpid(), SIGPIPE) == 0);
    for (unsigned i = 0; i < 10000; ++i) tuntom::log_info("still running");
    ::usleep(1200000);
    assert(metric("log_write_errors") <= 3);
    assert(metric("log_dropped") > 0);
}

void full_queue() {
    const int reader = pipe_sink();
    fault = blocked;
    tuntom::logger.start();
    tuntom::log_info("stalled");
    until([] { return entered.load() != 0; });
    auto now = tuntom::AsyncLogger::Clock::now() + std::chrono::hours(1);
    for (unsigned i = 0; i < tuntom::AsyncLogger::capacity; ++i) {
        now += std::chrono::seconds(1);
        tuntom::logger.submit("queued\n", 7, false, now);
    }
    assert(metric("log_dropped") == 1); // blocked record still owns its slot
    const auto began = tuntom::AsyncLogger::Clock::now();
    deny_allocation = true;
    for (unsigned i = 0; i < 10000; ++i) tuntom::log_info("no wait ", i);
    deny_allocation = false;
    assert(tuntom::AsyncLogger::Clock::now() - began < std::chrono::seconds(1));
    assert(metric("log_rate_limited") > 0);
    assert(metric("log_dropped") == 10001);
    fault = healthy;
    assert(read_line(reader).find("stalled\n") != std::string::npos);
    // Verify fresh publication after the old queue has been consumed.
    until([] { return calls.load() == 64; });
    tuntom::logger.submit("recovered\n", 10, false, now + std::chrono::seconds(1));
    std::string output;
    until([&] {
        char buffer[4096];
        const auto count = ::read(reader, buffer, sizeof(buffer));
        if (count > 0) output.append(buffer, static_cast<std::size_t>(count));
        return output.find("recovered\n") != std::string::npos;
    });
}

void creation_failure() {
    const int reader = pipe_sink();
    (void)reader;
    fail_create = true;
    tuntom::logger.start();
    tuntom::logger.start();
    assert(creates == 1);
    assert(not tuntom::logger.started());
    assert(metric("log_start_errors") == 1);
    deny_allocation = true;
    tuntom::log_info("discarded");
    tuntom::log_fatal("no synchronous fallback");
    deny_allocation = false;
    assert(metric("log_dropped") == 2);
    assert(calls.load() == 0);
}

void missing_stderr() {
    ::close(STDERR_FILENO);
    tuntom::logger.ignore_sigpipe();
    // Simulate a packet endpoint acquiring descriptor 2 after initialization.
    assert(::open("/dev/null", O_WRONLY | O_CLOEXEC) == STDERR_FILENO);
    tuntom::log_fatal("must not write to the reused descriptor");
    tuntom::logger.start();
    tuntom::log_info("discarded");
    assert(not tuntom::logger.started());
    assert(metric("log_start_errors") == 1);
    assert(metric("log_dropped") == 2);
    assert(calls.load() == 0);
}

void queue_wraparound() {
    const int reader = pipe_sink();
    tuntom::logger.start();
    auto now = tuntom::AsyncLogger::Clock::now();
    for (unsigned batch = 0; batch < 16; ++batch) {
        std::string expected, output;
        for (unsigned i = 0; i < 32; ++i) {
            const auto message = "ordered " + std::to_string(batch * 32 + i) + "\n";
            expected += message;
            now += std::chrono::milliseconds(50);
            tuntom::logger.submit(message.data(), message.size(), false, now);
        }
        until([&] {
            char buffer[4096];
            const auto count = ::read(reader, buffer, sizeof(buffer));
            if (count > 0) output.append(buffer, static_cast<std::size_t>(count));
            return output.size() >= expected.size();
        });
        assert(output == expected);
    }
    assert(metric("log_dropped") == 0);
}

void output_failure(int kind) {
    const int reader = pipe_sink();
    fault = kind;
    tuntom::logger.start();
    tuntom::log_info("failed");
    until([] { return metric("log_write_errors") == 1; });
    assert(calls.load() <= 4);
    fault = healthy;
    tuntom::log_info("recovered");
    assert(read_line(reader).find("recovered\n") != std::string::npos);
    assert(metric("log_dropped") == 1);
}

void file_limit() {
    FILE* file = ::tmpfile();
    assert(file);
    const int fd = ::fileno(file);
    assert(::dup2(fd, STDERR_FILENO) == STDERR_FILENO);
    assert(::ftruncate(fd, tuntom::AsyncLogger::file_limit - 2) == 0);
    tuntom::logger.start();
    tuntom::log_info("too large");
    until([] { return metric("log_file_limit_drops") == 1; });
    struct stat status {};
    assert(::fstat(fd, &status) == 0);
    assert(status.st_size == tuntom::AsyncLogger::file_limit - 2);
    assert(::ftruncate(fd, 0) == 0);
    tuntom::log_info("after rotation");
    until([&] { return ::fstat(fd, &status) == 0 and status.st_size > 0; });
    assert(status.st_size == 15); // no hole at the former 16 MiB offset
    char buffer[32] {};
    assert(::pread(fd, buffer, sizeof(buffer), 0) == 15);
    assert(std::string(buffer, 15) == "after rotation\n");
}

void exit_stalled() {
    pipe_sink();
    fault = blocked;
    tuntom::logger.start();
    tuntom::log_info("forever");
    until([] { return entered.load() != 0; });
    // Return through ordinary exit/static destruction with the writer blocked.
}
} // namespace

[[gnu::noinline]] void* operator new(std::size_t size) {
    if (deny_allocation) throw std::bad_alloc();
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
[[gnu::noinline]] void operator delete(void* p) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete(void* p, std::size_t) noexcept { std::free(p); }

extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                                       void* (*entry)(void*), void* context) {
    ++creates;
    std::size_t size = 0;
    int detached = 0;
    assert(::pthread_attr_getstacksize(attr, &size) == 0 and size == 65536);
    assert(::pthread_attr_getdetachstate(attr, &detached) == 0 and detached == PTHREAD_CREATE_DETACHED);
    sigset_t mask;
    assert(::pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0);
    assert(::sigismember(&mask, SIGUSR1) == 1);
    if (fail_create) return EAGAIN;
    return __real_pthread_create(thread, attr, entry, context);
}

extern "C" ssize_t __real_write(int, const void*, std::size_t);
extern "C" ssize_t __wrap_write(int fd, const void* data, std::size_t size) {
    if (fd != STDERR_FILENO) return __real_write(fd, data, size);
    ++calls;
    ++entered;
    while (fault.load() == blocked) ::usleep(10000);
    if (fault.load() == io_error) { errno = EIO; return -1; }
    if (fault.load() == interrupted) { errno = EINTR; return -1; }
    if (fault.load() == would_block) { errno = EAGAIN; return -1; }
    if (fault.load() == no_space) { errno = ENOSPC; return -1; }
    if (fault.load() == zero_write) return 0;
    if (fault.load() == short_write) size = 1;
    return __real_write(fd, data, size);
}

int main() {
    const auto run = [](const char* name, auto test) {
        const pid_t child = ::fork();
        assert(child >= 0);
        if (child == 0) {
            ::alarm(8);
            tuntom::logger.ignore_sigpipe();
            test();
            std::exit(0);
        }
        int status = 0;
        assert(::waitpid(child, &status, 0) == child);
        if (not WIFEXITED(status) or WEXITSTATUS(status) != 0) {
            std::cout << "FAIL: " << name << " status=" << status << std::endl;
            std::exit(1);
        }
        std::cout << "PASS: " << name << std::endl;
    };
    run("bounded allocation-free formatting", formatting);
    run("SIGPIPE and error backoff", broken_pipe);
    run("bounded queue, flood and stalled writer recovery", full_queue);
    run("ordered records across repeated queue wraparound", queue_wraparound);
    run("thread creation failure", creation_failure);
    run("closed inherited stderr must not become a packet endpoint", missing_stderr);
    run("EIO recovery", [] { output_failure(io_error); });
    run("EINTR recovery", [] { output_failure(interrupted); });
    run("EAGAIN recovery", [] { output_failure(would_block); });
    run("ENOSPC recovery", [] { output_failure(no_space); });
    run("zero write recovery", [] { output_failure(zero_write); });
    run("partial write bound and recovery", [] { output_failure(short_write); });
    run("file growth bound and copytruncate recovery", file_limit);
    run("exit without joining stalled writer", exit_stalled);
}
