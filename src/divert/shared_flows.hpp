#pragma once

#include "flows.hpp"
#include "../via/adapter.hpp"
#include "../vendor/siphash.hpp"
#include <algorithm>
#include <atomic>
#include <fcntl.h>
#include <memory>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tuntom::divert {
struct SharedFlowsTest;
namespace shared {
inline void check(int error, const char* operation) {
    if (error) throw std::runtime_error(std::string(operation) + ": " + std::strerror(error));
}
inline std::size_t aligned(std::size_t n) { return (n + 63) & ~std::size_t(63); }
inline std::int64_t ticks(Clock::time_point now) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}
inline std::uint64_t hash(const FlowKey& key, const linux_siphash::siphash_key_t& seed) {
    std::uint8_t data[38]{key.version, key.protocol};
    std::copy(key.source.begin(), key.source.end(), data + 2);
    std::copy(key.destination.begin(), key.destination.end(), data + 18);
    data[34] = static_cast<std::uint8_t>(key.source_port >> 8); data[35] = static_cast<std::uint8_t>(key.source_port);
    data[36] = static_cast<std::uint8_t>(key.destination_port >> 8); data[37] = static_cast<std::uint8_t>(key.destination_port);
    return linux_siphash::siphash(data, sizeof(data), &seed);
}
struct Route {
    FlowKey forward;
    via::Envelope client, server;
    std::int64_t touched = 0;
    unsigned kind() const { return 0; }
};
struct AdmissionValue {
    unsigned value = 0; // Existing TCP, diverted TCP, existing UDP.
    unsigned kind() const { return value; }
};

// Fixed pools and integer links only: no process-local pointers in mapped memory.
// Each operation holds one robust, process-shared shard mutex. An undo record
// protects the single modified node; indexes can be rebuilt after owner death.
template<class Value> class Table {
    friend struct tuntom::divert::SharedFlowsTest;
    struct Node { FlowKey key; Value value; std::uint32_t next = 0; bool present = false; };
    struct alignas(64) Shard {
        pthread_mutex_t mutex;
        std::uint32_t dirty = 0, free = 0, sweep = 0, counts[3]{};
        std::uint64_t expired = 0, recovered = 0;
        Node undo;
    };
    std::uint8_t* memory_;
    std::size_t capacity_, classes_, shards_ = 1, slots_, buckets_ = 1, stride_;
    linux_siphash::siphash_key_t seed_;
    std::size_t quota(std::size_t shard) const { return capacity_ / shards_ + (shard < capacity_ % shards_); }
    Shard& state(std::size_t shard) { return *reinterpret_cast<Shard*>(memory_ + shard * stride_); }
    std::uint32_t* heads(Shard& s) { return reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(&s) + sizeof(Shard)); }
    Node* nodes(Shard& s) { return reinterpret_cast<Node*>(reinterpret_cast<std::uint8_t*>(&s) + aligned(sizeof(Shard) + buckets_ * sizeof(std::uint32_t))); }
    std::size_t bucket(const FlowKey& key) const { return (hash(key, seed_) / shards_) % buckets_; }
    void rebuild(std::size_t shard) {
        auto& s = state(shard); auto* n = nodes(s); auto* h = heads(s);
        std::fill_n(h, buckets_, 0); std::fill_n(s.counts, 3, 0); s.free = 0;
        for (std::size_t i = quota(shard) * classes_; i; --i) {
            auto& node = n[i - 1];
            if (node.present) {
                const auto b = bucket(node.key); node.next = h[b]; h[b] = static_cast<std::uint32_t>(i);
                ++s.counts[node.value.kind()];
            } else { node.next = s.free; s.free = static_cast<std::uint32_t>(i); }
        }
    }
