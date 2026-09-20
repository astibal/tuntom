// Saturated codec/queue experiment. No sockets, session state, or production keys.
#include "../../src/protocol.hpp"
#include <atomic>
#include <chrono>
#include <exception>
#include <iomanip>
#include <memory>
#include <sched.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <time.h>

using Clock = std::chrono::steady_clock;
using tuntom::Packet;
using tuntom::ProtocolV5;

static double cpu_time(clockid_t clock) {
    timespec ts {};
    if (clock_gettime(clock, &ts)) throw std::runtime_error("clock_gettime failed");
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}
static void relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}
static void pin(int cpu) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask))
        throw std::runtime_error("cannot pin to CPU " + std::to_string(cpu));
}

struct Options {
    std::string mode = "mixed";
    std::size_t size = 1400, workers = 0, batch = 1, slots = 256;
    std::size_t packets = 100000, warmup = 4096, delay_us = 0;
    std::vector<int> cpus;
    bool verify = false, tamper = false;
};
static Options options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--verify") { o.verify = true; continue; }
        if (arg == "--tamper") { o.tamper = true; continue; }
        if (++i == argc) throw std::runtime_error("missing value for " + arg);
        const std::string value = argv[i];
        if (arg == "--mode") o.mode = value;
        else if (arg == "--cpus") {
            std::istringstream input(value);
            std::string item;
            while (std::getline(input, item, ',')) o.cpus.push_back(std::stoi(item));
        } else {
            if (value.empty() || value.front() == '-') throw std::runtime_error("invalid number");
            std::size_t end = 0;
            const auto n = std::stoull(value, &end);
            if (end != value.size()) throw std::runtime_error("invalid number");
            if (arg == "--size") o.size = n;
            else if (arg == "--workers") o.workers = n;
            else if (arg == "--batch") o.batch = n;
            else if (arg == "--slots") o.slots = n;
            else if (arg == "--packets") o.packets = n;
            else if (arg == "--warmup") o.warmup = n;
            else if (arg == "--delay-us") o.delay_us = n;
            else throw std::runtime_error("unknown option " + arg);
        }
    }
    if (o.mode != "tx" && o.mode != "rx" && o.mode != "mixed" && o.mode != "copy")
        throw std::runtime_error("mode must be tx, rx, mixed, or copy");
    if (!o.size || o.size > 65535 || !o.packets || o.packets > 1000000000 ||
        o.warmup > 1000000 || o.workers > 64 || !o.batch || o.batch > 64 ||
        !o.slots || o.slots > 1024 || o.slots % o.batch ||
        o.slots / o.batch < o.workers || o.delay_us > 100000 ||
        o.cpus.size() != o.workers + 1)
        throw std::runtime_error("invalid bounds; provide one CPU for main plus one per worker");
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed)) throw std::runtime_error("get affinity");
    for (std::size_t i = 0; i < o.cpus.size(); ++i) {
        const int cpu = o.cpus[i];
        if (cpu < 0 || cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, &allowed) ||
            std::find(o.cpus.begin(), o.cpus.begin() + i, cpu) != o.cpus.begin() + i)
            throw std::runtime_error("CPUs must be allowed and distinct");
    }
    if (o.tamper && o.mode != "rx" && o.mode != "mixed")
        throw std::runtime_error("tamper requires rx or mixed");
    return o;
}

// Exactly one producer/consumer per ring; payload ownership travels with pointers.
template<class T> class Ring {
    alignas(64) std::atomic<std::size_t> write_ {0};
    alignas(64) std::atomic<std::size_t> read_ {0};
    std::vector<T*> data_;
public:
    explicit Ring(std::size_t capacity) : data_(capacity + 1) {}
    bool push(T* item) {
        const auto w = write_.load(std::memory_order_relaxed);
        const auto next = (w + 1) % data_.size();
        if (next == read_.load(std::memory_order_acquire)) return false;
        data_[w] = item;
        write_.store(next, std::memory_order_release);
        return true;
    }
    T* pop() {
        const auto r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return nullptr;
        auto* item = data_[r];
        read_.store((r + 1) % data_.size(), std::memory_order_release);
        return item;
    }
};

