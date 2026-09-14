# Stateless ingress classifier

`tuntom` and `tuntom-switch-adapter` accept `--classifier-file <path>` to assign
an initial label stack from inner IPv4/IPv6 headers. The classifier shares the
adapter's IP/flow parser with reverse-route lookup and switch ECMP. It retains
no packets or flow entries; each eligible packet is classified independently.
Matching rules are parsed once at startup, with no per-packet allocation.
The adapter parses each connected TUN ingress packet once and shares the
`ParsedIpFlow` by const reference between reverse lookup and classification.
Tuntom uses the packet entry point, which parses once inside the classifier.

```text
tuntom:
  authenticated UDP DATA -> transport reassembly -> classifier -> SWITCH
  no match -> existing --switch-label

adapter:
  TUN -> L4 reverse cache -> L3 reverse cache -> classifier -> SWITCH
                                                  no match -> drop
```

A cache hit always wins, preserving the complete learned stack. The existing
L3 fallback applies even when a TCP/UDP tuple misses L4: a new connection
between an already cached IP pair can therefore inherit that pair's labels.
Only incoming `EXIT` packets teach the adapter reverse routes. Classification
does not teach either cache and does not apply to incoming switch frames.
Tuntom classifies UDP DATA just before delivery to its switch interface, after
authentication, transport reassembly and the existing processing/MTU checks.
Its TUN-to-UDP and switch-to-UDP paths are unchanged.

## Configuration

Classifier files are separate from switch rulesets. They require `format 1`
and contain ordered `classify` statements. The first matching statement assigns
the **entire** output stack; all its conditions are ANDed. No longest-prefix
or most-specific-rule override is applied. Omitted fields match any value.

```text
format 1
classify ip4 src 10.42.0.0/16 proto tcp dport 443 to ["H42", 99, 0x10]
classify proto udp dport 53 to ["H42", 99, b1000]
classify ip6 dst 2001:db8::/32 to ["H42", 17]
classify to ["H42", 99]
```

| Selector | Accepted values |
| --- | --- |
| `ip4`, `ip6` | IP family; omit for both |
| `src`, `dst` | Numeric IP, CIDR prefix or `*` |
| `proto` | `tcp`, `udp`, `icmp`, `icmp6`, decimal 0..255 or `*` |
| `sport`, `dport` | Decimal 0..65535, inclusive `<A,B>` or `*` |
| `to` | Complete stack of 1..8 literal uint64 labels; one scalar is `[scalar]` |

For example, `sport <1024,65535> dport 443` requires both port predicates.
Port conditions with an omitted protocol accept TCP and UDP only. An explicit
port `*` still requires a readable TCP/UDP tuple. Duplicate fields, conflicting
IP families, reversed ranges and out-of-range values are errors.

Label literals follow switch format 2: decimal, `0xff`, `b1000` (`0b1000` also
works), or strings up to 8 UTF-8 bytes. Strings occupy the high bytes and use a
zero suffix: `"ABCD" = 0x4142434400000000`. Quoted `#` is literal; elsewhere it
begins a comment. Unlike switch rewrite templates, classifier output has no
input stack to copy: `*` and `...` are rejected, as are label ranges and masks.

The last unconditional statement above is optional. Without a match, tuntom
keeps its required `--switch-label`; the adapter drops a packet missing both
reverse caches. Without `--classifier-file`, both retain their previous behavior.
An empty rule list (`format 1` only) also falls through. Limits are 1 MiB and
4096 rules. Invalid configuration fails startup before opening TUN/UDP devices.
Files are loaded at startup; changing them requires restarting that producer.
Switch `rules check/load/show` commands manage switch rules, not classifiers.

## Fragments and parsing limits

IPv4 options and IPv6 Hop-by-Hop, Routing, Destination Options, Fragment and
AH headers are traversed with bounds checks (up to eight IPv6 extension headers).
The parser reads TCP/UDP ports when the first four transport bytes are present;
it does not validate transport checksums or inspect application payloads.

First IP fragments can expose ports. Later fragments cannot match port
conditions; they can still match addresses and a known protocol. If a later
IPv6 fragment names another extension header, the final protocol is unavailable.
No IP fragment reassembly or classification cache is introduced. Include an
L3 rule if all fragments must get a common stack; a port-based rule alone can
classify the first fragment differently from the rest. UDP tunnel transport
fragments are a separate layer and are reassembled before classification.

Malformed or unsupported IP input is not classified, including by an
unconditional statement. The caller's existing fallback still applies.

## Startup and statistics

```bash
tuntom server 42 - \
  --switch-socket /run/tuntom/switch.sock --switch-port-id H42 \
  --switch-label 17 --classifier-file /etc/tuntom/ingress.classifier

./mk_adapter.sh exit0 \
  --switch-socket /run/tuntom/switch.sock --switch-port-id local-origin \
  --classifier-file /etc/tuntom/ingress.classifier
```

`mk_tunnel.sh` accepts `--client-classifier-file` and
`--server-classifier-file`, each requiring the corresponding switch attachment.
Paths refer to files already on the respective host; the helper does not copy
classifier files. Group members reuse their side's classifier configuration.
Prefer absolute paths. A [complete example](../examples/ingress.classifier) is
included; set origin labels for the deployment's actual return routes.

Both programs expose `classifier_enabled`, `classifier_rules`,
`classifier_hits`, `classifier_misses` and `classifier_parse_errors` through
their existing statistics interface. Hits count selected rules, before IPC
backpressure or downstream switch decisions. Adapter reverse-cache hits do
not enter classifier counters. Existing `cache_miss_drops` counts packets
that neither reverse cache nor classification can route.

Labels remain local switch-interface metadata. UDP V5 still carries only
the payload; a tuntom receiver assigns a fresh stack locally. This feature
does not add tunnel label-stack transport, NAT, QoS actions or flow tracking.
