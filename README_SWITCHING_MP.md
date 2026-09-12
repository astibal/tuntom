# tomtom-switch-mp

Independent multithread switch target. `tuntom-switch` remains the single-thread
implementation; deployment helpers still select it. Both use the existing
SOCK_SEQPACKET registration and frame protocol, so the same `tuntom` tunnels,
exit adapters and `tuntomctl` work with either binary.

## Local helper and automatic pool

`mk_switch_mp.sh` builds and manages a local MP instance using the same
build-before-stop, hook, socket ownership and saved-endpoint lifecycle as
`mk_switch.sh`. It accepts every MP binary option, plus the usual helper options
and `--auto-pool`, `--reserve-cpus` and `--dry-run`. No SSH or tunnel secret is
needed. Use `--help` for the complete option list.

```bash
# Inspect the proposed allocation without sudo, hooks or daemon startup:
./mk_switch_mp.sh fabric --auto-pool --rules-file examples/switch.rules --dry-run

# Build, validate and start/restart after reviewing the rules:
./mk_switch_mp.sh fabric --auto-pool --rules-file examples/switch.rules

# Explicit CPU headroom and TX weighting; all aggregate weights remain tunable:
./mk_switch_mp.sh fabric --auto-pool --reserve-cpus 2 --workers 6 \
  --tx-weight 2 --exit-port internet --trunk-port backbone \
  --route tunnel0:17=internet:83 --route internet:83=tunnel0:17

./mk_switch_mp.sh fabric --stop
```

Automatic planning uses the **same C++ hardware detector and scheduler as the
switch**, compiled into a small temporary planning utility. It reads affinity,
physical package/core IDs from sysfs and the process's cgroup v2 hierarchy from
`/proc`. The available CPU budget is the smaller of physical cores allowed by
affinity and any detected CPU quota, with the same conservative fallback as the
binary. Hyperthreads are not counted as independent physical cores.

The initial policy is explicit and reproducible:

- By default, leave half of the available CPU budget, rounded up, outside the
  data worker pool. Always leave capacity for at least one data worker. Thus
  8 physical cores give a pool of 4, and 16 physical cores give a pool of 8.
- `--reserve-cpus N` replaces that default; zero allows the full detected budget.
  Reserving the entire budget is rejected before stopping the current instance.
  `--workers N` adds a cap on the remaining pool. These are worker-count limits,
  **not exclusive CPU reservations** for tunnels, adapters or the hypervisor.
- Keep the selected split threshold and RX/TX weights (defaults 8 and 1/1).
  Unless explicitly set, adapter weight becomes `max(2, ceil(threshold/2))`
  and trunk weight becomes `max(4, threshold)`: defaults 4 and 8. At default
  direction weights and with enough workers, up to two adapters share an RXa/TXa
  pair; a third adapter or second trunk triggers another aggregate group.
- Count distinct ports from both sides of routes and from `--exit-port` and
  `--trunk-port`, including rules read after `pre/up`. Run the normal weighted
  scheduler to print the expected role groups and each worker's role mask.
  The pool retains idle workers for future registrations.

For example, **20 declared tunnel ports + 3 adapters** with 16 physical cores,
the default reserve and enough port capacity predicts a pool of 8 with
`2 RX + 2 TX + 2 RXa + 2 TXa`. With 8 physical cores the default pool is 4 and
the roles share one worker each. On a one-worker budget all four roles share
that worker. Explicit weights take precedence regardless of option order.

This policy is a starting point informed by the IPC experiments; it does not
measure traffic or promise a throughput gain. Printed `active_when_connected`
and role counts assume **all declared ports are connected**. Real ownership is
assigned on registration and recalculated on topology changes. Declaring an
adapter does not create or start it; use `mk_adapter.sh` for that. Undeclared
ports can still register and consume the pool within the same runtime policy.

`--dry-run` only reads inline/seed rules and compiles its planner in a temporary
directory. It prints the plan and shell-escaped daemon command, then removes
that directory. A C++17 compiler and an executable `TMPDIR` (default `/tmp`)
are required. It does not run `pre/up`, so hook-generated rules are absent.
An actual start recomputes after sudo and `pre/up`, before the validation probe
and before stopping the current instance. CPU affinity/quotas may differ across
the privilege boundary. No per-worker pinning or NUMA placement is performed;
guest CPU topology still depends on what the hypervisor exposes.

Both helpers manage **one switch per name**, using the same `switch-<name>`
state directory, binary path and instance lock. Running `mk_switch_mp.sh NAME`
replaces a single-thread switch of that name; `mk_switch.sh NAME` switches back.
Supply the desired rules/options as on a normal restart. Both helpers finish
the build, pre/up hooks, planning and isolated configuration check before
stopping the existing instance. A failure in those preparation steps keeps the
existing process running. As with a normal restart, failures after switchover
do not automatically roll back the previous binary or hook side effects.

