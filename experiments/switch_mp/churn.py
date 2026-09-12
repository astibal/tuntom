#!/usr/bin/env python3
"""Dense RX/TX matrix growth under verified traffic; no host network changes."""
import argparse
import collections
import json
import random
import selectors
import signal
import socket
import struct
import threading
import time
from pathlib import Path

from harness import Switch, metrics, physical_cpus, until


def name(index):
    return f"tunnel{index}" if index < 20 else f"adapter{index-20}" if index < 23 else "trunk0"


def dense_options(count=24):
    options = ["--trunk-port", "trunk0"]
    for index in range(20, 23):
        options += ["--exit-port", name(index)]
    for source in range(count):
        for target in range(count):
            options += ["--route", f"{name(source)}:{1000+target}={name(target)}:{2000+source}"]
    return options


def frame(source, target, generation, sequence, received=False, size=None):
    size = size or (64, 1500, 9000, 65535)[sequence % 4]
    payload = struct.pack("!IIIIQ", source, target, generation, size, sequence)
    payload += bytes([(source + generation + sequence) % 251]) * (size - 40)
    payload += struct.pack("!QQ", source, sequence)
    labels = [2000 + source if received else 1000 + target]
    if sequence % 11 == 0:
        labels += list(range(0x123456789ABCDEF0, 0x123456789ABCDEF7))
    opcode = 2 if received and 20 <= target < 23 else 1
    return struct.pack("!BBBBI", 1, opcode, 0, len(labels), 8 + len(labels) * 8 + size) + b"".join(
        struct.pack("!Q", label) for label in labels) + payload


def verify(wire, target, previous):
    assert len(wire) >= 8
    version, opcode, flags, labels, length = struct.unpack_from("!BBBBI", wire)
    assert version == 1 and flags == 0 and 1 <= labels <= 8 and length == len(wire)
    start = 8 + 8 * labels
    source, destination, generation, size, sequence = struct.unpack_from("!IIIIQ", wire, start)
    assert destination == target and source < 24 and size == len(wire) - start and sequence > 0
    assert wire == frame(source, target, generation, sequence, True, size), (source, target, generation, sequence)
    key = (source, generation, target)
    assert sequence > previous[key], ("duplicate/reordered", key, sequence, previous[key])
    previous[key] = sequence
    return source, generation


