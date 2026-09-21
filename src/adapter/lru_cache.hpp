#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>

namespace tuntom {

template<typename Key, typename Value, typename Hash>
class LruCache {
public:
    using Clock = std::chrono::steady_clock;

    LruCache(std::size_t capacity, Clock::duration idle_timeout)
        : capacity_(capacity), idle_timeout_(idle_timeout) {}

    void put(const Key& key, const Value& value, Clock::time_point now) {
        reclaim();
        auto found = entries_.find(key);
        if (found != entries_.end()) {
            const bool stale = found->second->generation != generation_;
            found->second->value = value;
            found->second->generation = generation_;
            if (stale) ++live_size_;
            found->second->last_seen = now;
            order_.splice(order_.begin(), order_, found->second);
            while (entries_.size() > capacity_) evict_last();
            return;
        }
        if (capacity_ == 0) return;
        order_.push_front({key, value, now, generation_});
        try {
            entries_.emplace(key, order_.begin());
        } catch (...) {
            // Keep the list and its index in sync if map allocation fails.
            order_.pop_front();
            throw;
        }
        ++live_size_;
        while (entries_.size() > capacity_) evict_last();
    }

    bool get(const Key& key, Value& value, Clock::time_point now) {
        auto found = entries_.find(key);
        if (found == entries_.end() || found->second->generation != generation_) return false;
        if (now - found->second->last_seen > idle_timeout_) {
            order_.erase(found->second);
            entries_.erase(found);
            --live_size_;
            ++expirations_;
            return false;
        }
        found->second->last_seen = now;
        order_.splice(order_.begin(), order_, found->second);
        value = found->second->value;
        return true;
    }

    // Does not refresh LRU order, timestamps, or hit/expiry counters.
    template<class Visitor> void visit_live(Clock::time_point now, Visitor visitor) const {
        for (const auto& entry : order_)
            if (entry.generation == generation_ && now - entry.last_seen <= idle_timeout_)
                visitor(entry.key, entry.value, entry.last_seen);
    }

    // Logical invalidation is constant time. Old entries are unreachable immediately.
    void flush() noexcept { ++generation_; live_size_ = 0; }
    void reclaim(std::size_t budget = 64) {
        while (budget-- && !order_.empty() && order_.back().generation != generation_) {
            entries_.erase(order_.back().key);
            order_.pop_back();
        }
    }
    std::size_t size() const { return live_size_; }
    std::uint64_t evictions() const { return evictions_; }
    std::uint64_t expirations() const { return expirations_; }

private:
    struct Entry {
        Key key;
        Value value;
        Clock::time_point last_seen;
        std::uint64_t generation;
    };

    void evict_last() {
        if (order_.back().generation == generation_) { --live_size_; ++evictions_; }
        entries_.erase(order_.back().key);
        order_.pop_back();
    }

    std::uint64_t generation_ = 0;
    std::size_t live_size_ = 0;
    std::size_t capacity_;
    Clock::duration idle_timeout_;
    std::list<Entry> order_;
    std::unordered_map<Key, typename std::list<Entry>::iterator, Hash> entries_;
    std::uint64_t evictions_ = 0;
    std::uint64_t expirations_ = 0;
};

} // namespace tuntom
