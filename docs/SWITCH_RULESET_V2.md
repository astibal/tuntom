# Switch ruleset format 2

Format 2 combines forwarding and label rewriting in `switch`, matches the full
stack, and adds ranges, bitmasks and non-decimal literals. Both switch
implementations accept it at startup and through `rules check/load/show`.
It uses the existing IPC frame (1..8 uint64 labels), without changing UDP V5.
Ruleset format 2 and IPC transport V2 are independent versions.

```text
format 2
serial 2026091403
exit inet*
switch H42*, [17, ...] to inet*, [99, ...] allow bidir
```

This forwards `[17, x, ...]` to one connected `inet*` port with `[99, x, ...]`.
The generated reverse rule restores the first label to 17. `exit inet*`
selects `EXIT` delivery; other ports receive `SWITCH`.

## Match elements and literals

| Element | Predicate on one label `L` |
| --- | --- |
| `V` | `L == V` |
| `*` | Any value |
| `<A,B>` | `A <= L && L <= B` (inclusive; `A > B` is invalid) |
| `&M` | `(L & M) != 0` (any selected bit, not all selected bits) |

`&24` accepts 8, 16 and 24, but not 0 or 32. `&0` never matches.
Values, interval bounds and masks accept these forms:

| Literal | uint64 value |
| --- | --- |
| `255` | Decimal 255 |
| `0xff`, `0XFF` | Hexadecimal 255 |
| `b1000`, `0b1000` | Binary 8 (`B`/`0B` also accepted) |
| `"ABCD"` | `0x4142434400000000` |
| `"ABCDEFGH"` | `0x4142434445464748` |
| `""` | Zero |

Strings are at most **8 bytes**, packed from the most significant byte and
padded on the right with zero bytes. UTF-8 characters count by encoded bytes,
not code points. Quotes protect spaces, `#`, commas and match punctuation.
Only `\"` and `\\` escapes are supported; literal control bytes are rejected.
Unquoted `#` starts a comment. Values outside uint64 and invalid digits fail
validation. Headers `format` and `serial` remain unsigned decimal integers.
Exports normalize literals to decimal, single-value ranges to exact values,
and scalar stack shorthand to bracketed form. Equal values in different
notations have equal canonical configuration content.

## Stack matches and output templates

```text
[17, 99]          exactly two labels
[17, 99, ...]     at least two labels, arbitrary remaining labels
[42, &0x10, ...]  first label 42, second with bit 16 set
17                shorthand for [17], exactly one label
*                 shorthand for [*], exactly one label
```

All explicit elements must match (AND). There must be 1..8 elements; `...`
may appear only at the end and accepts zero additional labels.
An omitted stack selector accepts any valid stack, equivalent to `[*, ...]`.

For `allow`, the destination stack is a **rewrite template**:

| Template | Input `[17,42,8]` becomes |
| --- | --- |
| `[99, ...]` | `[99,42,8]` |
| `99` or `[99]` | `[99]` |
| `[*, 99, ...]` | `[17,99,8]` |
| `[*, *, 7]` | `[17,42,7]` |
| Omitted | `[17,42,8]` |

`*` copies the original value at the same position. Missing source positions
drop the frame. `...` copies original positions after the explicit template
items; it does not push or pop a variable-length prefix. Literals can create
new positions. Ranges and bitmasks cannot appear in an `allow` output template.
The payload stays unchanged. A result exceeding the frame buffer is dropped.

## Ordered forwarding and drops

```text
switch [SOURCE[,MATCH]] [to DESTINATION[,TEMPLATE]] allow [bidir] [id=NAME]
switch [SOURCE[,MATCH]] [to DESTINATION[,MATCH]] drop [bidir] [id=NAME]
```

Brackets in the grammar above denote optional arguments; actual stack matches
use their own literal brackets. Omitted ports retain match-all semantics.
A bracketed stack can appear without a port:

```text
switch [42, &16, ...] to inet*, [42, *, ...] allow
switch H42*, [17, ...] drop
switch to blocked* drop
switch drop
```

Port patterns are exact names or non-empty prefixes ending in one `*`.
An explicit bare port `*` is rejected. Omitted ports and `*` label elements
remain valid. `H42*` also matches `H42` and `H420`.

Evaluation preserves file order:

1. Scan rules whose source port and original stack match. A matching drop with
   no destination restriction terminates immediately.
2. The first matching `allow` fixes the output group and rewrite template.
   There is no fallback to a later allow if the destination is disconnected,
   the rewrite fails, or all candidates are denied.
3. Earlier destination-scoped drops filter that group's candidates. Their
   source match sees the original stack; destination match sees the proposed
   rewritten stack. Rules after the selected allow cannot change its decision.
4. ECMP selects one surviving connected candidate. If none remains, drop.

```text
switch H42* to inet-block*, [99, ...] drop
switch H42*, [17, ...] to inet*, [99, ...] allow
```

The first statement removes `inet-block*` before ECMP. A more specific later
allow never overrides an earlier matching allow. Without an allow, drop.
`switch allow` forwards with an unchanged stack among all connected ports,
including the ingress. Use bounded groups for ordinary configurations.
No origin-to-port relationship is inferred.
`exit` and `trunk` retain their format-1 meanings. `label` is rejected in format
2; use `switch` for forwarding and rewriting. `capture` remains unsupported.

## Bidirectional shorthand

```text
switch H42*, [17, ...] to inet*, [99, ...] allow bidir [id=path]
```

Expands immediately into:

```text
switch H42*, [17, ...] to inet*, [99, ...] allow [id=path]
switch inet*, [99, ...] to H42*, [17, ...] allow [id=path.reverse]
```

The reverse input matches values produced by the forward template. Preserved
positions retain the original predicate, including ranges and masks:

```text
switch H42*, [42, &16, ...] to inet*, [99, *, ...] allow bidir
# Reverse:
switch inet*, [99, &16, ...] to H42*, [42, *, ...] allow
```

The reverse rewrite restores exact source values and copies preserved
non-exact positions. A wildcard, range or mask whose value was overwritten or
removed makes `bidir` invalid; write two explicit rules instead.
For lossy `[17, ...] -> [99]`, the reverse is `[99] -> [17]`: discarded trailing
labels are not recovered. Reverse `...` is present only when both original
match and template have it. This is a deterministic shorthand, not flow-state
tracking or a general guarantee of lossless inversion.

For `drop bidir`, only source/destination matches are swapped. No rewrite
occurs. Earlier rules keep precedence. IDs and expanded statements obey the
same collision checks and 4096-statement limit as format 1.

## Reload and compatibility

Format 1 retains its original top-label match and independent `switch`/`label`
lists. Format-1 `17` matches the top of a longer stack; format-2 `17` matches
exactly one label. Use a higher serial when migrating. Changing format under
the same serial is rejected. `show` exports the active format and expanded
ordinary rules; `check` and `load` use the same parser. Statistics report
`ruleset_format=1` or `2`.
The control protocol, transactional queue handling, limits and restart
persistence follow [the format-1 guide](SWITCH_RULESET_V1.md#control-commands-versions-and-persistence).
MP resolves destination members during plan preparation. Ingress port
predicates are compiled per port; stack rules retain their order. The format-1
top-label lookup keeps its existing fast path.

## Label scope

The adapter caches the complete received stack for replies. UDP V5 still
transports only the payload; a tuntom receiver assigns its configured single
ingress label or a stack selected by the separate [L3/L4 classifier](PACKET_CLASSIFIER.md).
Switch rules do not add tunnel stack transport or adapter QoS actions.