`--stop` through either helper stops the named switch regardless of which
implementation started it, using saved sockets and down hooks. Concurrent
operations through the two helpers are rejected by the shared lock. Older MP
instances in `switch-mp-<name>` are recognized and retired during replacement,
including duplicates alongside the canonical instance. Their saved endpoints
and hooks are used for teardown, and a compatibility lock prevents overlap with
an older MP helper already operating on that name. Legacy binary files and the
lock directory can remain after migration; their process, PID and endpoint
record are removed. Use consistent `TUNTOM_STATE_DIR` and `TUNTOM_BIN_DIR` for
subsequent operations, as with the original helper.

The default socket names remain `<name>.sock` and `<name>.control`. Use different
**names** to run multiple managed switches; custom sockets do not make a second
instance of the same name. Sockets owned by another named instance are rejected.
Hooks retain `TUNTOM_COMPONENT=switch`, the existing
`TUNTOM_SWITCH_{PRE,POST}_HOOK` overrides, and `TUNTOM_SWITCH_RULES_FILE` seed
support. MP rules additionally accept `trunk-port ID`.

## Direct build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tomtom-switch-mp tuntomctl -j 4

./build/tomtom-switch-mp --socket /tmp/example-mp.sock \
  --control-socket /tmp/example-mp.ctl \
  --route tunnel0:17=adapter0:83 --exit-port adapter0 \
  --route adapter0:83=tunnel0:17
./build/tuntomctl /tmp/example-mp.ctl show stats
```

## Worker pool and scheduling

All data workers start together and run the same loop. Work consists of complete
port directions, with exactly one RX owner and one TX owner per registered port.

```text
                   generic worker pool (fixed hardware budget)
                  +-----------------------------------------+
initial roles     | RX(tunnels) | TX(tunnels) | idle | idle ..|
1 tunnel+adapter  | RX + TXa    | RXa + TX    | idle | idle ..|
more ports        | RX(tunnels) | TX(tunnels) | RXa  | TXa ...|
after threshold   | RX group 0  | RX group 1  | TX ..| RXa ...|
                  +-----------------------------------------+

RX(port A) -> pool A -> label lookup -> Q[A,B] --pointer--> TX(port B)
RX(port C) -> pool C -> label lookup -> Q[C,B] --pointer--/     |
                                                            +-> IPC send
