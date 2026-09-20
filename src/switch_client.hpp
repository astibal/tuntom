#pragma once

#include "ipc/switch_transport.hpp"
#include <chrono>
#include <poll.h>
#include <sys/un.h>

namespace tuntom {

class SwitchClient {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr auto connect_timeout = std::chrono::seconds(5);

    SwitchClient(const std::string &path, const std::string &port_id, ipc::Options options = {})
        : path_(path), registration_(encode_switch_registration(port_id)), options_(options) {
        if (path.size() >= sizeof(sockaddr_un::sun_path)) throw std::runtime_error("Switch socket path is too long");
        if (!ipc::valid_frame_limit(options.frame_limit) || !options.slots || options.slots > ipc::max_slots ||
            !options.batch || options.batch > ipc::max_batch) throw std::runtime_error("Invalid switch IPC limits");
    }
    ~SwitchClient() { disconnect(); }
    SwitchClient(const SwitchClient &) = delete;
    SwitchClient &operator=(const SwitchClient &) = delete;

    void start_connect(Time now) {
        disconnect(); last_error_ = 0;
        connect_deadline_ = now + connect_timeout;
        probe_deadline_ = now + std::chrono::seconds(1);
        legacy_attempt_ = options_.mode == ipc::Mode::legacy;
        open_socket();
    }
    void advance_connect(Time now, short revents) {
        if (!connecting()) return;
        if (now >= connect_deadline_) { fail(ETIMEDOUT); return; }
        if (!legacy_attempt_ && (state_ == State::hello || state_ == State::welcome) && now >= probe_deadline_) {
            fallback(now); return;
        }
        if (revents & POLLNVAL) { fail(EBADF); return; }
        if (state_ == State::connecting) {
            if (!(revents & (POLLOUT | POLLERR | POLLHUP))) return;
            int error = 0; socklen_t size = sizeof(error);
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
                if (errno != EINTR) fail(errno);
                return;
            }
            if (error || (revents & (POLLERR | POLLHUP))) { fail(error ? error : ECONNRESET); return; }
            state_ = legacy_attempt_ ? State::registration : State::hello;
        }
        if (state_ == State::welcome || state_ == State::setup || state_ == State::active) {
            if (revents & (POLLIN | POLLERR | POLLHUP)) read_setup(now);
        } else if (revents & (POLLERR | POLLHUP)) fail(ECONNRESET);
        else if (revents & POLLOUT) write_setup();
    }
    void disconnect() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1; state_ = State::disconnected; transport_.reset();
    }
    int fd() const { return fd_; }
    bool connected() const { return state_ == State::connected; }
    bool connecting() const { return state_ != State::disconnected && state_ != State::connected; }
    bool receive_pending() const { return connected() && transport_.receive_pending(); }
    short poll_events() const {
        if (connected()) return static_cast<short>(POLLIN | (transport_.retry_writable(Clock::now()) ? POLLOUT : 0));
        return state_ == State::connecting || state_ == State::hello || state_ == State::registration ||
            state_ == State::ready ? POLLOUT : POLLIN;
    }
    int poll_timeout_ms(Time now, int maximum) const {
        if (receive_pending()) return 0;
        if (!connecting()) return transport_.retry_timeout(now, maximum);
        const auto deadline = (state_ == State::hello || state_ == State::welcome) ? std::min(connect_deadline_, probe_deadline_) : connect_deadline_;
        if (now >= deadline) return 0;
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
        return static_cast<int>(std::min<std::int64_t>(maximum, left));
    }
    int last_error() const { return last_error_; }
    ssize_t receive(std::uint8_t *buffer, std::size_t size) {
        if (!connected()) { errno = ENOTCONN; return -1; }
        return transport_.receive(fd_, buffer, size);
    }
    ssize_t send(const std::uint8_t *buffer, std::size_t size) {
        if (!connected()) { errno = ENOTCONN; return -1; }
        return transport_.send(fd_, {buffer, size});
    }
    ssize_t send_batch(const ipc::FrameParts *frames, std::size_t count) {
        if (!connected()) { errno = ENOTCONN; return -1; }
        return transport_.send_batch(fd_, frames, count);
    }
    ssize_t send_frame(SwitchOpcode opcode, const std::uint64_t *labels, std::size_t label_count,
                       const std::uint8_t *payload, std::size_t payload_size) {
        if (!connected()) { errno = ENOTCONN; return -1; }
        SwitchFrameHeader header;
        const auto n = encode_switch_header(header, opcode, labels, label_count, payload_size);
        return transport_.send(fd_, {header.data(), n, payload, payload_size});
    }
    using Outcome = ipc::Transport::Outcome;
    Outcome append_frame(SwitchOpcode opcode, const std::uint64_t *labels, std::size_t label_count,
                         const std::uint8_t *payload, std::size_t payload_size) {
        if (!connected()) { Outcome out; out.drops = 1; out.error = ENOTCONN; return out; }
        SwitchFrameHeader header;
        const auto n = encode_switch_header(header, opcode, labels, label_count, payload_size);
        return transport_.append(fd_, {header.data(), n, payload, payload_size});
    }
    Outcome flush() { return transport_.flush(fd_); }
    Outcome discard_staged() { return transport_.discard_staged(); }
    void write_stats(std::ostream &out, const std::string &prefix = "switch_ipc_") const {
        transport_.write_stats(out, prefix);
        out << prefix << "legacy_fallbacks=" << fallbacks_ << '\n';
    }
    const ipc::Transport &transport() const { return transport_; }
