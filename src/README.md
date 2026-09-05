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
| `protocol.hpp` | Protocol interface and v1/v2/v4 wire codecs |
| `tun_device.hpp` | Linux TUN device |
| `udp_endpoint.hpp` | UDP sockets and peer handling |
| `reassembly.hpp` | Fragment reassembly |
| `session.hpp` | V4 handshake and session lifecycle |
| `ip.hpp` | IP checksums and hop compensation |
| `fragmentation.hpp` | Fragment sizing and probe state |
| `tunnel.hpp` | Event loop, forwarding, RTT, PMTUD and stats |
| `cli.hpp` | Usage and argument parsing |

`../mk_tunnel.sh` sends this directory as a tar stream over SSH, compiles
`main.cpp` remotely and removes the temporary sources on exit. No generated
source file or custom include processing is needed.