def run(args):
    rng = random.Random(args.seed)
    root = args.output
    root.mkdir(parents=True, exist_ok=True)
    options = dense_options() + ["--workers", str(args.workers), "--pool-size", "16", "--queue-size", "16"]
    stable = list(range(6)) + [20]
    with Switch(args.switch, options, physical_cpus(), root / (args.tag + ".switch.log")) as switch:
        peers = {index: switch.connect(name(index)) for index in stable}
        for peer in peers.values():
            peer.setblocking(False)
        initial = switch.stats()
        baseline = metrics(switch.process.pid)
        sent = collections.Counter()
        received = collections.Counter()
        backpressure = collections.Counter()
        previous = collections.defaultdict(int)
        stop_sending, stop_receiving = threading.Event(), threading.Event()
        errors = []
        sequence = collections.Counter()
        def send():
            began, rounds = time.monotonic(), 0
            try:
                while not stop_sending.is_set():
                    for source in stable:
                        sequence[source] += 1
                        seq = sequence[source]
                        target = stable[(stable.index(source) + seq) % len(stable)]
                        wire = frame(source, target, 1, seq)
                        try:
                            assert peers[source].send(wire) == len(wire)
                            sent[source] += 1
                        except BlockingIOError:
                            backpressure[source] += 1
                    rounds += 1
                    delay = began + rounds * len(stable) / args.rate - time.monotonic()
                    if delay > 0:
                        stop_sending.wait(delay)
            except BaseException as error:
                errors.append(repr(error))
                stop_sending.set()
        def receive():
            try:
                with selectors.DefaultSelector() as selector:
                    for target, peer in peers.items():
                        selector.register(peer, selectors.EVENT_READ, target)
                    while not stop_receiving.is_set():
                        for key, _ in selector.select(.05):
                            for _ in range(32):
                                try:
                                    wire = key.fileobj.recv(70000)
                                except BlockingIOError:
                                    break
                                assert wire, "stable peer disconnected"
                                source, generation = verify(wire, key.data, previous)
                                received[(source, generation)] += 1
            except BaseException as error:
                errors.append(repr(error))
                stop_sending.set()
        producer = threading.Thread(target=send)
        consumer = threading.Thread(target=receive)
        consumer.start()
        producer.start()
        observations = []
        began = time.monotonic()
        completed = 0
        try:
            for cycle in range(args.cycles):
                assert not errors, errors
                extra = {}
                # Gradually cross 8 and 16 tunnels, then add adapters and trunk.
                additions = list(range(6, 20)) + [21, 22, 23]
                if cycle % 2:
                    rng.shuffle(additions)
                for index in additions:
                    peer = switch.connect(name(index))
                    extra[index] = peer
                    for seq in range(1, 5):
                        peer.sendall(frame(index, stable[seq % len(stable)], cycle * 2 + 2, seq, size=1500))
                full = switch.stats()
                assert full["connections_current"] == 24 and full["matrix_queues"] == 24 * 24, full
                assert full["workers_pool"] == initial["workers_pool"]
                # Reconnect live extra sources without closing the old peer first.
                for index in rng.sample(additions, 3):
                    replacement = switch.connect(name(index))
                    extra[index].close()
                    extra[index] = replacement
                    replacement.sendall(frame(index, stable[0], cycle * 2 + 3, 1, size=9000))
                rng.shuffle(additions)
                for index in additions:
                    extra[index].close()
                until(lambda: switch.stats()["connections_current"] == len(stable), description="dense shrink")
                current = switch.stats()
                assert current["matrix_queues"] == len(stable) ** 2
                sample = dict(cycle=cycle, full=full, shrunk=current, metrics=metrics(switch.process.pid),
                              sent=sum(sent.values()), received=sum(received.values()), elapsed=time.monotonic()-began)
                observations.append(sample)
                with open(root / (args.tag + ".jsonl"), "a") as output:
                    output.write(json.dumps(sample) + "\n")
                completed = cycle + 1
                if cycle % 10 == 0:
                    print(f"CHURN {args.tag}: cycle {completed}/{args.cycles}, "
                          f"version {current['scheduler_version']}, stable sent {sum(sent.values())}, "
                          f"RSS {sample['metrics']['rss_kib']} KiB", flush=True)
        finally:
            stop_sending.set()
            producer.join(timeout=10)
            try:
                until(lambda: errors or sum(received[(source, 1)] for source in stable) == sum(sent.values()),
                      timeout=20, description="stable traffic drain")
                until(lambda: switch.stats()["buffers_in_use"] == 0, description="pool reclaim")
            finally:
                stop_receiving.set()
                consumer.join(timeout=10)
        assert not producer.is_alive() and not consumer.is_alive() and not errors, errors
        assert completed == args.cycles
        assert all(received[(source, 1)] == sent[source] and sent[source] > 100 for source in stable)
        final = switch.stats()
        assert final["queue_full_drops"] == final["send_errors"] == final["target_disconnected"] == final["route_misses"] == 0, final
        # Only retired extra-source packets may be discarded; stable frames are
        # verified individually and counted independently of those generations.
        after = metrics(switch.process.pid)
        assert after["fd_count"] <= baseline["fd_count"] + 1, (baseline, after)
        assert after["threads"] == baseline["threads"], (baseline, after)
        result = dict(command=switch.command, seed=args.seed, cycles=completed,
                      elapsed=time.monotonic()-began, stable_sent=dict(sent),
                      stable_received={source: received[(source, 1)] for source in stable},
                      source_backpressure=dict(backpressure), initial=initial, final=final,
                      before=baseline, after=after, peak_rss_kib=max(item["metrics"]["rss_kib"] for item in observations))
        (root / (args.tag + ".summary.json")).write_text(json.dumps(result, indent=2) + "\n")
        print(f"PASS {args.tag}: {completed} dense grow/shrink cycles, {sum(sent.values())} stable frames, "
              f"{final['reconfigurations']-initial['reconfigurations']} plan swaps", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--switch", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--cycles", type=int, default=100)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--rate", type=int, default=3000)
    parser.add_argument("--seed", type=int, default=20260912)
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    run(args)


if __name__ == "__main__":
    main()
