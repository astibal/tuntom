#pragma once

#include "switch_v2.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace tuntom::ipc {
class File {
    int fd_ = -1;
public:
    explicit File(int fd = -1) : fd_(fd) {}
    ~File() { reset(); }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    File(File &&o) noexcept : fd_(o.release()) {}
    File &operator=(File &&o) noexcept { if (this != &o) { reset(); fd_ = o.release(); } return *this; }
    int get() const { return fd_; }
    int release() { return std::exchange(fd_, -1); }
    void reset() { if (fd_ >= 0) ::close(fd_); fd_ = -1; }
};
struct MappingError : std::runtime_error {
    bool invalid;
    MappingError(bool peer, const char *what) : std::runtime_error(what), invalid(peer) {}
};
inline bool supported_abi() {
#if defined(__linux__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __atomic_always_lock_free(sizeof(std::uint64_t), nullptr);
#else
    return false;
#endif
}
// No compiler structure/atomic layout goes on the wire. This ABI is restricted
// to aligned, lock-free native u64 atomic instructions on little-endian Linux.
class Mapping {
    File fd_;
    std::uint8_t *base_ = nullptr;
    Parameters geometry_{};
    std::uint32_t id_ = 0, cursor_ = 0;
    static std::array<std::uint8_t, 4096> pool_header(const Parameters &v, std::uint32_t id) {
        std::array<std::uint8_t, 4096> h{};
        std::memcpy(h.data(), "TTMMAP01", 8);
        const auto put = [&](std::size_t offset, const auto &value) {
            std::memcpy(h.data() + offset, &value, sizeof(value));
        };
        put(8, pool_abi); put(12, id); put(16, v.epoch); put(24, v.slots); put(28, v.capacity);
        put(32, v.stride); put(40, v.mapping_size);
        return h;
    }
    std::uint64_t *word(std::uint32_t slot) const {
        return reinterpret_cast<std::uint64_t *>(base_ + 4096 + slot * geometry_.stride);
    }
    void map() {
        void *p = ::mmap(nullptr, static_cast<std::size_t>(geometry_.mapping_size),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd_.get(), 0);
        if (p == MAP_FAILED) throw MappingError(false, "Cannot mmap switch pool");
        base_ = static_cast<std::uint8_t *>(p);
    }
public:
    Mapping() = default;
    ~Mapping() { reset(); }
    Mapping(const Mapping &) = delete;
    Mapping &operator=(const Mapping &) = delete;
    Mapping(Mapping &&o) noexcept { *this = std::move(o); }
    Mapping &operator=(Mapping &&o) noexcept {
        if (this != &o) {
            reset(); fd_ = std::move(o.fd_); base_ = std::exchange(o.base_, nullptr);
            geometry_ = o.geometry_; id_ = o.id_; cursor_ = o.cursor_;
        }
        return *this;
    }
    void reset() {
        if (base_) ::munmap(base_, static_cast<std::size_t>(geometry_.mapping_size));
        base_ = nullptr; fd_.reset();
    }
    explicit operator bool() const { return base_ != nullptr; }
    int fd() const { return fd_.get(); }
    void close_fd() { fd_.reset(); } // mmap retains the file; active ports need no extra FDs.
    std::uint32_t id() const { return id_; }
    std::uint64_t size() const { return base_ ? geometry_.mapping_size : 0; }
    std::uint8_t *data(std::uint32_t slot) const {
        return base_ + 4096 + slot * geometry_.stride + 64;
    }
    std::uint64_t token(std::uint32_t slot) const { return __atomic_load_n(word(slot), __ATOMIC_ACQUIRE); }
    void publish(const Ref &r) { __atomic_store_n(word(r.slot), r.token, __ATOMIC_RELEASE); }
    void release(const Ref &r) { __atomic_store_n(word(r.slot), r.token + 1, __ATOMIC_RELEASE); }
    bool reserve(std::uint32_t length, Ref &r) {
        for (std::uint32_t step = 0; step < geometry_.slots; ++step) {
            const auto slot = cursor_;
            cursor_ = (cursor_ + 1) % geometry_.slots;
            const auto generation = token(slot);
            if (generation & 1) continue;
            if (generation >= UINT64_MAX - 1) { errno = EOVERFLOW; return false; }
            r = {slot, length, generation + 1};
            return true; // Producer alone owns this direction. Publish before the next reserve.
        }
        errno = EAGAIN;
        return false;
    }
    static void validate_geometry(const Parameters &v) {
        const auto stride = (64ULL + v.capacity + 63) & ~63ULL;
        if (!v.epoch || v.abi != pool_abi || !v.slots || v.slots > max_slots ||
            !valid_frame_limit(v.capacity) || v.capacity > v.frame_limit ||
            v.stride != stride || v.mapping_size != 4096 + v.slots * stride)
            throw MappingError(true, "Invalid switch pool geometry");
    }
    static void validate_file(int fd, const Parameters &v, std::uint32_t id) {
        validate_geometry(v);
        struct stat st{};
        const auto seals = ::fcntl(fd, F_GET_SEALS);
        const auto access = ::fcntl(fd, F_GETFL);
        constexpr auto required = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;
        // 0x10 is F_SEAL_FUTURE_WRITE (also reject with older libc headers).
        if (access < 0 || (access & O_ACCMODE) != O_RDWR ||
            ::fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            static_cast<std::uint64_t>(st.st_size) != v.mapping_size || seals < 0 ||
            (seals & required) != required || (seals & (F_SEAL_WRITE | 0x10)))
            throw MappingError(true, "Invalid switch pool file/seals");
        std::array<std::uint8_t, 4096> h{};
        ssize_t got;
        do { got = ::pread(fd, h.data(), h.size(), 0); } while (got < 0 && errno == EINTR);
        if (got != static_cast<ssize_t>(h.size()) || h != pool_header(v, id))
            throw MappingError(true, "Invalid switch pool header");
    }
    static Mapping create(const Parameters &v, std::uint32_t id) {
        Mapping m;
        validate_geometry(v);
        if (!supported_abi()) throw MappingError(false, "Unsupported mmap atomic ABI");
        m.geometry_ = v; m.id_ = id;
        m.fd_ = File(::memfd_create("tuntom-ipc-v2", MFD_CLOEXEC | MFD_ALLOW_SEALING));
        if (m.fd_.get() < 0 || ::ftruncate(m.fd_.get(), static_cast<off_t>(v.mapping_size)) < 0)
            throw MappingError(false, "Cannot create switch pool");
        // Reserve backing storage before exposing a mapping, so tmpfs exhaustion
        // is a setup failure rather than a later SIGBUS in the packet path.
        if (::fallocate(m.fd_.get(), 0, 0, static_cast<off_t>(v.mapping_size)) < 0)
            throw MappingError(false, "Cannot reserve switch pool storage");
        if (::fcntl(m.fd_.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0)
            throw MappingError(false, "Cannot seal switch pool");
        m.map();
        const auto h = pool_header(v, id);
        std::memcpy(m.base_, h.data(), h.size());
        return m;
    }
    static Mapping attach(File fd, const Parameters &v, std::uint32_t id) {
        Mapping m;
        if (!supported_abi()) throw MappingError(false, "Unsupported mmap atomic ABI");
        m.fd_ = std::move(fd); m.geometry_ = v; m.id_ = id;
        validate_file(m.fd_.get(), v, id);
        m.map();
        const auto expected = pool_header(v, id);
        std::array<std::uint8_t, 4096> copy;
        std::memcpy(copy.data(), m.base_, copy.size());
        if (copy != expected) throw MappingError(true, "Invalid switch pool header");
        for (std::uint32_t i = 0; i < v.slots; ++i) {
            if (m.token(i)) throw MappingError(true, "Nonempty initial switch pool");
            for (unsigned j = 8; j < 64; ++j)
                if (m.base_[4096 + i * v.stride + j]) throw MappingError(true, "Invalid slot padding");
        }
        // All subsequent bounds use geometry_, never peer-writable header fields.
        m.close_fd();
        return m;
    }
};

// Fixed ancillary storage covers Linux's SCM_MAX_FD (253). Every installed FD
// is CLOEXEC and RAII-owned, including on malformed/truncated records.
struct Received {
    std::array<File, 256> files;
    std::size_t count = 0;
    ssize_t size = -1;
    int flags = 0;
    bool valid = true;
};
inline Received receive_record(int fd, void *buffer, std::size_t capacity) {
    Received result;
    alignas(cmsghdr) std::array<unsigned char, CMSG_SPACE(sizeof(int) * 256)> control{};
    iovec part{buffer, capacity};
    msghdr msg{}; msg.msg_iov = &part; msg.msg_iovlen = 1;
    msg.msg_control = control.data(); msg.msg_controllen = control.size();
    result.size = ::recvmsg(fd, &msg, MSG_DONTWAIT | MSG_TRUNC | MSG_CMSG_CLOEXEC);
    if (result.size < 0) return result;
    result.flags = msg.msg_flags;
    result.valid = !(msg.msg_flags & MSG_CTRUNC);
    for (auto *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS || c->cmsg_len < CMSG_LEN(0)) {
            result.valid = false; continue;
        }
        const auto bytes = c->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int)) result.valid = false;
        for (std::size_t i = 0; i < bytes / sizeof(int); ++i) {
            int incoming = -1;
            std::memcpy(&incoming, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            if (result.count < result.files.size()) result.files[result.count++] = File(incoming);
            else { ::close(incoming); result.valid = false; }
        }
    }
    return result;
}
inline ssize_t send_record(int fd, const Record &r, int first = -1, int second = -1) {
    if (first < 0) return ::send(fd, r.bytes.data(), r.size, MSG_DONTWAIT | MSG_NOSIGNAL);
    alignas(cmsghdr) std::array<unsigned char, CMSG_SPACE(sizeof(int) * 2)> control{};
    iovec part{const_cast<std::uint8_t *>(r.bytes.data()), r.size};
    msghdr msg{}; msg.msg_iov = &part; msg.msg_iovlen = 1;
    msg.msg_control = control.data(); msg.msg_controllen = control.size();
    auto *c = CMSG_FIRSTHDR(&msg); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * 2);
    const int fds[]{first, second}; std::memcpy(CMSG_DATA(c), fds, sizeof(fds));
    return ::sendmsg(fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
}
inline bool retry_error() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
} // namespace tuntom::ipc
