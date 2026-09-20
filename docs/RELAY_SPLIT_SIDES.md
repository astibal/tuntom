# Separate IN and OUT relay workers

This optional VIA mode separates the two sides of a proxy into disjoint groups
of tunnels and adapter processes. A packet crossing the proxy uses an IN relay
and an OUT relay, even when Linux delivers it to a different multiqueue TUN
worker than the one that learned its flow. No packet handoff between adapter
workers, TUN steering program, or switch flow cache is needed.

```text
Core switch                            Proxy host
  proxy-in-link0  <== V5 ==> IN worker 0 --+
  proxy-in-link1  <== V5 ==> IN worker 1 --+-- di0
                                               |
                                           smithproxy
                                               |
  proxy-out-link0 <== V5 ==> OUT worker 0 -+-- do0
  proxy-out-link1 <== V5 ==> OUT worker 1 -+

                     All four workers share one flow table.
```

IN and OUT refer to `divert-in` (client side) and `divert-out` (server side).
**Each tunnel remains bidirectional.** An IN worker opens only `di0` and its
IN IPC channel; an OUT worker opens only `do0` and its OUT IPC channel. Linux
can select any active queue of a TUN, but every queue belongs to the appropriate
side. This separates work between processes; it does not promise a specific
throughput increase or remove other switch, proxy, or host bottlenecks.

## Configuration

Use [the complete switch rules](../examples/relay/split-sides.rules). The new
fields must appear together and cannot be combined with the existing `relay`:

```text
service smithproxy {
    client-side proxy-in*
    server-side proxy-out*
    client-relay proxy-in-link*
    server-relay proxy-out-link*
    instances ["smithproxy#0"]
    stickiness hash
    unavailable drop
}
```

The relay selectors accept an exact physical port ID or a prefix followed by `*`.
They must not overlap: one physical tunnel cannot carry both sides of this
service. A registration on the wrong side is rejected. Service chains and ad hoc
format-3 `match ... via [...]` use the same service definitions.

All workers for one actual proxy instance use exactly the same `--via-instance`.
In split mode, path IDs distinguish transport attachments, not proxy instances:

```text
IN worker 0:  proxy-in.0~via:c:smithproxy#0
IN worker 1:  proxy-in.1~via:c:smithproxy#0
OUT worker 0: proxy-out.0~via:s:smithproxy#0
OUT worker 1: proxy-out.1~via:s:smithproxy#0
```

The switch first chooses a live proxy instance using the configured `hash` or
ordered `failover` policy. It then uses rendezvous flow hashing among the live
workers on the requested side of that instance. Adding a transport worker does
not change that proxy instance's weight. IN and OUT groups may have different
sizes; matching numeric path IDs do not form fixed pairs. At least one live
worker on **each** side is required. If none of the instances is complete,
`unavailable drop|pass` applies as before.

Use different instance IDs and shared files for different proxy instances.
Workers sharing an ID must serve the same proxy, network namespace, TUN pair,
and shared flow table. Registration cannot verify shared-memory identity across
hosts; this relationship is an explicit deployment contract. Owner tokens are
still checked within a relay. Split service configuration explicitly authorizes
workers on different relays to belong to the same proxy instance. Duplicate
live attachment names remain rejected. Each worker needs its own relay process.

## Example commands: two workers per side

Run the commands on the indicated hosts, using the same private `TUNTOM_SECRET`
on both hosts as for existing relays. Provision `/run/tuntom` and permissions
first. The example assumes existing `edge` and `internet` adapters and a core
address of `192.0.2.10`; substitute your own address.

On the core host:

```sh
tuntom-switch --socket /run/tuntom/core.sock \
    --rules-file examples/relay/split-sides.rules
```

In a separate shell on the core host:

```sh
for side in in out; do
    for path in 0 1; do
        tid=$((232 + path))
        if [ "$side" = out ]; then tid=$((tid + 2)); fi
        tuntom server "$tid" - --no-stats \
            --relay-connect /run/tuntom/core.sock \
            --relay-port-id "proxy-$side-link$path" &
    done
done
```

On the proxy host, start the remote relay endpoints:

```sh
for side in in out; do
    for path in 0 1; do
        tid=$((232 + path))
        if [ "$side" = out ]; then tid=$((tid + 2)); fi
        tuntom client "$tid" - 192.0.2.10 --no-stats \
            --relay-listen "/run/tuntom/relay-$side$path.sock" \
            --control-socket "/run/tuntom/relay-$side$path.control" &
    done
done
```

Then run the adapter workers in the proxy's network namespace:

