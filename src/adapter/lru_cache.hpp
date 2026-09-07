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
        auto found = entries_.find(key);
        if (found != entries_.end()) {
            found->second->value = value;
            found->second->last_seen = now;
            order_.splice(order_.begin(), order_, found->second);
            return;
        }
        if (capacity_ == 0) return;
        order_.push_front({key, value, now});
        entries_[key] = order_.begin();
        while (entries_.size() > capacity_) evict_last();
    }

    bool get(const Key& key, Value& value, Clock::time_point now) {
        auto found = entries_.find(key);
        if (found == entries_.end()) return false;
        if (now - found->second->last_seen > idle_timeout_) {
            order_.erase(found->second);
            entries_.erase(found);
            ++expirations_;
            return false;
        }
        found->second->last_seen = now;
        order_.splice(order_.begin(), order_, found->second);
        value = found->second->value;
        return true;
    }

    std::size_t size() const { return entries_.size(); }
    std::uint64_t evictions() const { return evictions_; }
    std::uint64_t expirations() const { return expirations_; }

private:
    struct Entry {
        Key key;
        Value value;
        Clock::time_point last_seen;
    };

    void evict_last() {
        entries_.erase(order_.back().key);
        order_.pop_back();
        ++evictions_;
    }

    std::size_t capacity_;
    Clock::duration idle_timeout_;
    std::list<Entry> order_;
    std::unordered_map<Key, typename std::list<Entry>::iterator, Hash> entries_;
    std::uint64_t evictions_ = 0;
    std::uint64_t expirations_ = 0;
};

} // namespace tuntom
