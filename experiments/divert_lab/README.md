# Divert: feasibility lab with a Linux router

The lab verifies inserting a service into the packet path through a label switch.
A real Linux router with two TUNs stands in for smithproxy. The TCP client and
server use real sockets; encrypted tuntom runs between the client and the switch.
Temporary Linux namespaces separate the network components.

```text
 namespace client           namespace core
 +------------------+       +----------------------+
 | TCP client A     |       | tuntom server        |
 |   |              |       |   | port edge=123    |
 | tc0 (TUN)        |       |   v                  |
 |   |              |       | divert switch        |
 | tuntom client ---+--UDP--+   |             ^    |
 +------------------+       +---+-------------+----+
                                |             |
 namespace router               v             |
 +------------------------------------------------+
 | divert-in (401)             divert-out (402)    |
 |        |                          ^            |
 |       di0 ----- Linux routing --> do0           |
 |                                                |
 | One process handles both adapter ports.        |
 | They share the flow-to-label mapping.           |
 +------------------------------------------------+

 switch -- EXIT --> exit adapter -- ex0 (TUN) -- Linux routing -- TCP server
                    namespace exit            lan0/lan1       namespace server
```

The server response travels through the exit adapter to the divert-out adapter,
through the router from `do0 -> di0`, and through the divert-in adapter and switch
back to the original port 123. After activation, the old TCP connection belongs
to `EXISTING` and bypasses the router.

## Running the lab

Requirements: Linux, `/dev/net/tun`, enabled unprivileged user/network namespaces,
`unshare`, `nsenter`, `ip`, `sysctl`, Python 3.9+, and a C++17 compiler.
The lab requires neither sudo nor changes to the host network configuration.

From the repository root:

```bash
bash experiments/divert_lab/build.sh /tmp/tuntom-divert-lab-build
python3 experiments/divert_lab/check.py --build /tmp/tuntom-divert-lab-build
python3 experiments/divert_lab/run.py \
  --build /tmp/tuntom-divert-lab-build \
  --output /tmp/divert-1500 --mtu 1500
python3 experiments/divert_lab/run.py \
  --build /tmp/tuntom-divert-lab-build \
  --output /tmp/divert-9000 --mtu 9000
```

The output directory must be new. It contains `RESULTS.md`, machine-readable
`result.json`, `switch.jsonl`, `adapter.jsonl`, and process logs. On failure,
`failure.txt` is also retained. The runner stops its processes and removes
namespaces, interfaces, and temporary sockets on exit. IPv6 is disabled only in
the lab namespaces because this experiment verifies IPv4.

### Rootless tuntom launcher

Production tuntom drops privileges to its runtime account after initialization.
A simple user namespace with a single mapped UID cannot make this transition.
`tuntom-userns` therefore compiles the real `src/main.cpp`, replacing only the
functions that switch users. Before creating a TUN or socket, it verifies that it
is running in a separate single-UID user namespace and outside the initial network
namespace. It retains hardening and remains root only inside the namespace.
Production files and the normal `tuntom` binary remain unchanged. The packet path,
TUN driver, UDP, encryption, and fragmentation are real.

## What is checked

1. A TCP connection using client port 40000 is established before divert activation.
2. `SIGUSR1` enables diversion of port `edge` before the switch's normal rules.
3. The original TCP connection continues transferring data, returns from the
   divert-in adapter as bypass, and sends no packets into the router TUNs.
4. New connections 40001 and 40002 traverse `divert-in -> di0 -> do0 -> divert-out`.
5. The first transfers 1 MiB in each direction, the second 128 KiB; all data is checked.
6. Both SYN and SYN/ACK lose exactly one TTL unit while crossing the router.
7. Normal rules match the original port and exact stack even though the physical
   ingress is `divert-out`.
8. The production exit adapter remembers the entire stack, including the DVRT
   block, and restores it on the server response. The divert-in adapter then
   restores the original stack for the client.
9. The server sees the original client IP and ports. The old connection continues
   working after new connections between the same IP pair have been processed.
10. At MTU 9000, the lab also checks that tuntom transport fragment counters are
    nonzero; the outer MTU remains 1500.

