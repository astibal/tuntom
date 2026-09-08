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
- `runtime_recovery_test.py`: one-shot/persistent main-poll and forwarding
  allocation failures in live tuntom, switch and adapter loops; control
  responsiveness, bounded retry/CPU and fresh DATA recovery. Also covers client
  and server startup entropy failure/recovery. `runtime_faults.cpp` is loaded
  only into disposable test processes; the adapter uses the TUN fixture.
- `switch_client_test.cpp`: real full Unix accept queue and recovery; injected
  asynchronous connect, registration backpressure/interruption, socket errors,
  fixed shared deadline and registration-before-DATA ordering. Linker wrappers
  affect only this test; an alarm bounds any blocking regression.
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
  also checks encrypted 9000-byte fragmentation and reassembly counters while
  automatic statistics are disabled.

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
