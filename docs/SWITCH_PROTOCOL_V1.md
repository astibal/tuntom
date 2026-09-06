# tuntom switch protocol v1

The switch protocol connects one tuntom link process to a deliberately small
label switch. It is local IPC over Unix `SOCK_SEQPACKET` in version 1. Filesystem
permissions are the local security boundary.

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

A tuntom instance sends only `SWITCH` to the switch. It assigns its configured
`--switch-label` to authenticated DATA received from its UDP peer.

The switch sends `SWITCH` to a destination tuntom port after a successful rule
lookup. That tuntom transmits the opaque payload through its authenticated V5
session. Labels are IPC metadata and do not change the V5 wire format.

On a lookup miss, `--default-back=on` changes the opcode to `EXIT` and returns
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

## Static switch configuration

V1 uses a separate named listening socket for every port, avoiding a data-plane
registration message:

```bash
tuntom-switch \
  --port honeypot=/run/tuntom/honeypot.sock \
  --port proxy=/run/tuntom/proxy.sock \
  --route honeypot:17=proxy:83 \
  --route proxy:91=honeypot:44 \
  --default-back=off
```

The switch creates sockets with mode `0660`. Their group ownership is inherited
from the containing directory/process environment and should be configured by
the service manager. A port accepts one active connection; a new connection
replaces the previous one.

Remote transports, dynamic control-plane updates, a native switch exit adapter
and exit-flow caching are outside protocol v1.