private:
    enum class State { disconnected, connecting, hello, welcome, registration, setup, ready, active, connected };
    void fail(int error) { last_error_ = error; disconnect(); }
    void fallback(Time now) {
        // The unnamed probe is the only retry inside this fixed connection budget.
        // A positive but malformed response, or failure after registration, never
        // downgrades. No published data is replayed.
        if (options_.mode != ipc::Mode::automatic || legacy_attempt_ || now >= connect_deadline_) {
            fail(EPROTONOSUPPORT); return;
        }
        disconnect(); legacy_attempt_ = true; ++fallbacks_; open_socket();
    }
    void open_socket() {
        fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd_ < 0) { last_error_ = errno; return; }
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
        if (::connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
            if (errno == EINPROGRESS) state_ = State::connecting;
            else fail(errno); // AF_UNIX EAGAIN has not queued a connection.
            return;
        }
        state_ = legacy_attempt_ ? State::registration : State::hello;
        write_setup();
    }
    void write_setup() {
        ipc::Record record;
        const std::uint8_t *data = nullptr; std::size_t size = 0;
        State next = State::disconnected;
        if (state_ == State::registration) {
            data = registration_.data(); size = registration_.size();
            next = legacy_attempt_ ? State::connected : State::setup;
        } else {
            if (state_ == State::hello) {
                auto request = options_;
                if (request.mode == ipc::Mode::automatic && !ipc::supported_abi()) request.mode = ipc::Mode::inline_only;
                record = ipc::hello(request); next = State::welcome;
            }
            else if (state_ == State::ready) {
                record = ipc::state(ipc::Type::ready, transport_.parameters(), map_declined_ ? 1 : 0);
                next = State::active;
            } else return;
            data = record.bytes.data(); size = record.size;
        }
        const auto sent = ::send(fd_, data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent == static_cast<ssize_t>(size)) state_ = next;
        else if (sent >= 0 || !ipc::retry_error()) fail(sent < 0 ? errno : EIO);
    }
    void read_setup(Time now) {
        std::array<std::uint8_t, ipc::max_record> buffer{};
        auto record = ipc::receive_record(fd_, buffer.data(), buffer.size());
        if (record.size < 0 && ipc::retry_error()) return;
        if (state_ == State::welcome && record.size == 0 && !record.count && record.valid) { fallback(now); return; }
        if (record.size <= 0 || !record.valid || (record.flags & MSG_TRUNC)) {
            fail(record.size < 0 ? errno : EPROTO); return;
        }
        const auto n = static_cast<std::size_t>(record.size);
        if (state_ != State::setup && record.count) { fail(EPROTO); return; }
        if (state_ == State::welcome) {
            if (ipc::is_header(buffer.data(), n, ipc::Type::error, 16) &&
                load_be32(buffer.data() + 8) == 1 && !load_be32(buffer.data() + 12)) { fallback(now); return; }
            ipc::Parameters offered;
            if (!ipc::decode_welcome(buffer.data(), n, offered) ||
                offered.frame_limit > options_.frame_limit || offered.slots > options_.slots || offered.batch > options_.batch ||
                ((options_.mode == ipc::Mode::inline_only || !ipc::supported_abi()) && offered.caps)) { fail(EPROTO); return; }
            offered_ = offered; state_ = State::registration;
        } else if (state_ == State::setup) {
            ipc::Parameters selected;
            if (!ipc::decode_setup(buffer.data(), n, offered_, selected) ||
                record.count != (selected.caps ? 2U : 0U)) { fail(EPROTO); return; }
            map_declined_ = false;
            if (selected.caps) {
                struct stat a{}, b{};
                if (::fstat(record.files[0].get(), &a) < 0 || ::fstat(record.files[1].get(), &b) < 0 ||
                    (a.st_dev == b.st_dev && a.st_ino == b.st_ino)) { fail(EPROTO); return; }
                try {
                    // Validate BOTH descriptors before treating an mmap failure
                    // as resource fallback. A malformed second FD is not hidden
                    // by ENOMEM while mapping the first one.
                    ipc::Mapping::validate_file(record.files[0].get(), selected, 1);
                    ipc::Mapping::validate_file(record.files[1].get(), selected, 2);
                    auto output = ipc::Mapping::attach(std::move(record.files[0]), selected, 1);
                    auto input = ipc::Mapping::attach(std::move(record.files[1]), selected, 2);
                    transport_.configure(selected, std::move(input), std::move(output));
                } catch (const ipc::MappingError &error) {
                    if (error.invalid) { fail(EPROTO); return; }
                    selected.inline_only(); transport_.configure(selected); map_declined_ = true;
                }
            } else transport_.configure(selected);
            state_ = State::ready;
        } else {
            std::uint32_t caps = 0, status = 0;
            if (!ipc::decode_state(buffer.data(), n, ipc::Type::active, transport_.parameters(), caps, status) ||
                status || caps != transport_.parameters().caps) { fail(EPROTO); return; }
            state_ = State::connected;
        }
    }
    int fd_ = -1, last_error_ = 0;
    State state_ = State::disconnected;
    Time connect_deadline_{}, probe_deadline_{};
    std::string path_;
    std::vector<std::uint8_t> registration_;
    ipc::Options options_;
    ipc::Parameters offered_;
    ipc::Transport transport_;
    bool legacy_attempt_ = false, map_declined_ = false;
    std::uint64_t fallbacks_ = 0;
};
} // namespace tuntom
