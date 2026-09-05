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
- `mac_test.cpp`: independent Ascon permutation comparisons, custom MAC
  vectors, v2/v4 tampering checks, both traffic directions, reflection rejection
  for ordinary v4 message types, legacy v3 rejection, and the XOR forgery regression.

- `session_test.cpp`: full handshake, lost INIT/ACK, duplicate control messages,
  reordering, restart replay, expired pending state, previous-session overlap,
  hint collisions, per-session reassembly, counter exhaustion, tampering and
  rejection of unsupported suites/nonempty DH. Uses a simulated clock.
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
  reflection, replay, counter exhaustion and CLI compatibility.
- `aead_bench.cpp`: optional allocation-reusing codec microbenchmark (MB/s):
  `g++ -std=c++17 -O3 -march=native tests/aead_bench.cpp -o /tmp/aead-bench && /tmp/aead-bench`.
  Results include header handling and authentication; they are not tunnel throughput.