```sh
for side in in out; do
    for path in 0 1; do
        tuntom-divert-adapter di0 do0 \
            --side "$side" --via-instance 'smithproxy#0' \
            --divert-in-port proxy-in --divert-out-port proxy-out \
            --relay-path "$path=/run/tuntom/relay-$side$path.sock" \
            --shared-flows /run/tuntom/smithproxy-split.flows \
            --admission immediate \
            --control-socket "/run/tuntom/adapter-$side$path.control" &
    done
done
```

Both positional TUN names remain required so all workers can validate their
shared group configuration. Each process only opens the TUN selected by `--side`.
Use `ip netns exec NAME` before adapter commands when appropriate; relay sockets
and the shared file must be accessible there. You create namespaces, routes,
addresses, VRFs, and proxy configuration yourself.

## Compatibility and migration

- `--side both` is the default and retains the existing two-channel behavior.
  Existing `--relay-path`, `--switch-socket`, and `relay` rules keep their meanings.
- `--side in|out` requires VIA, `--shared-flows`, and exactly one `--relay-path`.
  Local direct-to-switch single-sided workers are not enabled by this option.
- Up to **16 total workers** share a file. Path IDs must be unique within each
  side; `in:0` and `out:0` are separate shared-table members.
- Capacities, MTU, TUN names, port prefixes, namespace, admission settings, and
  proxy instance ID must agree. Mixed split/paired workers cannot join one live
  shared group. Shared table layout is unchanged.
- Upgrade the ST/MP switch and adapter binaries. The current V5 relay transport,
  IPC records, VIA label format, and eight-label limit require no wire change.
- Stop the old adapter group before migrating. Use separate IN/OUT relay sockets,
  the new service fields, and preferably a new shared file. Reapply TUN addresses,
  routes and VRF membership if the devices disappear after the last old worker
  closes them. Existing sessions are not guaranteed to survive this migration.

## Failure behavior and inspection

An exiting worker removes only its own side's TUN queue. Other workers keep the
shared contexts, and the switch can select remaining paths independently on each
side. Restarted workers reuse that state while any member remains alive. Losing
all workers on one side makes the instance unavailable. The shared state resets
only after all workers leave, as in the existing shared mode.

A worker detaches its queue while its **local** IPC channel is disconnected.
A failed remote tunnel with a still-connected local IPC channel remains the
existing limitation: TUN output can reach that worker and be lost. Check the
relay's `relay_acknowledged` status; this feature does not add lossless failover.

```sh
tuntomctl /run/tuntom/adapter-in0.control show stats
tuntomctl /run/tuntom/adapter-out0.control show flows
tuntomctl /run/tuntom/relay-in0.control show stats
```

New `worker_side=in|out|both` and `connected_paths` fields describe the worker.
For a connected single-sided worker, `connected_paths=1`, `connected_pairs=0`,
and only its own `divert_in_connected` or `divert_out_connected` is `1`.
`tun_queues_active=1` means that worker's queue is attached. Shared flow counters
cover the whole group; do not sum them across workers.

## Validation

`relay_split_test` runs actual ST/MP switches, encrypted V5 relay processes and
adapter loops with simulated TUN queues. It forces different output workers,
checks immediate shared-context visibility, startup with a missing side, worker
loss/restart, unequal group sizes, and rejection of invalid adapter options.
Unit tests cover side-specific registration, duplicate rejection, configuration
round trips, and stable instance selection when transport membership changes.

For real TCP and Linux multiqueue TUN routing, build the namespace-only tuntom
launcher as described in [VIA validation](VIA.md#validation), then run:

```sh
python3 -B experiments/divert_lab/run_integrated.py \
    --build /tmp/tuntom-via-build --split-sides --relay-paths 2 \
    --switch st --mtu 1500 --output /tmp/tuntom-split-st

python3 -B experiments/divert_lab/run_integrated.py \
    --build /tmp/tuntom-via-build --split-sides --relay-paths 2 \
    --switch mp --vrf --mtu 9000 --output /tmp/tuntom-split-mp
```

The lab uses a Linux router in the service namespace. It checks TCP payloads,
source identities, TTL changes across both TUNs, labels, and warmup bypass.
It does not validate smithproxy/TPROXY integration or measure throughput.

### Verified routing runs (2026-09-19)

| Switch | MTU | Service VRF | Workers | Result |
| --- | ---: | --- | --- | --- |
| ST | 1500 | No | 2 IN + 2 OUT | PASS |
| MP | 9000 | Yes | 2 IN + 2 OUT | PASS |

Each run completed 18 VIA TCP connections and one pre-existing bypassed
connection, with 1,773,568 application bytes in each direction across all
exchanges. All four workers carried IPC traffic. The switch and adapter drop
counters and TUN error counters remained zero. These are routing checks, not
throughput benchmarks.
