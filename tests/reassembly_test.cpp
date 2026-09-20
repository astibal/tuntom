// Deterministic overload and loss tests. No sockets, real-time sleeps or root.
#include "../src/reassembly.hpp"
#include <iostream>
#include <limits>
#include <sstream>

using namespace tuntom;
using Time = Reassembler::Time;
using namespace std::chrono;
const auto start = Time{} + seconds(100);

static void require(bool ok, const char* why) {
    if (not ok) throw std::runtime_error(why);
}

static Packet fragment(std::uint64_t id, std::uint32_t offset = 0,
                       std::uint32_t total = 8, std::size_t size = 2) {
    Packet packet;
    packet.type = PacketType::data;
    packet.message_id = id;
    packet.original_length = total;
    packet.fragment_offset = offset;
    packet.payload.resize(size);
    for (std::size_t i = 0; i < size; ++i)
        packet.payload[i] = static_cast<std::uint8_t>(offset + i);
    return packet;
}

static void test_oldest_and_late_fragments() {
    Reassembler pool(100, nullptr, {2, 200, 32});
    std::vector<std::uint8_t> out;
    // Equal timestamps still have deterministic FIFO admission order. Updating
    // ID 1 changes its expiry order, but not its capacity-eviction order.
    pool.accept(fragment(1), out, nullptr, start);
    pool.accept(fragment(2), out, nullptr, start);
    pool.accept(fragment(1, 2), out, nullptr, start + milliseconds(1));
    pool.accept(fragment(3), out, nullptr, start + milliseconds(2));
    require(pool.metrics().capacity_evictions == 1, "pool did not evict oldest");
    const auto bytes = pool.metrics().active_bytes;
    for (auto offset : {0u, 2u, 4u, 6u}) {
        require(not pool.accept(fragment(1, offset), out, nullptr,
                                start + milliseconds(3)), "discarded ID completed");
    }
    require(pool.metrics().late_fragment_drops == 4, "late fragments not counted");
    require(pool.metrics().capacity_evictions == 1 &&
            pool.metrics().active_entries == 2 && pool.metrics().active_bytes == bytes,
            "late fragments churned live pool");
    for (auto offset : {6u, 4u, 2u}) {
        const bool done = pool.accept(fragment(2, offset), out, nullptr,
                                      start + milliseconds(4));
        require(done == (offset == 2), "recent message was evicted instead of oldest");
    }
    require(out == fragment(0, 0, 8, 8).payload, "bad reassembled bytes");
}

static void test_sustained_loss_without_blackout() {
    Reassembler pool(9000);
    std::vector<std::uint8_t> out;
    auto now = start;
    for (std::size_t id = 1; id <= max_reassembly_entries; ++id)
        pool.accept(fragment(id, 0, 9000, 1500), out, nullptr, now);
    require(pool.metrics().active_entries == max_reassembly_entries, "prefill failed");

    constexpr unsigned rounds = 5000;
    for (unsigned i = 0; i < rounds; ++i) {
        now += microseconds(10);
        // A permanently incomplete packet keeps applying capacity pressure.
        pool.accept(fragment(10000 + i, 0, 9000, 1500), out, nullptr, now);
        const auto evictions = pool.metrics().capacity_evictions;
        // Fragments of the first evicted message must not reopen it.
        pool.accept(fragment(1, 1500, 9000, 1500), out, nullptr, now);
        require(pool.metrics().capacity_evictions == evictions, "orphan caused eviction");

        // All six fragments of each healthy message arrive, in reverse order.
        // The old 64-entry reject-new policy blacked out until its 3s timeout.
        for (int part = 5; part >= 0; --part) {
            now += microseconds(1);
            const bool done = pool.accept(
                fragment(100000 + i, static_cast<std::uint32_t>(part * 1500), 9000, 1500),
                out, nullptr, now);
            require(done == (part == 0), "healthy traffic blacked out under pressure");
        }
        require(out == fragment(0, 0, 9000, 9000).payload, "flood corrupted data");
        require(pool.metrics().active_entries <= max_reassembly_entries &&
                pool.metrics().active_bytes <= max_reassembly_bytes, "pool exceeded limits");
    }
    require(now - start < Reassembler::timeout, "test accidentally waited for expiry");
    require(pool.metrics().expired_entries == 0, "unexpected expiration in flood");
    require(pool.metrics().completed_packets == rounds, "lost intact messages in flood");
    require(pool.metrics().capacity_evictions > 0, "test did not reach pressure");
    require(pool.metrics().late_fragment_drops == rounds, "orphan accounting failed");
    pool.cleanup_expired(now + Reassembler::timeout);
    require(pool.metrics().active_entries == 0 && pool.metrics().active_bytes == 0,
            "pool did not recover after traffic stopped");
}