struct Job {
    std::vector<std::uint8_t> plain, incoming, wire;
    Packet packet;
    std::uint64_t ticket = 0, sequence = 0, rx_sequence = 0;
    bool rx = false, corrupt = false, ok = false, sampled = false;
    Clock::time_point started;
};
struct Batch {
    std::vector<Job> jobs;
    std::size_t ticket = 0, count = 0;
    bool ready = false; // Only the coordinator touches this flag.
};
struct Worker {
    Ring<Batch> input, output;
    std::atomic<bool> ready {false}, failed {false};
    std::exception_ptr error;
    std::thread thread;
    explicit Worker(std::size_t count) : input(count), output(count) {}
};
struct Result {
    std::uint64_t tx = 0, rx = 0, rejected = 0, checksum = 0, reordered = 0;
    std::size_t max_inflight = 0;
    double seconds = 0, cpu = 0, main_cpu = 0;
    std::vector<double> latency_us;
};

class Experiment {
    const Options o_;
    // Public synthetic keys; separate directions, never a user's tunnel material.
    tuntom::ascon::key_type tx_key_ {}, rx_key_ {};
    std::unique_ptr<ProtocolV5> codec_, peer_;
    std::vector<std::unique_ptr<Batch>> batches_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<std::uint8_t> scratch_, verify_scratch_;
    Packet verified_;
    std::atomic<bool> stop_ {false};
    std::uint64_t next_sequence_ = 1;

    void process(Job& job, std::vector<std::uint8_t>& scratch) const {
        if (o_.mode == "copy") {
            job.wire.assign(job.packet.payload.begin(), job.packet.payload.end());
            job.ok = true;
        } else if (job.rx) {
            job.ok = codec_->decode_with_scratch(job.wire.data(), job.wire.size(), job.packet, scratch);
        } else {
            codec_->encode_into(job.packet, job.wire, scratch);
            job.ok = true;
        }
    }
    void prepare(Job& job, std::uint64_t ticket) {
        job.ticket = ticket;
        job.rx = o_.mode == "rx" || (o_.mode == "mixed" && ticket % 2);
        job.sampled = ticket % 256 == 0;
        // Samples alternate directions in mixed mode instead of always sampling TX.
        if (o_.mode == "mixed") job.sampled = ticket % 257 == 0;
        if (job.sampled) job.started = Clock::now();
        if (job.rx) {
            job.wire.assign(job.incoming.begin(), job.incoming.end());
            job.sequence = job.rx_sequence;
        } else {
            job.packet.payload.assign(job.plain.begin(), job.plain.end());
            job.packet.type = tuntom::PacketType::data;
            job.packet.tunnel_id = 42;
            job.packet.fragment_offset = 0;
            job.packet.original_length = static_cast<std::uint32_t>(o_.size);
            job.packet.message_id = 0;
            job.sequence = job.packet.sequence = next_sequence_++;
        }
    }
    void consume(Job& job, std::uint64_t expected, Result& r) {
        if (job.ticket != expected) throw std::runtime_error("completion order broken");
        if (job.sampled)
            r.latency_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - job.started).count());
        if (job.rx) ++r.rx; else ++r.tx;
        const bool should_pass = !(job.rx && job.corrupt);
        if (job.ok != should_pass) throw std::runtime_error("unexpected authentication result");
        if (!job.ok) { ++r.rejected; return; }
        const auto& output = job.rx ? job.packet.payload : job.wire;
        const auto expected_size = o_.size + (!job.rx && o_.mode != "copy" ? 25 : 0);
        if (output.size() != expected_size || (job.rx && job.packet.sequence != job.sequence))
            throw std::runtime_error("incorrect output size or sequence");
        r.checksum += output[job.ticket % output.size()];
        if (!o_.verify) return;
        if (job.rx || o_.mode == "copy") {
            if (output != job.plain) throw std::runtime_error("payload mismatch");
        } else {
            if (!peer_->decode_with_scratch(output.data(), output.size(), verified_, verify_scratch_) ||
                verified_.sequence != job.sequence || verified_.payload != job.plain)
                throw std::runtime_error("TX verification failed");
        }
    }
    void check_workers() const {
        for (const auto& worker : workers_)
            if (worker->failed.load(std::memory_order_acquire)) std::rethrow_exception(worker->error);
    }
    void shutdown() noexcept {
        stop_.store(true, std::memory_order_release);
        for (auto& worker : workers_) if (worker->thread.joinable()) worker->thread.join();
    }
