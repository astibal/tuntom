#include "../src/switch_mp/config.hpp"
#include "../src/switch_mp/queues.hpp"
#include "../src/switch_mp/scheduler.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {
void require(bool condition) {
    if (!condition) {
        std::cerr << "MP invariant failed\n";
        std::abort();
    }
}

void scheduling() {
    using namespace tuntom::mp;
    for (std::size_t budget = 1; budget <= 16; ++budget) {
        for (std::size_t count = 0; count <= 64; ++count) {
            std::vector<Kind> ports;
            for (std::size_t i = 0; i < count; ++i)
                ports.push_back(i % 7 == 0    ? Kind::adapter
                                : i % 11 == 0 ? Kind::trunk
                                              : Kind::tunnel);
            const auto plan = schedule(ports, budget);
            require(plan.active <= budget && plan.active >= 1);
            require(plan.owners.size() == count && plan.worker_roles.size() == budget);
            std::array<std::set<std::size_t>, 4> role_workers;
            for (std::size_t i = 0; i < count; ++i) {
                for (std::size_t direction = 0; direction < 2; ++direction) {
                    const auto role = (ports[i] == Kind::tunnel ? 0U : 2U) + direction;
                    const auto owner = plan.owners[i][direction];
                    require(owner < plan.active && (plan.worker_roles[owner] & (1U << role)));
                    role_workers[role].insert(owner);
                }
            }
            for (std::size_t role = 0; role < 4; ++role) {
                if (!role_workers[role].empty())
                    require(role_workers[role].size() == plan.shards[role]);
            }
        }
    }
    std::vector<Kind> tunnels(8, Kind::tunnel);
    require(schedule(tunnels, 8).active == 2);
    tunnels.push_back(Kind::tunnel);
    auto expanded = schedule(tunnels, 8);
    require(expanded.active == 4 && expanded.shards[0] == 2 && expanded.shards[1] == 2);
    tunnels.pop_back();
    require(schedule(tunnels, 8).active == 2);
    tunnels.insert(tunnels.end(), 4, Kind::adapter);
    require(schedule(tunnels, 8).active == 4); // All four adapters share RXa/TXa.
    tunnels.push_back(Kind::adapter);
    require(schedule(tunnels, 8).active == 6);
    require(schedule(tunnels, 1).worker_roles[0] == 15);
    Policy policy;
    policy.tx_weight = 2;
    auto asymmetric = schedule(std::vector<Kind>(6, Kind::tunnel), 8, policy);
    require(asymmetric.shards[0] == 1 && asymmetric.shards[1] == 2);
    auto trunk = schedule(std::vector<Kind>(3, Kind::trunk), 8);
    require(trunk.shards[2] == 2 && trunk.shards[3] == 2);
    require(quota_workers(250000, 100000) == 2 && quota_workers(50000, 100000) == 1);
    auto hardware = detect_hardware();
    require(hardware.limit() >= 1 && hardware.limit() <= hardware.physical &&
            hardware.physical <= hardware.logical);
}

void queues_and_pool() {
    using namespace tuntom::mp;
    Pool small(2);
    auto *a = small.acquire();
    auto *b = small.acquire();
    require(a && b && a != b && !small.acquire());
    a->release();
    require(small.acquire() == a);
    a->release();
    b->release();
    require(small.in_use() == 0);
    Spsc<Buffer> full(1);
    require(full.push(a) && !full.push(b) && full.pop() == a && !full.pop());

    // Two ingress pools, two independent producers, two TX consumers. Each TX
    // can return slots to either pool while RX reuses them. This exercises the
    // release/acquire chain on actual payload memory (also run under TSan).
    constexpr std::size_t messages = 20000;
    Pool p0(16), p1(16);
    std::array<Pool *, 2> pools{&p0, &p1};
    std::array<std::array<std::unique_ptr<Spsc<Buffer>>, 2>, 2> matrix;
    for (auto &row : matrix)
        for (auto &queue : row)
            queue = std::make_unique<Spsc<Buffer>>(4);
    std::vector<std::thread> threads;
    for (std::size_t rx = 0; rx < 2; ++rx)
        threads.emplace_back([&, rx] {
            for (std::size_t sequence = 0; sequence < messages; ++sequence) {
                Buffer *buffer = nullptr;
                while (!(buffer = pools[rx]->acquire()))
                    std::this_thread::yield();
                buffer->size = sequence;
                for (std::size_t i = 0; i < 64; ++i)
                    buffer->data[i] = static_cast<std::uint8_t>(sequence + rx + i);
                while (!matrix[rx][sequence % 2]->push(buffer))
                    std::this_thread::yield();
            }
        });
    for (std::size_t tx = 0; tx < 2; ++tx)
        threads.emplace_back([&, tx] {
            std::array<std::size_t, 2> next{tx, tx};
            for (std::size_t received = 0; received < messages;) {
                for (std::size_t rx = 0; rx < 2; ++rx) {
                    if (auto *buffer = matrix[rx][tx]->pop()) {
                        require(buffer->size == next[rx]);
                        for (std::size_t i = 0; i < 64; ++i)
                            require(buffer->data[i] ==
                                    static_cast<std::uint8_t>(next[rx] + rx + i));
                        next[rx] += 2;
                        buffer->release();
                        ++received;
                    }
                }
                std::this_thread::yield();
            }
        });
    for (auto &thread : threads)
        thread.join();
    require(p0.in_use() == 0 && p1.in_use() == 0);
}

} // namespace

int main() {
    scheduling();
    queues_and_pool();
    std::cout
        << "PASS: hardware budget, weighted splits/merges, mixed roles, SPSC FIFO and pool reuse\n";
}
