# Tests

Run the C++ regression tests from any working directory:

```bash
bash /path/to/tuntom/tests/run.sh
```

Requires a C++17 compiler (`g++` by default; override with `CXX=clang++`).
The runner checks that each header compiles on its own, then compiles
`src/main.cpp`. The regression tests include the real `src/` headers.
The C++ tests are also available through CMake/CTest in CLion.
Builds use a temporary directory that is removed on exit. No root privileges,
live tunnel, or network access are needed.

- `replay_test.cpp`: reordered timestamps, duplicates, window eviction,
  integer boundaries, and reassembly of 64 fragments received in reverse order.
- `udp_batch_test.cpp`: real loopback datagram boundaries and ordering, one-call
  fragment batches, partial success followed by completion or EMSGSIZE/EAGAIN,
  interruption, the 64-fragment bound and the single-datagram fast path.
  Linker wrappers inject send outcomes only into this test.
- `logging_test.cpp`: bounded allocation-free formatting, errno preservation,
  SIGPIPE/EPIPE, a full queue with a stalled writer, flood limiting and recovery,
  ordered queue wraparound, failed thread creation, inherited closed stderr,
  I/O failures and partial writes, the file-size cap and copytruncate recovery,
  and exit without joining a blocked writer.
- `logging_recovery_test.py`: twelve live-daemon scenarios (full stderr pipe,
  closed reader, stalled write, failed thread creation) across tuntom, switch
  and adapter. Verifies control and DATA during failure, fixed thread counts,
  logging recovery and graceful exit with a blocked writer. Uses test-only
  `logging_faults.cpp` injection and the socketpair TUN fixture.
- `reassembly_test.cpp`: full-pool FIFO eviction, late-fragment suppression,
  5,000 intact packets delivered in reverse-fragment order under sustained loss
  before the three-second timeout, byte-limit pressure, activity-ordered expiry,
  bounded discarded IDs, validation and shared metrics lifetime. Uses a simulated
  clock, including equality at timeout boundaries.
- `mac_test.cpp`: independent Ascon permutation comparisons, custom MAC
  vectors, v5 tampering checks, both traffic directions, reflection rejection
  for ordinary v5 message types, legacy v4 rejection, and the XOR forgery regression.

- `session_test.cpp`: full handshake, lost INIT/ACK, duplicate control messages,
  reordering, restart replay, expired pending state, previous-session overlap,
  hint collisions, per-session reassembly, counter exhaustion, tampering and
  rejection of unsupported suites/nonempty DH. Uses a simulated clock.
- `session_latency_test.cpp`: all suites at 400/1200/4800 ms RTT, loss of each
  handshake flight, bidirectional DATA and replay checks, exact INIT/RESPONSE
  retries, unchanged pending deadlines, rejection of altered or retired INIT
  replays, fresh PFS material after expiry, and delayed periodic PFS rekey with
  a lost RESPONSE. Also checks the immediate first RESPONSE resend and 200ms
  spacing under duplicate floods. Uses real codecs with a simulated network and clock.
- `dissector_test.py`: offline synthetic capture with handshake fields,
  split counters, malformed messages, v3 compatibility and fragment separation
  across different hints. Run automatically if `tshark` and `python3` are
  installed, otherwise explicitly skipped. No global Wireshark settings change.

These are regression checks, not a security proof of the custom MAC.

`pmtud-iptables-test.sh` is a separate, manual integration helper that changes
firewall rules and requires root. It is deliberately not run by `run.sh`.

- `aead_test.cpp`: all 1,089 official Ascon-AEAD128 known-answer vectors,
  in-place operation, modified ciphertext/tags, and failure wiping.
  `ascon-kat.txt` comes from the Ascon team's public reference repository:
  https://github.com/ascon/ascon-c/blob/main/crypto_aead/asconaead128/LWC_AEAD_KAT_128_128.txt
- `encrypted_session_test.cpp`: both modes, mode mismatch/downgrade rejection,
  encrypted fragments/control packets, tampering, retransmission, restart,
  reflection, replay, counter exhaustion and CLI compatibility. All suites also
  check full-pool recovery and late-fragment suppression through the real codec.
- `aead_bench.cpp`: optional allocation-reusing codec microbenchmark (MB/s):
  `g++ -std=c++17 -O3 -march=native tests/aead_bench.cpp -o /tmp/aead-bench && /tmp/aead-bench`.
  Results include header handling and authentication; they are not tunnel throughput.

