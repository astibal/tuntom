# tuntom switch protocol v1

The switch protocol connects tuntom link processes to a deliberately small
label switch. All processes connect to one Unix `SOCK_SEQPACKET` listener in
version 1. Filesystem permissions are the local security boundary.

The switch never handles tuntom credentials, UDP, encryption, IP routing or
TUN devices. Its data-plane operation is:

```text
(input port, top label) -> (output port, replacement top label)
```

## Frame

All integers use network byte order.

```text
offset  size                 field
0       1                    version = 1
1       1                    opcode
2       1                    flags = 0
3       1                    label_count
4       4                    total_length
8       8 * label_count      label stack, top first
...     remaining bytes      opaque nonempty payload
```

`label_count` is in the range 1..8. V1 preserves labels below the top label;
ordinary forwarding replaces only the top label. PUSH and POP operations are
not defined. `total_length` must equal the `SOCK_SEQPACKET` record length.

Opcodes:

```text
1  SWITCH   the receiver provides delivery
2  EXIT     the receiver exits the label network
```

A connection first sends one registration record. This setup record is separate
from the data frames above:

```text
offset  size       field
0       3          magic = "TTP"
3       1          version = 1
4       1          port ID length, 1..63
5       3          reserved = 0
8       variable   printable ASCII port ID
```

After registration, a tuntom instance sends only `SWITCH` to the switch. It assigns its configured
`--switch-label` to authenticated DATA received from its UDP peer.

The switch sends `SWITCH` to a destination tuntom port after a successful rule
lookup. That tuntom transmits the opaque payload through its authenticated V5
session. Labels are IPC metadata and do not change the V5 wire format.

Routes whose destination was declared with `--exit-port <port-id>` change the
opcode to `EXIT` before delivery. On a lookup miss, `--default-back=on` also
changes the opcode to `EXIT` and returns
the otherwise unchanged frame to its input port. A tuntom instance accepts
`EXIT` only with `--switch-exit-node`; it writes only that payload to its TUN.

```text
UDP DATA -> tuntom -> IPC SWITCH -> tuntom-switch

IPC SWITCH -> tuntom -> UDP DATA

IPC EXIT -> tuntom --switch-exit-node -> TUN

TUN -> tuntom --switch-exit-node -> UDP DATA
```

There is no fallback from `SWITCH` to TUN. Malformed frames, unsupported
opcodes and `EXIT` received without exit-node mode are dropped.

## Listener and static routes

V1 uses one listener. Connections identify themselves with a stable port ID;
a reconnect with the same ID replaces the previous connection:

```bash
tuntom-switch \
  --socket /run/tuntom/switch.sock \
  --exit-port internet \
  --route honeypot:17=proxy:83 \
  --route proxy:91=honeypot:44 \
  --default-back=off
```

The switch creates the listener with mode `0660`. Its group ownership is
inherited from the containing directory/process environment and should be
configured by the service manager; a setgid runtime directory such as
`root:tuntom` mode `2770` is recommended. The listener remains active as tuntom
instances connect and disconnect; adding a link does not restart the switch.
If the listener disappears, tuntom keeps its UDP control plane alive, drops DATA
fail-closed, and retries connection and registration once per second. The
switch may therefore be restarted without restarting established tunnels.

`tuntom-switch-adapter` consumes `EXIT`, writes its IP payload to a TUN and
caches the reverse label stack. TUN return packets use L4, then L3 lookup and
are emitted as `SWITCH`; cache misses are dropped. Cache mechanics are local to
the adapter and do not change this wire format.

Remote transports and dynamic control-plane updates are outside protocol v1.
