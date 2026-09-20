# Multiple adapters sharing RX/TX workers

Standalone local IPC experiment. Eight tunnels are assigned equally to 1, 2 or
4 adapter endpoints. It uses the real tuntom SwitchClient, AF_UNIX SOCK_SEQPACKET,
production frame decoding/routing/label rewrites and the existing shared-worker
data path. No real adapter, NIC, encryption, route updates or multi-frame IPC.

The shared-worker draft now accepts a complete explicit ownership map, e.g.
`--rx-owner adapter0:0 --rx-owner adapter1:0 --rx-owner tunnel0:1 ...`, and
equivalently `--tx-owner`. Every port must be mapped, every worker must own a
port, and explicit maps cannot mix with dedicated options in the same direction.
Ownership is fixed at startup. These options do not change the packet loops.

Pools stay per input **port**, 128 buffers, with N×N bounded SPSC rings of 128
pointers. Capacity is identical between variants at a given adapter count;
adding ports increases total pools/rings. One frame per IPC message. RX/TX
rotating sweeps, pending buffers per output, POLLOUT recovery and one eventfd
wake gate per TX worker are unchanged.

## Workload and CPU controls

`up` = tunnels → adapters; `down` = adapters → tunnels; `duplex` = 50:50 total
offered traffic by direction. `equal` splits offered load equally among adapters;
`hot` gives adapter0 80%, the others split 20%. Its tunnels receive proportional
weights. Rate is aggregate frames/s over all endpoints/directions, not per port.
Pacing uses weighted rounds; this is not perfectly uniform packet spacing.

At rate=0 the source retries after POLLOUT and skips blocked ports; saturation
does not promise equal realized traffic among adapters. Fixed-rate EAGAIN is
recorded as source offer loss, not silently retried. Per-adapter offered and
delivered counts and per-port P99 expose imbalance. Each source/destination
sequence is checked for routing, length, identity and FIFO. Full-body checks
run separately from performance. Performance payloads have one label.

On the documented eight-physical-core host: switch CPU0–3; sources4,5; sinks6,7.
No workload SMT overlap. The OS and other processes remain untouched. Adapter
and tunnel ownership is asserted from the switch's control snapshot every run.

| Profile | `shared` | `sharded` | `per-adapter` |
| --- | --- | --- | --- |
| duplex | One adapter RX and one adapter TX; one tunnel RX/TX | Two RX/TX groups for four adapters | One RX/TX pair per adapter; tunnel pair unchanged |
| up | Two tunnel RXs; one TX for all adapters | Two TX groups for four adapters | One TX per adapter |
| down | One RX for all adapters; two tunnel TXs | Two RX groups for four adapters | One RX per adapter |

In duplex all adapter RX threads stay on CPU0, all adapter TXs on CPU1, tunnel
RX on CPU2 and tunnel TX on CPU3. **Extra threads do not gain extra cores.**
This compares thread ownership within the original four-core budget, not a
dedicated physical core per adapter thread.
The duplex `sharded` case was added in the follow-up series; the original
129-run main series omitted it. Its two adapter RXs still share CPU0 and
its two adapter TXs still share CPU1. No measured C++ or native binary changed.

In `up`, tunnel RXs keep CPU0,1; adapter TXs use CPU2 and, with multiple groups,
CPU3. In `down`, adapter RXs use CPU0 and potentially CPU1; tunnel TXs keep
CPU2,3. Thus directional tests can show the benefit of giving the busy adapter
direction two cores, while preserving two cores on the opposite side. With
four adapters, `sharded` and `per-adapter` have the same two adapter cores;
the latter time-shares two threads per core. Inactive directions still have
port owners/threads but are not included in the reported active-role CPU sums.

At one adapter the variants are identical, so only `shared` runs. At two,
`sharded` equals `per-adapter`, so the duplicate is skipped. Port-count and
variant order rotate between repetitions.

## Build and verify

```bash
bash experiments/switch_adapters/build.sh
python3 experiments/switch_adapters/check.py
g++ -std=c++17 -pthread -O1 -g -fsanitize=thread -Isrc \
  experiments/switch_workers/main.cpp -o /tmp/tuntom-switch-adapters-tsan
g++ -std=c++17 -pthread -O1 -g -fsanitize=thread -Isrc \
  experiments/switch_adapters/load.cpp -o /tmp/tuntom-switch-adapters-load-tsan
python3 experiments/switch_adapters/check.py --draft /tmp/tuntom-switch-adapters-tsan
python3 experiments/switch_adapters/bench.py --output /tmp/multi-adapter.json
```

The correctness suite includes one blocked adapter while a second adapter
remains live in both directions on the same RX/TX pair; POLLOUT-only recovery,
idle wakes, FIFO and buffer reclaim; plus invalid ownership rejection and
the preceding protocol/pressure checks. `--verify-all` enables full-body
validation for short smoke/sanitizer runs only.

Raw output is written after each run and not overwritten without `--resume`.
Resume verifies measured arguments and source/binary hashes. Each metadata file
records CPU topology, exact binary/source hashes, and compiler information.
PPS uses source elapsed time and counts the 500ms drain. CPU includes drain;
use CPU microseconds per delivered frame and per-worker CPU with that caveat.
P99 is sampled every 32nd sequence and describes delivered packets only.
Source/sink CPU and actual offer must be checked before calling a result a
switch capacity limit. Small differences across three repeats are not a guarantee.