- `x25519_test.cpp`: RFC 7748 public/shared secrets and 1/1000-iteration
  vectors, scalar clamping, noncanonical/high-bit public inputs, low-order rejection.
- `akdf_test.cpp`: exact AKDF framing and fixed known-answer vectors, domain
  separation, full DH/PSK/context binding and exception-path buffer wiping.
  `python3 tests/akdf_vectors.py` regenerates vectors with an independent,
  test-only lookup-S-box AMAC implementation.
- `pfs_session_test.cpp`: suite-2 exchange, tampering, authenticated malformed
  DH/lengths, suite isolation, lost flights, periodic DH rekey under continuous
  traffic, previous-session expiry, restart and counter limits. The encrypted
  session tests also run with PFS, including every control type and fragmentation.
- `session_stats_test.cpp`: serialized suite/session gauges, both-side handshake
  completion boundaries, duplicate flights, retries/timeouts, rekey counts,
  authenticated suite/DH rejection counters, and reassembly accounting across
  overlapping sessions, retirement, expiry and rekey. Runs without TUN or root.

- `stats_control_test.cpp`: `--no-stats` argument-order independence and path
  preservation, real SIGUSR1 toggle / SIGUSR2 snapshot handling, pending signals, snapshot
  write gating while disabled, and handler restoration.

- `compact_protocol_test.cpp`: exact compact layouts, implicit tunnel context,
  authenticated version rejection, flag and truncation rejection, 16-bit fragment
  boundaries, large reassembly and PMTUD wire sizes.
- `switch_protocol_test.cpp`: switch frame layout, label stacks, top-label swap,
  truncation and malformed-header rejection.
- `switch_options_test.cpp`: switch CLI dependencies and label parsing.
- `runtime_state_test.cpp`: allocation-failure sweeps across all handshake
  stages/suites, LRU insertion and reassembly; entropy failures/backoff at every
  suite-2 RNG step, continued old-session traffic, replay/TX nonce safety,
  expired candidate retirement and control FD cleanup on allocation failure.
- `runtime_recovery_test.py`: one-shot/persistent main-poll failures in live
  tuntom, switch and adapter loops; control responsiveness, bounded retry/CPU
  and fresh DATA recovery. With the former frame-buffer allocation failure
  armed, forwarding must now succeed without entering recovery. Also covers client
  and server startup entropy failure/recovery. `runtime_faults.cpp` is loaded
  only into disposable test processes; the adapter uses the TUN fixture.
- `switch_client_test.cpp`: real full Unix accept queue and recovery; injected
  asynchronous connect, registration backpressure/interruption, socket errors,
  fixed shared deadline and registration-before-DATA ordering. Linker wrappers
  affect only this test; an alarm bounds any blocking regression. Also checks
  allocation-free scatter/gather sends, exact record boundaries and bytes for
  1/8 labels and 64/1500/9000/65535-byte payloads, send errors, real full-queue
  recovery and closed peers.
- `switch_reconnect_test.py`: full queue during tuntom startup and reconnect,
  live UDP handshake/data reception and responsive control during the outage,
  bounded retries and bidirectional recovery. Also runs the real adapter loop
  with a socket pair in place of TUN (`adapter_tun_fixture.cpp`) and verifies
  TUN service, retained routes and recovery; checks live switch listener flags.
  Runs through both CTest (when Python is available) and `tests/run.sh`.
- `exit_adapter_test.cpp`: reverse L3/L4 learning, fragment fallback, cache miss,
  idle expiry and LRU eviction.
- `switch_test.py`: one-listener Unix `SOCK_SEQPACKET` registration and reconnect,
  label swap, stack preservation, explicit exit-port routes and default-back
  `EXIT` behavior, plus `tuntomctl show stats` and switch BPS/PPS fields over the
  control socket.
- `switch_tunnel_test.py`: two unprivileged tuntom processes carrying an opaque
  payload through a live UDP session and the label switch without creating TUNs;
  also checks encrypted 9000-byte fragmentation in both directions, TX buffer
  reuse across changing fragment counts and reassembly counters while automatic
  statistics are disabled.

