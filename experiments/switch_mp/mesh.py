#!/usr/bin/env python3
"""Exercise every edge of a live 24 x 24 SPSC matrix, including loopback."""
import argparse
import collections
import json
from pathlib import Path
import selectors
import threading
import time

from churn import dense_options, frame, name, verify
from harness import Switch, physical_cpus, until


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--switch", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--variant", default="native")
    parser.add_argument("--duration", type=float, default=30)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    tag = "mesh-" + args.variant
    with Switch(args.switch, dense_options() + ["--pool-size", "16", "--queue-size", "16"],
                physical_cpus(), args.output / (tag + ".log")) as switch:
        peers = [switch.connect(name(index)) for index in range(24)]
        for peer in peers:
            peer.setblocking(False)
        initial = switch.stats()
        assert initial["matrix_queues"] == 576
        sent, received, previous = collections.Counter(), collections.Counter(), collections.defaultdict(int)
        stop, errors = threading.Event(), []
        def receive():
            try:
                with selectors.DefaultSelector() as selector:
                    for target, peer in enumerate(peers):
                        selector.register(peer, selectors.EVENT_READ, target)
                    while not stop.is_set():
                        for key, _ in selector.select(.02):
                            for _ in range(32):
                                try:
                                    wire = key.fileobj.recv(70000)
                                except BlockingIOError:
                                    break
                                source, generation = verify(wire, key.data, previous)
                                assert generation == 1
                                received[source, key.data] += 1
            except BaseException as error:
                errors.append(repr(error))
        consumer = threading.Thread(target=receive)
        consumer.start()
        backpressure, sequence = 0, 0
        began = time.monotonic()
        try:
            while time.monotonic() - began < args.duration:
                assert not errors, errors
                sequence += 1
                for source, peer in enumerate(peers):
                    target = (source + sequence) % 24
                    wire = frame(source, target, 1, sequence)
                    try:
                        assert peer.send(wire) == len(wire)
                        sent[source, target] += 1
                    except BlockingIOError:
                        backpressure += 1
                delay = began + sequence * 24 / 6000 - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
            until(lambda: errors or sum(sent.values()) == sum(received.values()),
                  timeout=20, description="all mesh edges drained")
        finally:
            stop.set()
            consumer.join(timeout=10)
        assert not errors and not consumer.is_alive(), errors
        assert len(sent) == len(received) == 576 and sent == received
        assert min(sent.values()) > 10
        until(lambda: switch.stats()["buffers_in_use"] == 0)
        final = switch.stats()
        assert final["frames_rx"] == final["frames_tx"] == sum(sent.values())
        assert final["queue_full_drops"] == final["send_errors"] == final["route_misses"] == 0
        row = dict(initial=initial, final=final, duration=args.duration, source_backpressure=backpressure,
                   accepted=sum(sent.values()), delivered=sum(received.values()), active_edges=len(sent),
                   min_per_edge=min(sent.values()), per_edge={f"{s}->{t}": n for (s, t), n in sent.items()})
        (args.output / (tag + ".json")).write_text(json.dumps(row, indent=2) + "\n")
        print(f"PASS {tag}: {row['accepted']} frames, all 576 edges, minimum {row['min_per_edge']} per edge", flush=True)


if __name__ == "__main__":
    main()
