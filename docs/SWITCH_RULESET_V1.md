# Switch ruleset format 1

For full-stack matches, masks and integrated forwarding/rewriting, use
[format 2](SWITCH_RULESET_V2.md). Format 1 keeps the behavior documented here.

Both `tuntom-switch` and `tomtom-switch-mp` accept `--rules-file PATH` at startup
and support `rules check/load/show` on their existing Unix control socket.
`mk_switch.sh` and `mk_switch_mp.sh` recognize the format header in their
`--rules-file` seed, including files produced by `pre/up` hooks.

```text
format 1
serial 2026091301

exit internet* # Several exit ports may be connected.
exit local-exit
trunk backbone*

switch blocked*,* drop [id=blocked]
switch client*,* to internet*,1001 allow
switch internet*,1001 to client-42*,44 allow

label client-42*,17 to internet*, [1001, ...]
label internet*,1001 to client-42*, [44, ...]
```

The data path is:

```text
(input port, original top label)
    -> first matching label mapping -> candidate output ports and label stack
    -> first matching switch policy for each candidate
    -> ECMP among allowed, connected candidates -> SWITCH or EXIT delivery
```

## Lexical rules and declarations

Each directive occupies one line. `#` begins a comment anywhere on the line;
the comment extends to its end. Blank lines and spaces/tabs around tokens are
ignored. CRLF and a final line without a newline are accepted. Tokens are
printable ASCII. There is no shell expansion, quoting, escaping or inclusion.
Commas and brackets are punctuation, so spaces around them are optional.

`format 1` must be the first non-comment directive, followed by `serial N`.
Each header appears exactly once. The serial is an unsigned decimal 64-bit
integer, including zero; `YYYYMMDDNN` is a convenient manual convention.
Numbers are decimal, including those with leading zeros. Labels use the full
unsigned 64-bit range. Unknown directives/options fail validation.

```text
exit PORT_PATTERN
trunk PORT_PATTERN
```

Declarations can be repeated for different ports, and support patterns. Exit
ports receive `EXIT` frames. Trunk declarations affect MP scheduling and retain
`SWITCH` delivery. ST accepts trunk declarations for a shared format. Overlapping
exit and trunk patterns are an error. Role changes apply to already registered
ports when a new ruleset is activated.

A port pattern is an exact name or a non-empty literal prefix followed by one `*`.
Bare `*` is rejected in every explicit port selector, including `exit`, `trunk`,
`switch`, and `label`, to prevent accidental selection of all ports.
Matching is case-sensitive. `edge*` includes `edge`, `edge1`, and
`edge-backup`. Interior/repeated stars are invalid. Labels match an exact number
or `*`; numeric string patterns such as `12*` are not supported. Names are
1..63 characters and cannot contain whitespace, `#`, `,`, `[` or `]`.

## Ordered switching policy

```text
switch [SOURCE[,LABEL]] [to DESTINATION[,LABEL]] allow|drop [id=NAME]
switch [SOURCE[,LABEL]] [to DESTINATION[,LABEL]] allow|drop bidir [id=NAME]
```

In this grammar, the source and destination selectors are optional; the
`[id=NAME]` suffix is an optional **literal bracketed option block**. `bidir`
is a bare keyword after the action and before that option block.
Omitted selectors still match everything: `switch allow`, `switch H42* drop`,
and `switch to inet* allow` remain valid. Only an explicit bare port `*` is
forbidden. Export keeps omitted selectors omitted, so `show` remains reloadable.
An endpoint without `,LABEL` has label `*`; label wildcards remain valid.

```text
switch blocked*,* drop
switch to internet*,* allow
switch client*,17 to proxy*,83 allow [id=relay]
switch drop
```

For each candidate, policies are evaluated in **file order**. The first match
terminates evaluation. A more specific later policy never overrides an earlier
match. No match means drop, so the last `switch drop` is optional. A standalone
`drop` is accepted as an alias for `switch drop`.

Input selectors match the original port/top label; destination selectors match
the candidate port and the rewritten top label. Disallowed candidates are
excluded **before** ECMP. `allow` alone does not create a destination mapping.

IDs are optional, unique across switch and label statements, and retained in
exports. They currently identify configuration statements, not separate counters.

### Bidirectional policy shorthand

```text
switch H42*,17 to exit0,99 allow bidir
```

The parser expands this into two adjacent policies:

```text
switch H42*,17 to exit0,99 allow
switch exit0,99 to H42*,17 allow
```

The reverse is inserted immediately after the original, before the next source
statement. It swaps both port and label selectors, preserving wildcard matches
and the action. Omitted selectors retain their match-all semantics when swapped
and remain omitted in exports. The same option works with `drop`;
identical forward/reverse selectors still produce two policies.
Earlier policies keep their precedence in both directions.

With `bidir [id=path]`, the original retains `path` and the reverse receives
`path.reverse`. A collision with any explicit or generated ID is a validation
error. Both expanded policies count toward the 4096-statement limit. `show`
exports the two ordinary policies without `bidir`; loading that export or the
equivalent shorthand with the same serial is a no-op.

`bidir` applies only to `switch`. It does not create or reverse `label` mappings;
configure the label mapping for each direction explicitly. Capture accepts the
option syntactically but remains unsupported in this phase.

## Ordered label mappings

```text
label SOURCE[,LABEL] to DESTINATION, [STACK_TEMPLATE] [id=NAME]
```

The first matching mapping in file order is final, including when its target is
disconnected, a `keep` position does not exist, or all candidates are denied.
There is no fallback to a later mapping and no chaining of rewrites. A mapping
miss drops the packet. Port wildcards work for both ingress and output groups.