public:
    explicit Experiment(const Options& o) : o_(o) {
        for (std::size_t i = 0; i < tx_key_.size(); ++i) {
            tx_key_[i] = static_cast<std::uint8_t>(i + 1);
            rx_key_[i] = static_cast<std::uint8_t>(i + 101);
        }
        codec_ = std::make_unique<ProtocolV5>(42, tx_key_, rx_key_, true);
        peer_ = std::make_unique<ProtocolV5>(42, rx_key_, tx_key_, true);
        scratch_.reserve(o.size + 64);
        verify_scratch_.reserve(o.size + 64);
        verified_.payload.reserve(o.size + 64);
        const auto count = o.slots / o.batch;
        for (std::size_t b = 0; b < count; ++b) {
            auto batch = std::make_unique<Batch>();
            batch->jobs.resize(o.batch);
            for (std::size_t j = 0; j < o.batch; ++j) {
                auto& job = batch->jobs[j];
                const auto id = b * o.batch + j;
                job.plain.resize(o.size);
                for (std::size_t p = 0; p < o.size; ++p)
                    job.plain[p] = static_cast<std::uint8_t>((p * 131 + id * 17 + (p >> 8)) & 255);
                job.packet.payload = job.plain;
                job.packet.payload.reserve(o.size + 64);
                job.packet.original_length = static_cast<std::uint32_t>(o.size);
                job.packet.sequence = job.rx_sequence = id + 1;
                peer_->encode_into(job.packet, job.incoming, scratch_);
                job.corrupt = o.tamper && id % 17 == 0;
                if (job.corrupt) job.incoming[9] ^= 1; // First byte of the DATA tag.
                job.wire.reserve(o.size + 64);
            }
            batches_.push_back(std::move(batch));
        }
        for (std::size_t i = 0; i < o.workers; ++i)
            workers_.push_back(std::make_unique<Worker>(count));
        try {
            for (std::size_t i = 0; i < o.workers; ++i) {
                Worker* worker = workers_[i].get();
                worker->thread = std::thread([this, worker, i] {
                    try {
                        pin(o_.cpus[i + 1]);
                        std::vector<std::uint8_t> scratch;
                        scratch.reserve(o_.size + 64);
                        worker->ready.store(true, std::memory_order_release);
                        while (!stop_.load(std::memory_order_acquire)) {
                            Batch* batch = worker->input.pop();
                            if (!batch) { relax(); continue; }
                            if (o_.delay_us && batch->ticket % 17 == 0)
                                std::this_thread::sleep_for(std::chrono::microseconds(o_.delay_us));
                            for (std::size_t j = 0; j < batch->count; ++j) process(batch->jobs[j], scratch);
                            // Ring holds every batch in the system; a full ring indicates an ownership bug.
                            if (!worker->output.push(batch)) throw std::runtime_error("completion ring overflow");
                        }
                    } catch (...) {
                        worker->error = std::current_exception();
                        worker->failed.store(true, std::memory_order_release);
                        worker->ready.store(true, std::memory_order_release);
                    }
                });
            }
            for (const auto& worker : workers_)
                while (!worker->ready.load(std::memory_order_acquire)) relax();
            check_workers();
        } catch (...) { shutdown(); throw; }
    }
    ~Experiment() { shutdown(); }

    Result run(std::size_t packets) {
        Result r;
        r.latency_us.reserve(packets / 256 + 2);
        const double cpu_start = cpu_time(CLOCK_PROCESS_CPUTIME_ID);
        const double main_start = cpu_time(CLOCK_THREAD_CPUTIME_ID);
        const auto started = Clock::now();
        if (workers_.empty()) {
            for (std::size_t n = 0; n < packets; ++n) {
                const auto slot = n % o_.slots;
                auto& job = batches_[slot / o_.batch]->jobs[slot % o_.batch];
                prepare(job, n);
                process(job, scratch_);
                consume(job, n, r);
            }
            r.max_inflight = packets ? 1 : 0;
        } else {
            std::size_t submitted = 0, committed = 0, sent_batches = 0, committed_batches = 0;
            std::size_t highest_observed = 0;
            while (committed < packets) {
                check_workers();
                while (submitted < packets && sent_batches - committed_batches < batches_.size()) {
                    auto& batch = *batches_[sent_batches % batches_.size()];
                    batch.ready = false;
                    batch.ticket = sent_batches;
                    batch.count = std::min(o_.batch, packets - submitted);
                    for (std::size_t j = 0; j < batch.count; ++j) prepare(batch.jobs[j], submitted + j);
                    auto& worker = *workers_[sent_batches % workers_.size()];
                    if (!worker.input.push(&batch)) throw std::runtime_error("input ring overflow");
                    submitted += batch.count;
                    ++sent_batches;
                }
                r.max_inflight = std::max(r.max_inflight, submitted - committed);
                for (auto& worker : workers_) {
                    while (auto* batch = worker->output.pop()) {
                        if (batch->ready) throw std::runtime_error("duplicate completion");
                        batch->ready = true;
                        if (batch->ticket < highest_observed) ++r.reordered;
                        highest_observed = std::max(highest_observed, batch->ticket);
                    }
                }
                while (committed_batches < sent_batches) {
                    auto& batch = *batches_[committed_batches % batches_.size()];
                    if (!batch.ready) break;
                    if (batch.ticket != committed_batches) throw std::runtime_error("batch order broken");
                    for (std::size_t j = 0; j < batch.count; ++j) consume(batch.jobs[j], committed++, r);
                    ++committed_batches;
                }
                relax();
            }
        }
        r.seconds = std::chrono::duration<double>(Clock::now() - started).count();
        r.main_cpu = cpu_time(CLOCK_THREAD_CPUTIME_ID) - main_start;
        r.cpu = cpu_time(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
        return r;
    }
};

