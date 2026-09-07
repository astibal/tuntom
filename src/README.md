# C++ implementation

The local CMake build starts at `main.cpp`. Every header declares its own
dependencies and can be included independently. Implementation lives in the
`tuntom` namespace; free functions and shared variables are `inline` for use
from multiple translation units.

| Header | Responsibility |
| --- | --- |
| `common.hpp` | Constants and logging |
| `privileges.hpp` | Process hardening and privilege drop |
| `packet.hpp` | Packets, options, statistics and byte dumps |
| `ascon.hpp` | Ascon permutation, MAC and key derivation |
| `wire.hpp` | Wire integers and secret parsing |
| `replay.hpp` | Sequence generation and replay window |
| `protocol.hpp` | Protocol interface and compact v5 wire codec |
| `tun_device.hpp` | Linux TUN device |
| `udp_endpoint.hpp` | UDP sockets and peer handling |
| `reassembly.hpp` | Fragment reassembly |
| `session.hpp` | V5 handshake and session lifecycle |
| `ip.hpp` | IP checksums and hop compensation |
| `fragmentation.hpp` | Fragment sizing and probe state |
| `tunnel.hpp` | Event loop, forwarding, RTT, PMTUD and stats |
| `cli.hpp` | Usage and argument parsing |
| `switch_client.hpp` | Tuntom-side Unix switch connection |
| `ipc/switch_protocol.hpp` | Shared `SWITCH` / `EXIT` frame codec |
| `control_socket.hpp` | Shared local `show stats` control server |
| `adaptive_polling.hpp` | Shared overload detection, batching policy and event-loop metrics |
| `control/main.cpp` | `tuntomctl` client executable |
| `switch/main.cpp` | Standalone label-switch executable |
| `adapter/ip_flow.hpp` | Safe IPv4/IPv6 L3 and TCP/UDP tuple parsing |
| `adapter/lru_cache.hpp` | Capacity and idle-time bounded LRU cache |
| `adapter/exit_adapter.hpp` | Reverse L3/L4 label learning and lookup |
| `adapter/main.cpp` | Standalone TUN exit-adapter executable |

`../mk_tunnel.sh` sends this directory as a tar stream over SSH, compiles
`main.cpp` remotely and removes the temporary sources on exit. No generated
source file or custom include processing is needed.

Packet-forwarding components must use nonblocking descriptors and the shared
`AdaptivePolling` policy: one fair round normally, confirmed-backlog batching,
round-robin source selection and a bounded processing slice with control traffic
handled first. New I/O loops should reuse this class and expose its standard
metrics rather than introducing an unbounded drain loop or one-packet-per-poll
bottleneck. Output backpressure is a drop/queueing condition, not a reason to
disconnect an otherwise healthy socket.

PFS implementation: `x25519.hpp` wraps the pinned `vendor/x25519.hpp` extraction;
`akdf.hpp` implements [AKDF v1](../docs/AKDF_V1.md) extract/expand; `secret.hpp` owns wiping
helpers. Suite 2 with Ascon encryption, X25519 PFS and periodic DH rekey is the
default. `--crypto-auth-only` selects plaintext suite 0. See
`docs/PROTOCOL_V5.md` for exact derivation and security assumptions.