| Stack template | Effect |
| --- | --- |
| `[keep, ...]` | Preserve all labels |
| `[1001, ...]` | Replace the top label, preserve the remaining labels |
| `[1001]` | Replace the entire stack with one label |
| `[1001, keep, ...]` | Replace the top label and retain the original second and remaining labels |
| `[1001, 1002]` | Replace the entire stack with these two labels |

Each template item describes the corresponding output position. `keep` reads
the same position in the original stack; a missing position drops the packet.
A final `...` copies original labels after the explicit template positions.
There must be 1..8 explicit items; the resulting frame retains 1..8 labels and
its payload unchanged. A rewrite exceeding the implementation's frame buffer
is dropped. The data frame's header and total length are updated together.

ECMP retains the existing symmetric flow hash and rendezvous selection. Each
packet goes to one allowed, connected output. Membership changes do not change
the configuration serial. Locally connected ports may include the ingress
itself when both mapping and policy permit it. Connectivity still means local
IPC registration, not remote tunnel health.

The switches expose `policy_drops`, `rewrite_drops`, and the existing route-miss,
target-disconnected and ECMP counters. In format-1 mode stats additionally show
`ruleset_format=1` and `ruleset_serial=N`.

## Capture syntax reserved for a later phase

```text
switch capture
switch blocked*,* capture
switch client*,* to internet*,* capture debug [id=diagnostic]
```

`capture` is parsed as an action with an optional target name and option block,
but validation currently rejects it with `capture is not supported yet`.
Target declarations, FIFO creation, capture data formats, sampling and readers
are not implemented. The reserved action will copy/observe and continue to the
next policy; only `allow` and `drop` terminate policy evaluation.

## Control commands, versions and persistence

```bash
# Explicit control socket; same convention as the existing 'show stats'.
tuntomctl /run/tuntom/fabric.control rules check switch.rules
tuntomctl /run/tuntom/fabric.control rules load switch.rules
tuntomctl /run/tuntom/fabric.control rules show > saved.rules

# Default control socket is /run/tuntom/switch.control.
tuntomctl rules show > switch.rules
tuntomctl rules check switch.rules
tuntomctl rules load switch.rules
cat switch.rules | tuntomctl rules load -
```

The client sends the file contents, never a path for the server to open. `check`
performs the same parsing, serial checks and plan preparation as `load`, without
publishing. Exact output ports need not be registered during validation.

- A larger serial may replace the active configuration.
- An equal serial with equal canonical content is a successful no-op.
- An equal serial with different content, or a lower serial, is rejected.
- Rollback uses old rule contents with a new, higher serial.

Canonical equality includes all statement order and IDs, but excludes comments,
whitespace, number spelling and optional-selector shorthand. `show` emits only
a valid canonical configuration to stdout, including format and serial. Errors
go to stderr with a failing exit status. An export is one consistent snapshot
and preserves patterns rather than expanding currently connected ports.
`show -> load` therefore succeeds without a serial change. Successful check/load
responses state `checked`, `applied` or `unchanged` and the serial.

All preparation precedes publication. On ST, compiled ingress tables and the
ruleset are swapped in the main loop. On MP, the next immutable plan is published
with all workers parked at the existing barrier. A ruleset change drains old
userspace link queues and pending TX frames, counting `reconfiguration_drops`;
a scheduling-only change continues to preserve surviving queues. Frames already
submitted to an IPC transport/kernel cannot be recalled. Unread ingress frames
use the rules active when RX consumes them. The ACK is sent after publication.
The internal MP plan version remains separate from the manual configuration serial.

Runtime load does not overwrite the startup file. Restart loads the file named
by `--rules-file` again. Save an export/update the startup file explicitly for
persistence; managed helpers retain their staged startup copy under the instance
state directory.

A fresh switch without legacy routing options starts with `format 1`, serial 0,
and implicit drop. Existing `--route`, `--exit-port`, `--trunk-port` and
`--default-back` retain their legacy behavior. A format-1 file cannot be mixed
with legacy routing options. Legacy-only configurations have no format-1 export;
`rules show` reports this explicitly until a format-1 ruleset is loaded. Helper
scripts still accept the old line-based files and now permit inline comments in
them as well.

## Control wire format

The existing socket remains `AF_UNIX/SOCK_SEQPACKET`, mode 0660. The original
single-record `show stats` request/response is unchanged. New requests use:

```text
record 1: rules check LENGTH
       or rules load LENGTH
       or rules show 0
records 2..N: exactly LENGTH bytes of configuration
```

Lengths are decimal byte counts. Bodies and canonical exports are limited to
1 MiB and configurations to 4096 statements. Each body record is at most 16384
bytes; chunks can end anywhere, including within a line. A short/incomplete
transfer does not activate anything; records exceeding the declared length fail.
The server responds:

```text
record 1: OK LENGTH\n       or ERROR LENGTH\n
records 2..N: exactly LENGTH bytes of export, success message or error text
```

The server holds at most eight active control clients, services bounded batches
of records without blocking I/O, and expires incomplete requests/stalled responses
after five seconds. The CLI uses bounded socket waits and buffers a complete
response before printing it. If the connection fails after a load was delivered,
the client reports an uncertain result: inspect `rules show` before retrying.
Serial comparison and publication are serialized in the switch main loop.

MP preparation additionally limits work to 1,048,576 compiled mapping/candidate
port pairs per plan. Preparation failure leaves the active plan usable; a new registration that
exceeds this bound is rejected without stopping the switch. Ingress
patterns and shadowed mappings are compiled outside the packet path; exact label
lookup plus a wildcard fallback preserves first-match semantics. MP output groups
are resolved at plan preparation. Numeric/output policy checks retain file order.
