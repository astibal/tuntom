#include "queues.hpp"
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
using namespace switch_mt;

int main() {
  { // exact capacity, FIFO, wraparound, independently reused pointer slots
    Spsc<int> q(3);
    int a = 1, b = 2, c = 3, d = 4;
    assert(q.push(&a) && q.push(&b) && q.push(&c) && !q.push(&d));
    assert(q.pop() == &a && q.push(&d));
    assert(q.pop() == &b && q.pop() == &c && q.pop() == &d && !q.pop());
  }
  constexpr unsigned n = 3, count = 30000;
  std::array<std::unique_ptr<Pool>, n> pools;
  std::array<std::array<std::unique_ptr<Spsc<Buffer>>, n>, n> q;
  std::array<WakeGate, n> wake;
  for (unsigned i = 0; i < n; ++i) {
    pools[i].reset(new Pool(7));
    for (unsigned j = 0; j < n; ++j)
      q[i][j].reset(new Spsc<Buffer>(3));
  }
  std::vector<std::thread> tx, rx;
  for (unsigned dest = 0; dest < n; ++dest)
    tx.emplace_back([&, dest] {
      std::array<std::uint64_t, n> previous{};
      unsigned cursor = 0, received = 0;
      auto pop = [&]() {
        for (unsigned k = 0; k < n; ++k) {
          auto s = cursor;
          cursor = (cursor + 1) % n;
          if (auto *b = q[s][dest]->pop())
            return b;
        }
        return static_cast<Buffer *>(nullptr);
      };
      while (received < count) {
        auto *b = pop();
        if (!b) {
          wake[dest].arm();
          b = pop();
          if (!b) {
            wake[dest].wait();
            continue;
          }
          wake[dest].cancel();
        }
        std::uint64_t source, sequence;
        std::memcpy(&source, b->data, 8);
        std::memcpy(&sequence, b->data + 8, 8);
        assert(source < n && sequence % n == dest &&
               sequence > previous[source]);
        previous[source] = sequence;
        assert(b->size == 257 && b->used.load());
        for (unsigned k = 16; k < 257; ++k)
          assert(b->data[k] == std::uint8_t(sequence + source + k));
        if (sequence % 31 == 0)
          std::this_thread::yield();
        b->release();
        ++received;
      }
    });
  for (unsigned source = 0; source < n; ++source)
    rx.emplace_back([&, source] {
      std::uint64_t probes = 0;
      for (std::uint64_t sequence = 1; sequence <= count; ++sequence) {
        Buffer *b = nullptr;
        while (!(b = pools[source]->acquire(probes)))
          std::this_thread::yield();
        b->size = 257;
        std::uint64_t src = source;
        std::memcpy(b->data, &src, 8);
        std::memcpy(b->data + 8, &sequence, 8);
        for (unsigned k = 16; k < 257; ++k)
          b->data[k] = std::uint8_t(sequence + source + k);
        const auto dest = sequence % n;
        while (!q[source][dest]->push(b))
          std::this_thread::yield();
        wake[dest].notify();
      }
    });
  for (auto &t : rx)
    t.join();
  for (auto &t : tx)
    t.join();
  for (auto &p : pools)
    assert(p->in_use() == 0);
  std::cout << "PASS: capacity, wraparound, 90000 cross-thread pooled frames, "
               "payload, FIFO, wakeups, complete reclamation\n";
}
