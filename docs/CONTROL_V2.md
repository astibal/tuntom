# Routed CONTROL v2

CONTROL v1 and `tuntomctl remote SOCKET --- COMMAND` remain compatible:
that syntax always controls the remote tunnel. Routed CONTROL v2 is supported
by tunnels, both switch implementations, relay endpoints, exit adapters and
divert adapters. Every origin, relay and target must explicitly enable network
CONTROL with `--allow-control-trusted`, or `--allow-control-all` for debugging
at the operator's own risk. Without either flag, incoming, outgoing and transit
CONTROL are all disabled. Loading keys alone does not enable the stack.
Trusted-mode destinations verify pinned authorities and capability grants;
trusted-mode relays may forward without holding any authority keys. Local Unix
control socket diagnostics remain available. See [CONTROL_AUTH.md](CONTROL_AUTH.md).

Examples:

```sh
# A locally attached tunnel:
tuntomctl switch /run/tuntom/switch.control --port tunnel42 --- show stats
# Its remote tunnel:
tuntomctl switch /run/tuntom/switch.control --port tunnel42 --peer --- show stats
# A port attached to the remote relay listener:
tuntomctl switch /run/tuntom/switch.control --port tunnel42 \
  --peer-port 'proxy-in0~via:c:smithproxy#0' --- classifier load classifier.conf
# Same target starting at the local tunnel:
tuntomctl remote /run/tuntom/tunnel.control \
  --peer-port 'proxy-in0~via:c:smithproxy#0' --- show stats
# Retrieve retained result (same originating process, same target):
tuntomctl switch /run/tuntom/switch.control --port tunnel42 \
  --peer-port 'proxy-in0~via:c:smithproxy#0' --- request status REQUEST_ID
# Bounded discovery snapshot:
tuntomctl switch /run/tuntom/switch.control --- discover
# Same discovery snapshot rendered as an ASCII tree:
tuntomctl switch /run/tuntom/switch.control --- discover tree
```

`discover tree` uses the same ASCII branches as `--port-tree`. Port labels
are exact registration names (decoded from discovery trace escapes), usable
with `--port` and `--peer-port`; `peer` denotes the `--peer` hop. Component and
`ALT_PATH`/`NO_RESPONSE` annotations are not part of the port name. Targeted
discovery retains the selected route prefix in the tree. Arbitrary deeper
paths are displayed but still require future N-hop CLI support.

Discovery is exposed only through `tuntomctl switch ... --- discover`;
`remote ... --- discover` is rejected by the CLI. Protocol-level discovery
remains available to all supporting components.

Files are read locally and sent as opaque bytes. `--peer-port` implies `--peer`.
`--remote-retries` and `--remote-wait` precede `---`. Exit statuses: 0 requires
successful execution and terminal confirmation, 255 means rejection, 1 means
failure (including an uncertain outcome after timeout). State is in memory only.
The local `--port-list` / `--port-tree` snapshots are unchanged; integration of
DISCOVER into those views and arbitrary N-hop CLI addressing is a later phase.

## Wire format

UDP uses authenticated V5 packet type 14. IPC carries the same logical CONTROL
payload directly, including over negotiated inline/mmap IPC. Integers are big
endian. No C++ structure layout is serialized.

```
offset bytes field
 0       1   version = 2
 1       1   kind
 2       1   transaction state (0 for discovery/routing errors)
 3       1   remaining hop budget (1..16)
 4      16   sender-selected request ID
20      16   originating process instance ID
36       4   block/acknowledged offset
40       4   total body length
44       2   command length
46       2   destination stack byte length
48       2   reply stack byte length
50       2   reserved = 0
52       *   destination stack, reply stack, command, body block
```

Kinds: 1 PUT, 2 STATUS, 3 REPLY, 4 FINISH, 5 CONFIRMED, 6 DISCOVER,
7 FOUND, 8 ALT_PATH, 9 ROUTE_ERROR. Transaction states retain v1 values:
1 RECEIVING, 2 READY, 3 RUNNING, 4 SUCCEEDED, 5 FAILED, 6 REJECTED,
7 EXPIRED, 8 NOT_FOUND. The request namespace is `(origin, request ID)`.

A stack consists of `type:u8 length:u8 value[length]` entries, top first:

| Type | Value |
| --- | --- |
| 1 PEER | empty; the tunnel peer |
| 2 PORT | 1..63 printable non-space ASCII bytes; local port name |
| 3 LINK | 16 nonzero bytes; opaque connection token |

An empty destination delivers locally. Otherwise resolve/pop/send; failure
returns `target_not_found`. Each receiver pushes its actual incoming connection
LINK onto `reply_path`. Replies use the received reply stack as destination and
start with an empty reply stack. This also supplies a connection-bound outward
path, used for transaction retries after the first reply. Tokens change on IPC
reconnect / tunnel transmit generation changes. Broken response paths are dropped
without recursive errors. Full frames exceeding a link's limit return
`path_mtu_exceeded`; routers do not fragment frames. Request and result bodies
use the existing acknowledged block transfer (currently conservative 64-byte
blocks for routed transactions). Long paths/commands can still exceed path MTU.

## Discovery and limits

An allowed node records `(origin, request ID)` before replying FOUND and sending
DISCOVER to every other active connection. Duplicate requests produce ALT_PATH
without forwarding. FOUND/ALT_PATH body is a TSV line:
`instance-ID<TAB>component<TAB>capabilities<LF>`; command carries the traversal
trace, with `%`, `/` and tab escaped inside names. One process has one instance
ID even when attached through multiple ports. Disabled nodes stay silent.

Discovery collects at most 1024 replies / 1 MiB of result text for two seconds. It is a time-bounded snapshot,
not proof that every branch answered; silence does not distinguish disabled,
missing, congested or old peers. Routing retains at most 256 discovery IDs for
30 seconds, allows at most 32 duplicate replies per ID per second, and caps
paths at 16 entries and 1024 encoded bytes per stack. All frames retain the hop
budget. Transaction admission is bounded to eight origins, each with v1's
128-entry result history and 16 MiB accounting; at most eight locally initiated
requests are outstanding. Input upload expires after two seconds without
progress or ten seconds total. Only one changing routed upload is admitted at a
time; cheap reads can proceed alongside it. Policy classification is shared
with the local ControlDispatcher.