`switch_admission_test.cpp` checks the accept-rate bound under continuous demand,
idle burst cap, retry deadlines and FD headroom calculations with simulated time.
`switch_capacity_test.py` runs the real switch with a 64-FD limit, including
inherited descriptors: a full pending pool preserves control and bidirectional
forwarding; pending expiry preserves idle registered ports; replacement at full
port capacity survives an injected allocation failure. Repeated churn checks
bounded acceptance, CPU, FD count and RSS. Forwarding from an already registered
63-character port must also survive faults on the former per-packet port-name
allocation. `accept_faults.cpp` injects one-shot
and persistent accept errors on either listener, including control accept failure
during PF-02 poll recovery. Tests check actual new packets and new registrations
after recovery without restarting. Faults are confined to disposable processes;
no system-wide resource exhaustion, root access or TUN setup is required. Both
CTest and `tests/run.sh` run these regressions. `mk_local_test.py` also verifies
capacity option validation and forwarding to the deployed switch.

`mk_stop_test.py` checks the deployment script's process cleanup with disposable
local processes (no root or SSH connection required). It covers missing/stale
PID files, duplicate instances, role/ID/interface matching, the remote command
wrapper and TERM-to-KILL fallback. Requires Python 3 and `cc`; the shell runner
runs it automatically when both are available.

`mk_switch_args_test.py` checks per-side switch argument validation, exit-node
dependencies and whether pure switch sides correctly suppress TUN setup.

`mk_local_test.py` checks `mk_switch.sh` and `mk_adapter.sh` with real binaries
and disposable local sockets. Covers generated flow forwarding, build/rule
failures keeping the old process alive, hook ordering and saved context, socket
ownership, lock contention, unrelated sockets/files/symlinks, missing/stale PID
files, duplicate processes, crash recovery, TERM-to-KILL fallback, adapter
startup without a switch, and failed-start cleanup. Privilege/account setup
and interface inspection are replaced; the real adapter loop uses
`adapter_tun_fixture.cpp`. No root, SSH or host network changes are needed.
Also checks cleanup of instances using the former binary location. When a
writable `noexec` mount is available, verifies switch restart and adapter startup
with runtime state there, binaries on an executable filesystem, and rejection
of a `noexec` binary directory before compilation or stopping the old process.
Run directly with `python3 tests/mk_local_test.py` (builds its fixtures), or
through `tests/run.sh`, which reuses its compiled fixtures.

`stats_socket_test.py` checks live socket snapshots with no stats path and an
unusable path, untouched/missing files under `--no-stats`, continued processing
and reassembly-span sampling, completed BPS/PPS throughput buckets, file-only SIGUSR1
toggles without history resets, and explicit SIGUSR2 file snapshots. Uses only
disposable unprivileged switch/UDP processes; run automatically by `run.sh`.

`ipc_bench.cpp` / `ipc_bench.py` form a manual A/B benchmark of the real switch
and IPC frame producer. Compile both switch versions with identical flags;
compile the producer against each version's `src/` headers, adding
`-DTUNTOM_IPC_GATHER` for the scatter/gather version. Example producer build:

```bash
g++ -std=c++17 -pthread -O3 -march=native -mtune=native -Isrc \
    -DTUNTOM_IPC_GATHER tests/ipc_bench.cpp -o /tmp/ipc-producer-new
python3 tests/ipc_bench.py OLD_SWITCH OLD_PRODUCER NEW_SWITCH NEW_PRODUCER OUTPUT.json
```

The runner uses temporary Unix sockets and three distinct available physical
cores for switch, source and sink. It alternates three paired 3-second runs
for 64/1500/9000-byte payloads at saturation and 9000-byte payloads at 15 kpps.
It records delivered PPS, losses, CPU seconds per process/thread, binary hashes
and affinity. It needs no root, TUN or live network configuration changes.
Results include all three participants; this is neither a standalone switch
limit nor a prediction of encrypted tunnel throughput. Affinity does not
reserve the cores against other workloads. This benchmark is not run by CTest.

`ipc_scale.cpp` / `ipc_scale.py` measure eleven full-duplex IPC ports: ten
tunnels in five pairs, plus an exit adapter. By default each tunnel sends 80%
of frames to its partner and 20% to the adapter; the adapter has twice each
tunnel's ingress rate and splits its traffic equally among the ten tunnels.
Use `--adapter-weight 1` for equal rates on all ports instead. A `mixed`
payload is two 9000-byte packets per 64-byte packet; these are opaque test
records, not actual IP/TCP sessions. Size 0 in the C++ driver selects this mix.