static void test_byte_limit() {
    Reassembler pool(100, nullptr, {10, 20, 32});
    std::vector<std::uint8_t> out;
    pool.accept(fragment(1), out, nullptr, start);
    pool.accept(fragment(2), out, nullptr, start);
    // 16 reserved + 18 new requires two oldest evictions.
    pool.accept(fragment(3, 0, 18), out, nullptr, start);
    require(pool.metrics().capacity_evictions == 2 &&
            pool.metrics().active_entries == 1 && pool.metrics().active_bytes == 18,
            "byte pressure not enforced");
    pool.accept(fragment(4, 0, 21), out, nullptr, start);
    require(pool.metrics().capacity_drops == 1 &&
            pool.metrics().capacity_evictions == 2 && pool.metrics().active_bytes == 18,
            "oversized allocation evicted healthy entries");
    // Complete DATA bypasses both capacity and fragmented-ID tombstones.
    require(pool.accept(fragment(4, 0, 21, 21), out, nullptr, start),
            "whole DATA incorrectly subjected to reassembly admission");
    require(pool.metrics().active_bytes == 18, "whole DATA changed pool state");
}

static void test_expiry_and_tombstone_bounds() {
    Reassembler pool(100, nullptr, {2, 200, 2});
    std::vector<std::uint8_t> out;
    pool.accept(fragment(1), out, nullptr, start);
    pool.accept(fragment(2), out, nullptr, start + seconds(1));
    pool.accept(fragment(1, 2), out, nullptr, start + seconds(2));
    pool.cleanup_expired(start + seconds(4));
    require(pool.metrics().expired_entries == 1 &&
            pool.metrics().active_entries == 1, "activity expiry order broken");
    pool.accept(fragment(2, 2), out, nullptr, start + seconds(4));
    require(pool.metrics().late_fragment_drops == 1, "expired ID reopened");
    pool.cleanup_expired(start + seconds(5));
    require(pool.metrics().expired_entries == 2 && pool.metrics().active_bytes == 0,
            "expiry deadline not enforced");

    // Late fragments do not prolong tombstone retention.
    pool.accept(fragment(2, 4), out, nullptr, start + seconds(6));
    pool.cleanup_expired(start + seconds(7));
    require(pool.metrics().discarded_ids == 1, "late fragment refreshed tombstone");
    pool.cleanup_expired(start + seconds(8));
    require(pool.metrics().discarded_ids == 0, "discarded IDs not reclaimed");

    // Finite suppression cache remains bounded even under unique-ID floods.
    for (std::uint64_t id = 100; id < 120; ++id)
        pool.accept(fragment(id), out, nullptr, start + seconds(9));
    require(pool.metrics().discarded_ids == 2 &&
            pool.metrics().discarded_id_evictions == 16, "unbounded tombstone storage");
    // The latest evicted ID remains suppressed even when older records roll off.
    const auto evictions = pool.metrics().capacity_evictions;
    pool.accept(fragment(117, 2), out, nullptr, start + seconds(9));
    require(pool.metrics().capacity_evictions == evictions, "recent tombstone missing");
}

static void test_validation_and_metrics_lifetime() {
    ReassemblyMetrics shared;
    std::vector<std::uint8_t> out;
    {
        Reassembler a(100, &shared), b(100, &shared);
        a.accept(fragment(1), out, nullptr, start);
        b.accept(fragment(1), out, nullptr, start); // independent session namespace
        require(shared.active_entries == 2 && shared.active_bytes == 16, "shared gauges");
        a.accept(fragment(1), out, nullptr, start); // duplicate, no expiry refresh
        require(shared.overlap_drops == 1, "overlap not counted");
        a.accept(fragment(1, 2, 10), out, nullptr, start);
        require(shared.invalid_fragments == 1 && shared.active_entries == 1,
                "inconsistent metadata did not retire message");
        b.accept(fragment(9, std::numeric_limits<std::uint32_t>::max()),
                 out, nullptr, start);
        require(shared.invalid_fragments == 2 && shared.active_entries == 1,
                "invalid fragment altered capacity");
        // Invalid message ID must be suppressed, too.
        a.accept(fragment(1, 4), out, nullptr, start);
        require(shared.late_fragment_drops == 1, "invalid message resurrected");
    }
    require(shared.active_entries == 0 && shared.active_bytes == 0 &&
            shared.discarded_ids == 0, "retired sessions leaked gauges");
    require(shared.peak_entries == 2 && shared.peak_bytes == 16 &&
            shared.session_discarded_entries == 1, "lifetime accounting lost");
    std::ostringstream stats;
    shared.write(stats);
    require(stats.str().find("reassembly_capacity_evictions=0\n") != std::string::npos &&
            stats.str().find("reassembly_session_discarded_entries=1\n") != std::string::npos,
            "missing exported counters");

    Reassembler fragments(100);
    for (unsigned i = 0; i < 64; ++i)
        fragments.accept(fragment(99, i, 65, 1), out, nullptr, start);
    fragments.accept(fragment(99, 64, 65, 1), out, nullptr, start);
    require(fragments.metrics().invalid_fragments == 1 &&
            fragments.metrics().active_entries == 0, "fragment count bound broken");
}

int main() {
    log_level = LogLevel::quiet;
    test_oldest_and_late_fragments();
    test_sustained_loss_without_blackout();
    test_byte_limit();
    test_expiry_and_tombstone_bounds();
    test_validation_and_metrics_lifetime();
    std::cout << "PASS: sustained loss/pressure, FIFO eviction, reordered fragments, "
                 "late suppression, expiry, byte limits and metrics lifetime\n";
}