int main(int argc, char** argv) {
    try {
        const auto o = options(argc, argv);
        pin(o.cpus.front());
        tuntom::log_level = tuntom::LogLevel::quiet;
        Experiment experiment(o);
        experiment.run(o.warmup);
        auto r = experiment.run(o.packets);
        std::sort(r.latency_us.begin(), r.latency_us.end());
        const auto percentile = [&](double p) {
            return r.latency_us.empty() ? 0.0 : r.latency_us[static_cast<std::size_t>(p * (r.latency_us.size() - 1))];
        };
        std::cout << std::setprecision(10)
            << "{\"mode\":\"" << o.mode << "\",\"size\":" << o.size
            << ",\"workers\":" << o.workers << ",\"batch\":" << o.batch
            << ",\"slots\":" << o.slots << ",\"packets\":" << o.packets
            << ",\"warmup\":" << o.warmup << ",\"verify\":" << (o.verify ? "true" : "false")
            << ",\"tamper\":" << (o.tamper ? "true" : "false") << ",\"delay_us\":" << o.delay_us
            << ",\"seconds\":" << r.seconds << ",\"pps\":" << o.packets / r.seconds
            << ",\"payload_gbps\":" << o.packets * double(o.size) * 8e-9 / r.seconds
            << ",\"cpu_seconds\":" << r.cpu << ",\"main_cpu_seconds\":" << r.main_cpu
            << ",\"cpu_cores\":" << r.cpu / r.seconds << ",\"cpu_ns_per_packet\":" << r.cpu * 1e9 / o.packets
            << ",\"p50_us\":" << percentile(.50) << ",\"p99_us\":" << percentile(.99)
            << ",\"latency_samples\":" << r.latency_us.size() << ",\"max_inflight\":" << r.max_inflight
            << ",\"tx\":" << r.tx << ",\"rx\":" << r.rx << ",\"rejected\":" << r.rejected
            << ",\"out_of_order_observed\":" << r.reordered << ",\"checksum\":" << r.checksum << "}\n";
    } catch (const std::exception& error) {
        std::cerr << "ascon-workers: " << error.what() << "\n";
        return 1;
    }
}
