#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sched.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace tuntom::mp {

struct Hardware {
    std::size_t logical = 1;
    std::size_t physical = 1;
    std::size_t quota = 0; // Zero means no discovered cgroup v2 quota.
    std::size_t limit() const { return quota ? std::min(physical, quota) : physical; }
};

inline std::size_t quota_workers(std::uint64_t quota, std::uint64_t period) {
    // Fractional CPU budgets use the smaller whole count, with a minimum of one.
    return period ? static_cast<std::size_t>(std::max<std::uint64_t>(1, quota / period)) : 0;
}

inline Hardware detect_hardware() {
    Hardware result;
    // Dynamic CPU sets also handle machines with CPU IDs above CPU_SETSIZE.
    auto count = std::max<long>(128, ::sysconf(_SC_NPROCESSORS_CONF));
    std::vector<int> allowed;
    for (; count <= 1024 * 1024; count *= 2) {
        cpu_set_t *set = CPU_ALLOC(static_cast<std::size_t>(count));
        if (!set)
            throw std::bad_alloc();
        const auto bytes = CPU_ALLOC_SIZE(static_cast<std::size_t>(count));
        CPU_ZERO_S(bytes, set);
        const int ok = ::sched_getaffinity(0, bytes, set);
        if (ok == 0) {
            for (int cpu = 0; cpu < count; ++cpu)
                if (CPU_ISSET_S(cpu, bytes, set))
                    allowed.push_back(cpu);
        }
        CPU_FREE(set);
        if (ok == 0)
            break;
    }
    if (allowed.empty())
        return result; // Conservative on unavailable topology.
    result.logical = allowed.size();
    std::set<std::pair<int, int>> cores;
    bool known = true;
    for (const auto cpu : allowed) {
        const auto base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
        int package = -1, core = -1;
        if (!(std::ifstream(base + "physical_package_id") >> package) ||
            !(std::ifstream(base + "core_id") >> core) || package < 0 || core < 0) {
            known = false;
            break;
        }
        cores.emplace(package, core);
    }
    result.physical = known ? cores.size() : 1;

    // Resolve the process's unified cgroup against its mount, including parents.
    // In cgroup namespaces the mount root can already be the process's group.
    std::string group, line;
    std::ifstream groups("/proc/self/cgroup");
    while (std::getline(groups, line))
        if (line.compare(0, 3, "0::") == 0)
            group = line.substr(3);
    std::ifstream mounts("/proc/self/mountinfo");
    while (!group.empty() && std::getline(mounts, line)) {
        if (line.find(" - cgroup2 ") == std::string::npos)
            continue;
        std::istringstream fields(line);
        std::string id, parent, device, root, mount;
        fields >> id >> parent >> device >> root >> mount;
        // Escaped mount paths are rare; fall back conservatively to affinity.
        if (mount.empty() || mount.find('\\') != std::string::npos)
            continue;
        std::string suffix;
        if (root == "/")
            suffix = group;
        else if (group == root || group == "/")
            suffix = "";
        else if (group.compare(0, root.size() + 1, root + "/") == 0)
            suffix = group.substr(root.size());
        else
            continue;
        std::string path = mount + (suffix == "/" ? "" : suffix);
        for (;;) {
            std::string amount;
            std::uint64_t period = 0;
            if (std::ifstream(path + "/cpu.max") >> amount >> period) {
                if (amount != "max" && period) {
                    std::istringstream number(amount);
                    std::uint64_t value = 0;
                    if (number >> value) {
                        const auto limit = quota_workers(value, period);
                        result.quota = result.quota ? std::min(result.quota, limit) : limit;
                    }
                }
            }
            if (path == mount)
                break;
            const auto slash = path.rfind('/');
            if (slash == std::string::npos || slash < mount.size())
                break;
            path.resize(slash);
        }
        break;
    }
    return result;
}

enum class Kind { tunnel, adapter, trunk };
enum class Role : std::size_t { rx = 0, tx = 1, rxa = 2, txa = 3 };
inline const char *role_name(std::size_t role) {
    constexpr const char *names[] = {"RX", "TX", "RXa", "TXa"};
    return names[role];
}
inline const char *kind_name(Kind kind) {
    return kind == Kind::adapter ? "adapter" : kind == Kind::trunk ? "trunk" : "tunnel";
}

struct Policy {
    std::size_t work_per_thread = 8;
    std::size_t rx_weight = 1, tx_weight = 1;
    std::size_t adapter_weight = 2, trunk_weight = 4;
};