public:
    Table(void* memory, std::size_t capacity, std::size_t classes, const linux_siphash::siphash_key_t& seed)
        : memory_(static_cast<std::uint8_t*>(memory)), capacity_(capacity), classes_(classes), seed_(seed) {
        while (shards_ < 64 && shards_ * 2 <= capacity) shards_ *= 2;
        slots_ = ((capacity + shards_ - 1) / shards_) * classes;
        while (buckets_ < slots_ * 2) buckets_ *= 2;
        stride_ = aligned(aligned(sizeof(Shard) + buckets_ * sizeof(std::uint32_t)) + slots_ * sizeof(Node));
    }
    std::size_t bytes() const { return shards_ * stride_; }
    std::size_t shards() const { return shards_; }
    void initialize() {
        pthread_mutexattr_t attr;
        check(::pthread_mutexattr_init(&attr), "shared mutex attributes");
        try {
            check(::pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED), "process-shared mutex");
            check(::pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST), "robust mutex");
            for (std::size_t i = 0; i < shards_; ++i) {
                auto& s = *new (memory_ + i * stride_) Shard{};
                check(::pthread_mutex_init(&s.mutex, &attr), "shared mutex initialization");
                for (std::size_t j = 0; j < slots_; ++j) new (nodes(s) + j) Node{};
                rebuild(i);
            }
        } catch (...) { ::pthread_mutexattr_destroy(&attr); throw; }
        ::pthread_mutexattr_destroy(&attr);
    }
    class Locked {
        friend struct tuntom::divert::SharedFlowsTest;
        Table& table_; std::size_t shard_; Shard& state_; Node* nodes_;
        void begin(std::uint32_t index) {
            state_.undo = nodes_[index - 1];
            __atomic_store_n(&state_.dirty, index, __ATOMIC_RELEASE);
            __atomic_thread_fence(__ATOMIC_SEQ_CST); // Publish undo before changing any node bytes.
        }
        void commit() { __atomic_store_n(&state_.dirty, 0, __ATOMIC_RELEASE); }
    public:
        Locked(Table& table, std::size_t shard) : table_(table), shard_(shard), state_(table.state(shard)), nodes_(table.nodes(state_)) {
            const auto error = ::pthread_mutex_lock(&state_.mutex);
            if (error == EOWNERDEAD) {
                if (state_.dirty) nodes_[state_.dirty - 1] = state_.undo;
                table_.rebuild(shard_); commit(); ++state_.recovered;
                const auto recovery = ::pthread_mutex_consistent(&state_.mutex);
                if (recovery) { ::pthread_mutex_unlock(&state_.mutex); check(recovery, "shared mutex recovery"); }
            } else check(error, "shared flow lock");
        }
        Locked(const Locked&) = delete;
        ~Locked() { ::pthread_mutex_unlock(&state_.mutex); }
        const Value& value(std::uint32_t index) const { return nodes_[index - 1].value; }
        std::uint32_t find(const FlowKey& key) const {
            auto index = table_.heads(state_)[table_.bucket(key)];
            while (index) {
                if (nodes_[index - 1].key == key) return index;
                index = nodes_[index - 1].next;
            }
            return 0;
        }
        std::uint32_t insert(const FlowKey& key, const Value& value) {
            if (state_.counts[value.kind()] >= table_.quota(shard_) || !state_.free) return 0;
            const auto index = state_.free;
            const auto b = table_.bucket(key);
            begin(index); state_.free = nodes_[index - 1].next;
            nodes_[index - 1] = Node{key, value, table_.heads(state_)[b], true};
            table_.heads(state_)[b] = index; ++state_.counts[value.kind()]; commit(); return index;
        }
        void update(std::uint32_t index, const Value& value) {
            begin(index); nodes_[index - 1].value = value; commit();
        }
        void touch(std::uint32_t index, std::int64_t now) {
            __atomic_store_n(&nodes_[index - 1].value.touched, now, __ATOMIC_RELAXED);
        }
        void erase(std::uint32_t index) {
            auto& node = nodes_[index - 1]; begin(index);
            auto* link = &table_.heads(state_)[table_.bucket(node.key)];
            while (*link != index) link = &nodes_[*link - 1].next;
            *link = node.next; --state_.counts[node.value.kind()];
            node.present = false; node.next = state_.free; state_.free = index;
            commit(); ++state_.expired;
        }
        void expire(std::int64_t now, std::int64_t idle, unsigned budget = 64) {
            const auto count = table_.quota(shard_) * table_.classes_;
            for (unsigned i = 0; i < budget && i < count; ++i) {
                state_.sweep = static_cast<std::uint32_t>((state_.sweep + 1) % count);
                const auto& node = nodes_[state_.sweep];
                if (node.present && now - node.value.touched >= idle) erase(state_.sweep + 1);
            }
        }
        template<class Visitor> void visit(Visitor visitor) const {
            for (std::size_t i = 0; i < table_.quota(shard_) * table_.classes_; ++i)
                if (nodes_[i].present) visitor(nodes_[i].key, nodes_[i].value);
        }
        void counters(std::uint64_t (&counts)[3], std::uint64_t& expired, std::uint64_t& recovered) const {
            for (unsigned i = 0; i < 3; ++i) counts[i] += state_.counts[i];
            expired += state_.expired; recovered += state_.recovered;
        }
    };
    Locked lock(const FlowKey& key) { return Locked(*this, hash(key, seed_) % shards_); }
    Locked lock_shard(std::size_t index) { return Locked(*this, index); }
    void counters(std::uint64_t (&counts)[3], std::uint64_t& expired, std::uint64_t& recovered) {
        for (std::size_t i = 0; i < shards_; ++i) lock_shard(i).counters(counts, expired, recovered);
    }
};
} // namespace shared

