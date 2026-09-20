# Shared RX/TX worker experiment

Separate experimental binary; production code and the existing per-port draft
are not modified. This implements fixed startup ownership of multiple ports by
one RX/TX worker. Every input port still has its own pool 128, and every ordered
input/output pair has a bounded 128-slot SPSC pointer ring. A single RX owns each
row, and a single TX owns each output column. Total pool/ring capacities thus
match the old draft at the same port count. No packet copies are introduced
between RX and TX; Unix IPC still copies packet bytes through the kernel.

RX sweeps its assigned inputs with quota 1, rotating the start. Pool exhaustion
backs off that input for 50 us while other inputs remain serviceable. When no work is
available it ppolls eligible inputs, with the nearest pool retry deadline.
TX maintains a separate pending buffer and POLLOUT state per output port. It
rotates output ports and then source queues, with quota 1 per output per sweep.
A blocked output never makes this TX worker synchronously wait for that port
alone. A blocked output can still exhaust a *shared input port's pool*, affecting
other destinations of that same input; this is an existing pool design property.

Each TX worker owns an eventfd-based wake gate. RX performs the original-style
acq_rel exchange on that worker's sleeping flag after enqueue; only an armed
worker receives an eventfd write. TX arms, rechecks its eligible queues, then
polls its eventfd and blocked output sockets. A 20 ms timeout bounds shutdown.
This differs from the previous futex-only wake and is separately controlled by
the `ports` vs `legacy` benchmark variants. `wake_calls` counts wake-write
requests, not actual context switches. No global broadcast or payload batching.

Workers and mutable lookup keys are cache-line aligned. Lookup keys are reused;
the `(string port,uint64 label)` hash, linear output lookup and label/opcode
rewrites preserve the original draft's routing semantics. Control snapshots
report per-port frame counts and owner IDs, plus per-worker CPU/poll counts.
They do not pretend to attribute shared-worker CPU to individual ports.

Complete fixed ownership maps can also be supplied with repeated
`--rx-owner PORT:WORKER` / `--tx-owner PORT:WORKER`. Every port must be mapped
and every worker assigned at least one port. This supports the multi-adapter
experiment documented in `experiments/switch_adapters/README.md`.

## Build and correctness

```bash
bash experiments/switch_workers/build.sh
python3 experiments/switch_workers/check.py
g++ -std=c++17 -pthread -O1 -g -fsanitize=thread -Isrc \
  experiments/switch_workers/main.cpp -o /tmp/tuntom-switch-workers-tsan
python3 experiments/switch_workers/check.py --draft /tmp/tuntom-switch-workers-tsan
```

Correctness checks cover full payloads through 65535 B, 1/8 labels, malformed frames,
EXIT/default-back, bounded pressure, FIFO, pool reclamation, an unread output
while another port on the same RX/TX workers remains live, POLLOUT-only recovery,
and wake-after-idle. Registration/FD lifetime remains fixed for the experiment;
production reconnect and dynamic route snapshots are not implemented.

## Variants and fixed CPU layout

The host has 8 physical cores / 16 logical CPUs. All measurements here use CPU 0-3
for the switch, sources 4,5 and sinks 6,7. No driver/switch SMT siblings overlap.
Affinity does not isolate the host from unrelated work or change its settings.

| Variant | RX:TX | Meaning |
| --- | --- | --- |
| reference | combined single thread | Current production switch binary, CPU 0 |
| legacy | N:N | Previous per-port futex draft; RXs on 0,1, TXs on 2,3 |
| ports | N:N | New eventfd draft, same per-port CPU mapping |
| g11 |1:1 | One global reader, one global writer; CPU 0,1 |
| g12 |1:2 | Three-worker follow-up; RX 0 and TX 1,2 |
| g21 |2:1 | Three-worker control; RX 0,1 and TX 2 |
| g22 |2:2 | Shared reader/writer groups; RX 0,1 and TX 2,3 |
| g31 |3:1 | RX 0,1,2 and TX 3 |
| g13 |1:3 | RX 0 and TX 1,2,3 |
| g22-tx |2:2 | TX worker0 owns only adapter; the other TX owns all tunnels |
| g22-both |2:2 | RX 0/TX 0 own only adapter; remaining RX/TX own all tunnels |

Dedicated variants run only with the hub profile. They reserve workers *within*
the same budget, not extra threads above it. Remaining ports are assigned by
round-robin, not by a hidden traffic oracle. The `pairs` profile uses equal
offered input weights; hub uses a 50:50 aggregate offer by direction. Trunk
aggregation is represented by this topology only; multi-frame IPC is not built.

```bash
python3 experiments/switch_workers/bench.py \
  --output /tmp/shared-workers-main.json --ports 8 --duration 4 --repeat 3
```

Use `--ports 6,10` and `--variants ...` for follow-up comparisons. An existing
result file is never overwritten unless `--resume` is explicit; resume keeps
completed runs, checks measured C++/binary hashes and records new metadata.
Every stored run checks frame identities, FIFO, routing and complete accounting.
Use `--verify-all` only for correctness, never mixed into performance A/B.

CPU includes the drain interval; PPS uses source elapsed time. P99 covers the
complete local IPC path and only delivered samples. Saturation sources adapt
to socket backpressure; fixed-rate sources expose EAGAIN as loss. Inspect both,
the per-direction counts, and generator/sink CPU before drawing conclusions.