The separate `check.py` uses the real experimental switch and temporary Unix
sockets. It also checks different registration orders, port 123 reconnecting with
an existing service block, an unknown origin, and a malformed block length.
`unit.cpp` tests variable block lengths and positions, pretty printing, and TCP/UDP
phase boundaries using simulated time, without waiting for hours.

## Labels

The cookie is generated at runtime as three ASCII letters and passed to both the
switch and the divert adapter. The fourth byte contains `body label count + 48`;
the lower four bytes of the 64-bit label are zero.

```text
original stack: [17,42]

diversion:
  [17,42,"hTX4","DVRT",123,0,17,42]
                      \----------/
                       body of length 4:
                       origin, action, original stack...

after normal rewrite, toward the exit adapter:
  [99,42,"hTX4","DVRT",123,1,17,42]

return to client:
  [17,42]
```

| Body action | Meaning |
| --- | --- |
| 0 | The switch offers the packet to the divert-in adapter |
| 1 | The divert-out adapter continues through normal rules toward the destination; the exit adapter retains DVRT |
| 2 | The divert-in adapter returns the packet to the stable origin port with the original stack |
| 3 | The divert-in adapter bypasses EXISTING traffic; the switch restores the origin and runs normal rules without DVRT |

The body length varies with the original stack. The parser searches for its
`cookie + DVRT` pair, validates the length, and rejects duplicate blocks belonging
to it. Normal rules see the stack without the service block; after rewriting,
the block is appended again. Stable IDs are configured explicitly and are not
connection indices. The switch has no per-flow cache.

The lab uses the current production IPC with an eight-label limit so it can use
the unchanged exit adapter. This does not prescribe a fixed length for future
DVRT blocks. Increasing the total label limit is a separate production change.

## Scope and limitations

- The switch is a separate, simple prototype. Normal matching and rewriting use
  the production `SwitchRuleset` and `RulesProgram`; it implements one local divert,
  explicit ports, and an unambiguous rule output. It has no MP, ECMP, or hot reload.
- The divert adapter uses two real TUNs and the production `SwitchClient` over IPC v1.
  It shares exact directional flow keys. An unknown context or capacity overflow
  terminates the lab with an error; active label entries are not evicted.
- Admission follows the agreed TCP phases of 60 minutes/24 hours and UDP phases
  of 60 seconds/1 hour. In this lab, its timer starts when the divert adapter
  starts; switch activation follows establishment of the first TCP connection.
  Unit checks cover the time boundaries.
- The network scenario tests TCP over Linux forwarding. UDP admission has unit
  coverage. Fragmented inner IP packets, ICMP errors/PMTU, and IPv6 are outside
  the lab's scope. Fragmentation of the tuntom transport itself is covered by
  the jumbo run.
- The Linux router does not create a new TCP connection as smithproxy does.
  The result demonstrates diversion, real routing, and preservation of label
  context; TPROXY integration remains unverified.
- Both directions of a client flow must preserve their IP/port identity.
  Overlapping tenant address spaces are not supported in this lab.
- Remote divert adapters and preservation of return labels directly in tuntom
  are not implemented here: this scenario requires the existing exit adapter's
  return cache.
- Every packet is traced. Results demonstrate feasibility; they do not measure
  CPU use, throughput, or VM cost.

## Files

Verified runs: [MTU 1500](results/2026-09-15-mtu1500/RESULTS.md),
[MTU 9000](results/2026-09-15-mtu9000/RESULTS.md). Complete JSON results are stored
alongside the reports; large per-packet traces and process logs remain local
and are excluded from Git.

- `switch.cpp`: divert before rules, origin restoration, service return handling.
- `adapter.cpp`: two switch ports, two TUNs, and a shared flow table.
- `common.hpp`: variable DVRT block, pretty printing, limited IP parser, and admission.
- `tuntom_userns.cpp`: rootless launcher wrapping production tuntom.
- `run.py`: namespace setup/cleanup, real TCP transfers, and trace validation.
- `check.py`, `unit.cpp`: protocol and timing checks.

## VIA service mode

`run_integrated.py --via` exercises the production format-3 service path, automatic
cookies and paired instance registration. It retains the existing warmup scenario
by passing `--admission warmup`. Combine it with `--switch st` or `--switch mp`,
`--vrf`, and `--mtu 9000`. See [VIA commands](../../docs/VIA.md#validation).