struct Assignment {
    std::vector<std::array<std::size_t, 2>> owners;
    std::vector<unsigned> worker_roles; // Bit mask; a worker may serve any mix.
    std::array<std::size_t, 4> shards{};
    std::size_t active = 0;
};

// Pure topology policy, deliberately independent of thread creation / sockets.
// Weighted sums are estimates, not a claim to measure CPU utilization. A new
// port can split a group; removal merges it. No periodic load-based flapping.
inline Assignment schedule(const std::vector<Kind> &ports, std::size_t budget,
                           const Policy &policy = {}) {
    if (!budget || !policy.work_per_thread || !policy.rx_weight || !policy.tx_weight ||
        !policy.adapter_weight || !policy.trunk_weight)
        throw std::invalid_argument("Invalid scheduler budget or weight");
    Assignment result;
    result.owners.resize(ports.size());
    result.worker_roles.resize(budget);
    std::array<std::vector<std::pair<std::size_t, std::size_t>>, 4> jobs;
    std::array<std::uint64_t, 4> totals{};
    for (std::size_t port = 0; port < ports.size(); ++port) {
        const auto kind = ports[port];
        const std::size_t base = kind == Kind::tunnel    ? 1
                                 : kind == Kind::adapter ? policy.adapter_weight
                                                         : policy.trunk_weight;
        for (std::size_t direction = 0; direction < 2; ++direction) {
            const auto role = (kind == Kind::tunnel ? 0U : 2U) + direction;
            const auto cost = base * (direction == 0 ? policy.rx_weight : policy.tx_weight);
            jobs[role].push_back({port, cost});
            totals[role] += cost;
        }
    }
    // A small tunnel <-> adapter path starts with one worker per forwarding
    // direction. The next topology change recomputes the ordinary role shards.
    // Pair only while both combined direction costs fit the configured target.
    if (ports.size() == 2 && budget >= 2 && jobs[0].size() == 1 && jobs[2].size() == 1 &&
        ports[jobs[2][0].first] == Kind::adapter &&
        totals[0] + totals[3] <= policy.work_per_thread &&
        totals[2] + totals[1] <= policy.work_per_thread) {
        result.owners[jobs[0][0].first] = {0, 1};
        result.owners[jobs[2][0].first] = {1, 0};
        result.worker_roles[0] = (1U << 0) | (1U << 3);
        result.worker_roles[1] = (1U << 2) | (1U << 1);
        result.shards = {1, 1, 1, 1};
        result.active = 2;
        return result;
    }
    result.shards = {1, 1, jobs[2].empty() ? 0U : 1U, jobs[3].empty() ? 0U : 1U};
    std::size_t used = result.shards[0] + result.shards[1] + result.shards[2] + result.shards[3];
    while (used < budget) {
        std::size_t best = 4;
        for (std::size_t role = 0; role < 4; ++role) {
            if (result.shards[role] >= jobs[role].size() ||
                totals[role] <= policy.work_per_thread * result.shards[role])
                continue;
            if (best == 4 ||
                totals[role] * result.shards[best] > totals[best] * result.shards[role])
                best = role;
        }
        if (best == 4)
            break;
        ++result.shards[best];
        ++used;
    }
    std::vector<std::uint64_t> loads(budget);
    std::size_t next = 0;
    for (std::size_t role = 0; role < 4; ++role) {
        std::vector<std::size_t> workers;
        for (std::size_t shard = 0; shard < result.shards[role]; ++shard) {
            // When even baseline roles don't fit, co-locate whole roles. The
            // same worker loop still makes progress in both directions.
            const auto worker =
                next < budget ? next++
                              : static_cast<std::size_t>(
                                    std::min_element(loads.begin(), loads.end()) - loads.begin());
            workers.push_back(worker);
            result.worker_roles[worker] |= 1U << role;
        }
        std::stable_sort(jobs[role].begin(), jobs[role].end(),
                         [](auto a, auto b) { return a.second > b.second; });
        std::vector<std::uint64_t> local_load(workers.size());
        for (const auto &job : jobs[role]) {
            const auto shard = static_cast<std::size_t>(
                std::min_element(local_load.begin(), local_load.end()) - local_load.begin());
            const auto worker = workers[shard];
            result.owners[job.first][role % 2] = worker;
            local_load[shard] += job.second;
            loads[worker] += job.second;
        }
    }
    result.active = next;
    return result;
}

} // namespace tuntom::mp
