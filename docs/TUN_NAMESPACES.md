# TUN file descriptors across network namespaces

Tuntom processes can keep their UDP or switch IPC sockets in one network
namespace while placing the kernel-facing TUN interface in another. Linux binds
a TUN interface to the network namespace in which `TUNSETIFF` runs; the open
file descriptor can subsequently be passed to and used by a process in a
different namespace.

```text
 process / transport namespace             packet-processing namespace
 +-----------------------------+           +--------------------------+
 | tuntom or adapter           |           | TUN interface            |
 |                             |           | routes / proxy / VRF     |
 | read/write(received TUN fd) |<----------| kernel network stack     |
 +-----------------------------+ SCM_RIGHTS+--------------------------+
```

Packet bytes do not pass through the Unix control socket. `SCM_RIGHTS` transfers
the open descriptor once; normal TUN reads and writes continue directly against
the kernel device.

## Integrated provider

`--tun-netns TARGET` uses a synchronous, short-lived provider child:

```text
 parent
   socketpair(AF_UNIX, SOCK_SEQPACKET)
   fork
     child: setns(TARGET, CLONE_NEWNET)
     child: open /dev/net/tun and TUNSETIFF
     child: set MTU and optionally bring the link up
     child: send fd(s) and versioned metadata with SCM_RIGHTS
     child: exit
   parent: validate interface name and MTU
   parent: continue with the received fd(s)
```

The fork happens during startup, before tuntom starts its logging/worker thread
and before its normal privilege drop. There is no long-lived privileged control
process and no privileged command protocol. The parent waits for the child, so
a namespace, TUN, metadata, or descriptor-transfer failure aborts startup.

The exit and divert adapters follow the same privilege lifecycle as the main
tunnel: they open their TUN, IPC, control and shared-flow descriptors first,
then drop permanently to `tuntom:tuntom`, disable core dumps and set
`NoNewPrivs`. Unix-socket directories and files reopened at runtime must
therefore be accessible to that account. Split divert workers retain only their
already-open multiqueue descriptors; disconnect/reconnect queue detach and
attach do not require a resident privileged helper.

`tuntom`, `tuntom-switch-adapter` and `tuntom-divert-adapter` accept
`--user NAME` and `--group NAME`. Each defaults to `tuntom`, preserving existing
commands. These options choose the permanent runtime identity after setup; they
do not affect which identity performs the initial TUN creation. The named user
and group must exist before startup.

`TARGET` accepts:

| Form | Resolution |
| --- | --- |
| `edge` | `/run/netns/edge` |
| `/run/netns/edge` | explicit namespace path |
| `pid:1234` | `/proc/1234/ns/net` |

Creating a TUN requires `CAP_NET_ADMIN` in the target network namespace.
Entering it requires the applicable `setns(2)` permission. The target namespace
must remain alive while the provider opens it; afterward the interface and its
open fd keep the network-namespace object alive.

## Commands

### UDP tunnel endpoint

```bash
tuntom client 42 ut42c server.example --tun-netns edge --tun-up
tuntom server 42 ut42s --tun-netns pid:1234 --tun-up
```

`--tun-up` belongs only to the integrated tuntom provider. Without it, existing
deployment tooling can configure link state after creation.

### Switch exit adapter

```bash
tuntom-switch-adapter ex0 \
  --switch-socket /run/tuntom/switch.sock \
  --switch-port-id exit \
  --tun-netns exit-ns
```

The exit adapter always sets its TUN MTU and brings the link up, matching its
original current-namespace behavior.

### Divert/VIA adapter

```bash
tuntom-divert-adapter di0 do0 \
  --switch-socket /run/tuntom/switch.sock \
  --via-instance smithproxy#0 \
  --tun-netns proxy-ns
```

Both active TUN sides are created by one child in the same target namespace and
returned to the adapter. In `--side in` or `--side out` mode only that side is
created. Shared-flow workers retain `IFF_MULTI_QUEUE` and their existing
`TUNSETQUEUE` attach/detach lifecycle. Every worker serving one proxy instance
must name the same namespace and TUN pair.

```text
 switch/relay IPC namespace                 proxy namespace

 divert-in IPC -> adapter -> write(di0 fd) -> di0 --+
                                                    | proxy/routing
 divert-out IPC <- adapter <- read(do0 fd)  <- do0 <-+
```

Switch IPC remains pathname Unix IPC (or its negotiated mmap transport); it is
not an Ethernet trunk. The adapter removes/adds tuntom opcodes, label stacks and
VIA/divert flow context around the raw L3 packet carried by each TUN.

## Standalone provider socket

`tuntom-tun-helper` supports independently supervised processes:

```bash
tuntom-tun-helper /run/tuntom/ut42c.fd ut42c --netns edge --mtu 1500 --up &
tuntom client 42 ut42c server.example --tun-socket /run/tuntom/ut42c.fd
```

The helper creates a mode-`0600` pathname `SOCK_SEQPACKET` listener, serves one
client, removes the socket path and exits. The protocol carries exactly one fd
plus a magic value, protocol version, interface name and MTU. Tuntom rejects a
name or MTU mismatch. A shared or bind-mounted filesystem path must be visible
to both processes; network namespaces do not imply shared mount namespaces.

The standalone form is intentionally available only to the tunnel endpoint.
Adapters use the integrated provider because they own one or two coordinated
TUN queues and already have a single startup lifecycle.

## Addressless operation

Neither TUN nor the packet-processing namespace inherently needs a local IP
address. Point-to-point L3 processing can use routes directly to an interface:

```bash
ip -n proxy-ns route add 10.20.0.0/16 dev di0
ip -n proxy-ns route add 10.30.0.0/16 dev do0
```

Whether forwarding, policy routing, proxy interception, `rp_filter`, redirects,
or VRFs need configuration depends on the service chain. The provider only
creates the requested interface(s), sets MTU/link state as documented, and
returns descriptors; it does not install addresses, routes or sysctls.

## Compatibility

All namespace options are additive. Omitting them preserves the original CLI
and code path:

```text
no --tun-netns / --tun-socket
  -> open /dev/net/tun in the current namespace
  -> TUNSETIFF with the original flags
  -> original MTU, link-state and multiqueue behavior
```

Existing positional interface names remain mandatory. Relay-only and non-exit
switch tunnel modes still create no TUN and reject incompatible provider
options. `--tun-netns` and `--tun-socket` are mutually exclusive.

## Regression coverage

The privileged integration tests use real network namespaces and `/dev/net/tun`:

- `tun_netns_integration_test`: encrypted tuntom traffic, fd transfer, child
  exit, UID/GID privilege drop and `NoNewPrivs`;
- `adapter_netns_integration_test`: verified adapter UID/GID drop and
  `NoNewPrivs`, exit traffic through both the original current-namespace path
  and the namespace provider, paired divert traffic through two addressless
  TUNs, split-side multiqueue detach/reconnect/reattach followed by fresh
  traffic, and clean failure for a missing namespace.

Both tests are always registered with CTest. A host without the required Linux
capabilities or `/dev/net/tun` reports CTest skip code 77 rather than a false
pass. Existing adapter, relay, reconnect, classifier and split-side integration
tests continue exercising the unchanged commands without namespace options.