// Linux-local file-backed MAP_SHARED state. OFD byte locks protect initialization
// and membership, and disappear even when a worker is killed. The last worker
// leaving permits the next opener to start a fresh group without stale contexts.
class SharedFlows {
    friend struct SharedFlowsTest;
    static constexpr std::uint64_t magic = 0x5454464c4f570001ULL;
    struct alignas(64) Header {
        std::uint64_t signature = magic, ready = 0, bytes = 0, capacity = 0, admission_capacity = 0;
        std::int64_t idle = 0, started = 0;
        bool warmup = false;
        linux_siphash::siphash_key_t seed{};
        char group[512]{}, workers[16][64]{};
    };
    int fd_ = -1;
    void* memory_ = MAP_FAILED;
    std::size_t bytes_ = 0, sweep_ = 0;
    Header* header_ = nullptr;
    Clock::time_point next_maintenance_{};
    std::unique_ptr<shared::Table<shared::Route>> routes_;
    std::unique_ptr<shared::Table<shared::AdmissionValue>> admission_;
    static struct flock range(short type, off_t offset) {
        struct flock lock{}; lock.l_type = type; lock.l_whence = SEEK_SET; lock.l_start = offset; lock.l_len = 1; return lock;
    }
    void lock_byte(off_t offset, short type, bool wait = true) {
        auto lock = range(type, offset); int result;
        do { result = ::fcntl(fd_, wait ? F_OFD_SETLKW : F_OFD_SETLK, &lock); } while (result < 0 && errno == EINTR);
        if (result < 0) shared::check(errno, "shared flow file lock");
    }
    bool occupied(unsigned slot) {
        auto lock = range(F_WRLCK, slot + 1);
        if (::fcntl(fd_, F_OFD_GETLK, &lock)) shared::check(errno, "shared worker lock query");
        return lock.l_type != F_UNLCK;
    }
    void close() {
        routes_.reset(); admission_.reset();
        if (memory_ != MAP_FAILED) ::munmap(memory_, bytes_);
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1; memory_ = MAP_FAILED;
    }
public:
    static std::string group(const std::vector<std::string>& fields) {
        struct stat ns{};
        if (::stat("/proc/self/ns/net", &ns)) shared::check(errno, "network namespace identity");
        std::string result = std::to_string(ns.st_dev) + ":" + std::to_string(ns.st_ino);
        for (const auto& field : fields) result += "|" + std::to_string(field.size()) + ":" + field;
        return result;
    }
    SharedFlows(const std::string& path, const std::string& group, const std::string& worker,
                std::size_t capacity, std::chrono::seconds idle, std::size_t admission_capacity, bool warmup) {
        if (group.size() >= sizeof(Header::group) || worker.empty() || worker.size() >= sizeof(Header::workers[0]) ||
            !capacity || capacity > 100000000 || !admission_capacity || admission_capacity > 100000000 || idle.count() <= 0)
            throw std::runtime_error("invalid shared flow configuration");
        try {
            fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd_ < 0) shared::check(errno, "open shared flow file");
            lock_byte(0, F_WRLCK);
            struct stat info{};
            if (::fstat(fd_, &info)) shared::check(errno, "shared flow file stat");
            if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0022))
                throw std::runtime_error("shared flow file must be a regular owned file without group/other write access");
            bool live = false; int slot = -1;
            for (unsigned i = 0; i < 16; ++i) {
                if (occupied(i)) live = true;
                else if (slot < 0) slot = static_cast<int>(i);
            }
            if (slot < 0) throw std::runtime_error("shared flow group already has 16 workers");
            Header old{};
            if (info.st_size) {
                const auto n = ::pread(fd_, &old, sizeof(old), 0);
                if (n < static_cast<ssize_t>(sizeof(old.signature)) || old.signature != magic)
                    throw std::runtime_error("unrecognized shared flow file; refusing to replace it");
                if (live && n != sizeof(old)) throw std::runtime_error("incomplete live shared flow header");
            } else if (live) throw std::runtime_error("empty live shared flow file");
            const auto idle_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(idle).count();
            const auto route_bytes = shared::Table<shared::Route>(nullptr, capacity, 1, old.seed).bytes();
            bytes_ = sizeof(Header) + route_bytes;
            if (warmup) bytes_ += shared::Table<shared::AdmissionValue>(nullptr, admission_capacity, 3, old.seed).bytes();
            if (live) {
                if (old.ready != magic || old.bytes != bytes_ || info.st_size != static_cast<off_t>(bytes_) ||
                    old.capacity != capacity || old.admission_capacity != admission_capacity || old.idle != idle_ns ||
                    old.warmup != warmup || std::string(old.group, sizeof(old.group)) != group + std::string(sizeof(old.group) - group.size(), '\0'))
                    throw std::runtime_error("shared flow group configuration or ABI mismatch");
                for (unsigned i = 0; i < 16; ++i)
                    if (occupied(i) && std::string(old.workers[i], sizeof(old.workers[i])) == worker + std::string(sizeof(old.workers[i]) - worker.size(), '\0'))
                        throw std::runtime_error("shared flow worker ID is already active");
            } else {
                // Leave an identifiable header if initialization is interrupted.
                if (::pwrite(fd_, &magic, sizeof(magic), 0) != sizeof(magic)) shared::check(errno ? errno : EIO, "shared flow signature");
                if (::ftruncate(fd_, static_cast<off_t>(bytes_))) shared::check(errno, "shared flow resize");
                shared::check(::posix_fallocate(fd_, 0, static_cast<off_t>(bytes_)), "shared flow allocation");
            }
            memory_ = ::mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
            if (memory_ == MAP_FAILED) shared::check(errno, "shared flow mmap");
            header_ = static_cast<Header*>(memory_);
            if (!live) {
                new (header_) Header{};
                header_->bytes = bytes_; header_->capacity = capacity; header_->admission_capacity = admission_capacity;
                header_->idle = idle_ns; header_->warmup = warmup;
                std::copy(group.begin(), group.end(), header_->group);
                ssize_t n;
                do { n = ::getrandom(&header_->seed, sizeof(header_->seed), 0); } while (n < 0 && errno == EINTR);
                if (n != sizeof(header_->seed)) shared::check(errno ? errno : EIO, "shared flow hash seed");
            }
            auto* data = static_cast<std::uint8_t*>(memory_) + sizeof(Header);
            routes_ = std::make_unique<shared::Table<shared::Route>>(data, capacity, 1, header_->seed);
            if (warmup) admission_ = std::make_unique<shared::Table<shared::AdmissionValue>>(data + route_bytes, admission_capacity, 3, header_->seed);
            if (!live) {
                routes_->initialize(); if (admission_) admission_->initialize();
                header_->ready = magic;
            }
            lock_byte(slot + 1, F_WRLCK, false);
            std::fill_n(header_->workers[slot], sizeof(header_->workers[slot]), '\0');
            std::copy(worker.begin(), worker.end(), header_->workers[slot]);
            lock_byte(0, F_UNLCK);
        } catch (...) { close(); throw; }
    }
    SharedFlows(const SharedFlows&) = delete;
    ~SharedFlows() { close(); }
    bool active() const { return __atomic_load_n(&header_->started, __ATOMIC_ACQUIRE) != 0; }
    double seconds(Clock::time_point now) const {
        const auto start = __atomic_load_n(&header_->started, __ATOMIC_ACQUIRE);
        return start ? static_cast<double>(shared::ticks(now) - start) / 1e9 : 0.0;
    }
    AdmissionResult classify(const PacketInfo& packet, Clock::time_point now) {
        std::int64_t expected = 0;
        __atomic_compare_exchange_n(&header_->started, &expected, shared::ticks(now), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        const auto age = seconds(now);
        if (!admission_ || (packet.flow.protocol == 6 ? age >= 86400 : age >= 3600)) return AdmissionResult::proxy;
        const auto key = canonical(packet.flow); auto lock = admission_->lock(key);
        const auto index = lock.find(key);
        if (packet.flow.protocol == 6) {
            if (index) return lock.value(index).value == 0 ? AdmissionResult::bypass : AdmissionResult::proxy;
            if (age >= 3600) return AdmissionResult::proxy;
            const bool proxy = (packet.flags & 0x12) == 0x02;
            if (!lock.insert(key, shared::AdmissionValue{proxy ? 1U : 0U})) return AdmissionResult::full;
            return proxy ? AdmissionResult::proxy : AdmissionResult::bypass;
        }
        if (index) return AdmissionResult::bypass;
        if (age >= 60) return AdmissionResult::proxy;
        return lock.insert(key, shared::AdmissionValue{2}) ? AdmissionResult::bypass : AdmissionResult::full;
    }
    Learn learn(const FlowKey& flow, const via::AdapterContext& context, bool from_client, Clock::time_point now) {
        const auto& env = std::get<via::Envelope>(context.value);
        const auto key = canonical(flow); const auto time = shared::ticks(now); auto lock = routes_->lock(key);
        auto index = lock.find(key);
        if (index && time - lock.value(index).touched >= header_->idle) { lock.erase(index); index = 0; }
        if (!index) {
            shared::Route value{from_client ? flow : reverse_key(flow), env, env, time};
            if (lock.insert(key, value)) return Learn::ok;
            lock.expire(time, header_->idle);
            return lock.insert(key, value) ? Learn::ok : Learn::full;
        }
        const auto& current = lock.value(index);
        if (!current.client.same_context(env) || !(flow == (from_client ? current.forward : reverse_key(current.forward)))) return Learn::conflict;
        const auto& previous = from_client ? current.client : current.server;
        if (previous.base == env.base && previous.reverse == env.reverse && previous.action == env.action && previous.present == env.present) {
            lock.touch(index, time); return Learn::ok;
        }
        auto value = current;
        (from_client ? value.client : value.server) = env; value.touched = time;
        lock.update(index, value); return Learn::ok;
    }
    bool lookup(const FlowKey& flow, bool to_client, Clock::time_point now, via::AdapterContext& context) {
        const auto time = shared::ticks(now); const auto key = canonical(flow); auto lock = routes_->lock(key);
        const auto index = lock.find(key);
        if (!index) return false;
        const auto& value = lock.value(index);
        if (time - value.touched >= header_->idle || !(flow == (to_client ? reverse_key(value.forward) : value.forward))) return false;
        context.value = to_client ? value.server : value.client; lock.touch(index, time); return true;
    }
    void maintain(Clock::time_point now) {
        if (now < next_maintenance_) return;
        next_maintenance_ = now + std::chrono::milliseconds(100);
        routes_->lock_shard(sweep_++ % routes_->shards()).expire(shared::ticks(now), header_->idle);
    }
    void dump_flows(FlowDump& dump, Clock::time_point now) {
        // Shards are individually consistent, not a global atomic snapshot.
        // Copy before formatting so other workers hold off only for the copy.
        for (std::size_t i = 0; i < routes_->shards(); ++i) {
            std::vector<shared::Route> values;
            {
                auto lock = routes_->lock_shard(i);
                lock.visit([&](const auto&, const auto& value) {
                    if (shared::ticks(now) - value.touched < header_->idle) values.push_back(value);
                });
            }
            for (const auto& value : values) dump.add("shared_routes", value.forward, [&](auto& out) {
                FlowDump::idle(out, std::chrono::nanoseconds(std::max<std::int64_t>(0, shared::ticks(now) - value.touched)));
                value.client.write_flow(out, "client");
                value.server.write_flow(out, "server");
            });
        }
        const auto age = seconds(now);
        if (admission_ && age < 86400) {
            for (std::size_t i = 0; i < admission_->shards(); ++i) {
                std::vector<std::pair<FlowKey, unsigned>> values;
                {
                    auto lock = admission_->lock_shard(i);
                    lock.visit([&](const auto& key, const auto& value) {
                        if (value.value == 0 || age < 3600) values.emplace_back(key, value.value);
                    });
                }
                for (const auto& value : values) {
                    const char* names[] = {"existing_tcp", "diverted_tcp", "existing_udp"};
                    dump.add(names[value.second], value.first, [](auto& out) { out << " labels=unknown"; });
                }
            }
        }
    }
    void stats(std::ostream& out) {
        std::uint64_t counts[3]{}, expired = 0, recovered = 0;
        routes_->counters(counts, expired, recovered);
        out << "shared_flows=1\nshared_flow_bytes=" << bytes_ << "\nshared_flow_shards=" << routes_->shards()
            << "\nflow_entries=" << counts[0] << "\nflow_expirations=" << expired << '\n';
        std::fill_n(counts, 3, 0);
        if (admission_) admission_->counters(counts, expired, recovered);
        const auto age = seconds(Clock::now());
        out << "existing_tcp=" << (age < 86400 ? counts[0] : 0) << "\ndiverted_tcp=" << (age < 3600 ? counts[1] : 0)
            << "\nexisting_udp=" << (age < 3600 ? counts[2] : 0) << "\nshared_lock_recoveries=" << recovered << '\n';
    }
};
} // namespace tuntom::divert