```bash
g++ -std=c++17 -pthread -O3 -march=native -mtune=native -Isrc \
    tests/ipc_scale.cpp -o /tmp/ipc-scale
python3 tests/ipc_scale.py --switch NEW_SWITCH --baseline OLD_SWITCH \
    --driver /tmp/ipc-scale --output /tmp/ipc-scale.json \
    --case mixed:180000 --case mixed:240000 --case 9000:0
```

`--baseline` is optional; both switches use the same scatter/gather driver,
so this comparison isolates the switch changes. Three paired 5-second runs
per case alternate order. Rate is aggregate offered **forwarded frames/s**:
one frame accounts for one source TX and one destination RX, not two switch
forwards. At 180 kframes/s the default mix means 30 k IPC TX+RX frames/s per
tunnel and 60 k per adapter. This models packet rates, not worker CPU burn.

The runner waits for all registrations via the control socket, pins switch,
sender and receiver to separate physical cores, and saves binary hashes and
raw counters, host CPU load and switch runqueue wait (zero when kernel scheduler
statistics are disabled). The reader drains at most 16 records per tunnel port
and 32 per weighted adapter per round. It cross-checks source/sink counts with switch RX/TX/drop stats,
and validates length, output opcode, label, destination and sequence order.
Per-port source EAGAIN counts and switch output drops are recorded separately.
P50/P99 end-to-end IPC latency samples every 32nd source sequence; maximum
latency includes every received record. Timestamps precede the source send,
so latency includes socket queues and reader scheduling, not just switch CPU.

The source sends paced rounds (12 frames by default); overdue rounds can
catch up in bursts. At rate 0 it attempts unlimited sends and counts EAGAIN
without retrying. Thus saturation `offered_loss_percent` includes rejected
source writes: consult `switch_loss_percent` for losses after switch ingress.
The receiver drains for 200 ms before reconciling counters. CPU accounting
includes startup/drain overhead but divides by the actual sending interval.
Do not treat a source/sink saturated result as a pure switch capacity limit.
Like `ipc_bench`, this is manual and does not require root, TUN or deployment.

## Multithread switch target

`switch_mp_plan_test.cpp` checks automatic helper allocation for physical versus
logical cores, CPU quotas, small budgets, aggregate role splitting and explicit
overrides. `mk_switch_mp_test.py` covers the helper's options, seed rules and
read-only dry run without sudo/hooks/runtime writes. Both run under CTest.
`mk_local_test.py MP_BINARY ADAPTER_FIXTURE CTL_BINARY mp` repeats the real local
lifecycle checks for the MP helper, including hook-generated adapter/trunk roles,
auto planning before restart, and preserving a running instance when planning
fails. `tests/run.sh` runs both lifecycle variants and the MP sudo handoff check.

`mk_switch_replacement_test.py` exercises single-thread/MP replacement in both
directions using the shared name, lock and state. It checks forwarding after
handoff, cross-helper stop, failed preparation preserving the old process,
custom saved endpoints, name isolation and migration of old `switch-mp-NAME`
instances, including stale PID files, duplicate instances and legacy hooks.
It uses disposable local sockets and replaces privilege/account setup; it runs
under CTest and `tests/run.sh` without creating real TUN devices.

`switch_mp_test.cpp` checks the weighted scheduler, hardware budget and concurrent
SPSC/pool reuse. `switch_mp_integration_test.py` runs `tomtom-switch-mp` through
live port additions/removals, RX/TX migration, pressure, reconnect generations,
protocol and admission tests with 1, 2 and up to 8 data workers. CTest and
`tests/run.sh` include both, plus the existing tunnel integration against the new
target. See [MP architecture and build options](../README_SWITCHING_MP.md).

The socket-phase regressions additionally check the arm/recheck/wait wake
handshake, RX syscall counts with 20 inactive ports, retained edge-triggered
readiness and fairness when an idle input becomes active. A tunnel/adapter pair
starts with two forwarding workers, splits when another port arrives and merges
again while duplex traffic continues. CPU sampling is checked independently of
packet rate. Resource-failure and multi-port stress tools, plus a reproducible
before/after MP benchmark, live in [experiments/switch_mp](../experiments/switch_mp/README.md).