```

An unassigned worker blocks on its own eventfd. `--workers N` caps the pool;
it cannot raise the detected hardware limit. The default counts physical cores
intersecting the process's startup CPU affinity, rather than SMT threads, and
also respects discovered cgroup v2 CPU quotas, including accessible ancestors.
Fractional quotas round down, with a minimum of one worker. If topology cannot
be read, the fallback is one. Cgroup v1 quota detection and later changes to
affinity/quota are not implemented; set `--workers` explicitly in that case.
Control and the existing logger have their own threads outside this **data**
worker budget. Workers inherit affinity; individual core pinning is left to Linux.

The default policy uses these weights:

| Parameter | Default | Meaning |
|---|---:|---|
| `--work-per-thread` | 8 | Target weighted sum per direction group |
| `--rx-weight` | 1 | RX direction cost multiplier |
| `--tx-weight` | 1 | TX direction cost multiplier |
| tunnel weight | 1 | Ordinary registered port |
| `--adapter-weight` | 2 | Port named by `--exit-port` |
| `--trunk-weight` | 4 | Port named by `--trunk-port` |

RX/TX tunnel roles start at one group each. Registering an adapter or trunk adds
the shared RXa/TXa roles. With spare workers, a group exceeding the target is
split, prioritizing the largest estimated per-worker load. A direction of one
port stays indivisible. Ports are assigned greedily by weight within the group.
When baseline roles exceed the CPU budget, one worker executes multiple roles;
even a single worker can progress in RX and TX without oversubscribing the pool.

For exactly one tunnel and one adapter, the startup policy co-locates the
forwarding directions: `RX(tunnel) + TX(adapter)` and `RX(adapter) + TX(tunnel)`.
It uses two workers when both combined weights fit `--work-per-thread`, avoiding
inter-worker notifications on these two paths. Other allocated workers sleep.
Adding a third port returns to the general role scheduler through the normal
barrier; removing it can restore this pairing. A trunk or a heavier combination
uses the general policy. This is a topology/weight rule, not measured-load scaling.
For this 1+1 topology, `--work-per-thread 2` with the default weights keeps the
four separate roles when the pool permits it. Pairing favors CPU efficiency;
it does not guarantee the same saturation throughput as four busy workers.

With at least eight available workers, the defaults produce:

| Connected ports | Active data workers |
|---|---|
| 0–8 tunnels | 1 RX + 1 TX |
| 9–16 tunnels | 2 RX + 2 TX |
| 1 tunnel + 1 adapter | 2: RX+TXa, RXa+TX |
| 8 tunnels + 1–4 adapters | 1 RX + 1 TX + 1 RXa + 1 TXa |
| 8 tunnels + 5 adapters | 1 RX + 1 TX + 2 RXa + 2 TXa |

This first policy recomputes on registration, replacement and disconnection.
Weights are configurable estimates informed by the earlier experiments;
it does **not** infer utilization or learn weights from live traffic. An idle
port counts toward its group. CPU-driven scaling/hysteresis is a later policy
extension, independent of the worker machinery.

## Ownership, tables and backpressure

Each ingress has a bounded pool (default 128 buffers). RX scans FREE/USED slots
circularly. Payload bytes are never cleared or copied between workers; a pointer
is published through a release/acquire SPSC ring. TX releases the original slot.
Pools belong to ingress ports so they survive moving RX to another worker.

The RX × TX matrix allocates one ring per connected, routed source/destination
pair, plus loopback pairs when default-back is enabled. Multiple labels for the
same pair share the ring. Unused pairs consume no ring storage. Default ring
capacity is 128 pointers; `--pool-size` and `--queue-size` tune the bounds.
A pool reserves about 8 MiB of virtual payload space at the default maximum
frame size; resident memory grows as those buffers are used.

Every worker rotates its job scan start. Each TX visits incoming RX queues in
round-robin order with quota **one frame per turn**, and sends one IPC record
per frame. Only the destination's current TX worker receives a packet wakeup.
All-worker wakeups happen at reconfiguration/shutdown barriers, not on data.

Each worker has an edge-triggered epoll set. Only ready RX sockets are read;
readiness stays set across round-robin turns until `recv()` returns `EAGAIN`.
New events are collected after at least 32 successful job operations, at the
end of that round-robin pass, or when the worker has no runnable work. This
avoids repeatedly reading inactive ports without starving newly active inputs.
The eventfd is read only when reported ready, with one successful read clearing
its accumulated count. Sleeping still uses arm/recheck/wait to avoid lost wakes.

TX retains a pending buffer on `EAGAIN`, enables `EPOLLOUT`, and continues other
jobs. A writable event enables the next attempt and removes output interest;
there is no periodic TX retry in normal operation. An exhausted RX pool is
retried after 50 microseconds while
other inputs continue. A full pointer ring drops the new frame and increments
`queue_full_drops`. Thus one congested destination can exhaust its source's pool
and backpressure that source's other traffic; other ingress pools remain usable.

Main constructs a complete next plan before pausing workers, including private
RX lookup tables, resolved TX worker IDs and new epoll sets. The separate sets
keep old readiness tokens from referring to new port generations. FD admission
reserves another epoll FD per worker while both plans coexist. Failed preparation
closes the unpublished resources and leaves the current plan usable.
At a bounded operation boundary all workers
park; main publishes the version, swaps owners, and resumes them. There is no
packet allocation or shared route-map lock on the forwarding path. Tables are
compiled per ingress with resolved outputs, unlike the reference switch's
linear target-name search. Routes themselves currently come from startup CLI;
live route-edit commands are not implemented. Registration changes refresh
the resolved targets through the same versioned plan mechanism.

Queues, pool cursors and pending TX pointers survive a worker migration.
A disconnected/replaced port gets a new generation: queued and pending frames
involving the old generation are released at the barrier and counted as
`reconfiguration_drops`. Frames already sent into an old peer's kernel socket
cannot be recalled. Frames still unread on an ingress socket are interpreted
under whichever plan is active when RX consumes them. Order is maintained
within each surviving source/destination pair; unrelated ingress streams have
no common ordering. Shutdown drops/reclaims outstanding userspace buffers.

## Observability and validation

`show stats` keeps `component=switch` and adds `implementation=tomtom-switch-mp`.
It includes the hardware budget, pool/active/idle worker counts, role group
counts, plan and per-worker versions, port generations and RX/TX owners, worker
CPU time and poll counts, queue/pool pressure, barrier time and discarded frames.
`worker_io_backend=epoll` identifies this socket phase. `recv_calls`,
`recv_eagain`, `send_calls`, `send_eagain`, `wake_calls`, `wake_reads` and
`cpu_samples` make syscall activity observable. `worker_N_poll_calls` counts
epoll waits (including nonblocking event collection); it excludes the extra
`ppoll()` on the epoll FD used for sub-millisecond RX pool retries.
Worker CPU snapshots refresh at most every 100 ms while the worker runs and at
shutdown; a sleeping worker may retain its last sample until its next wake.
Use process CPU deltas over a measured interval for benchmark comparisons.
Counters are concurrent observations, not an atomic cross-worker snapshot.
`workers_active` counts assigned roles (including the two empty startup roles),
not runnable threads. The reference switch's adaptive-polling counters are not
part of this implementation.

```bash
cmake --build build -j 4
ctest --test-dir build --output-on-failure
# Focused real IPC and policy/queue checks:
ctest --test-dir build --output-on-failure -R 'switch_mp'
```

The tests cover weighted splitting/merging, small CPU budgets and affinity,
concurrent SPSC payload reuse, original label/EXIT semantics, adapters sharing
workers, trunks, live migration under traffic, FIFO, blocked-output isolation,
pending-buffer migration, exhausted pools, reconnect generation cleanup,
registration limits/timeouts and interoperability with the existing tunnel.
The same unit and IPC suites can run against ThreadSanitizer builds. These
tests do not create TUN devices or change host network configuration.
