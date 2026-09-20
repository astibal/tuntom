#!/usr/bin/env python3
"""Blocked adapters, source pool exhaustion and malformed frames at 20+N ports."""
import argparse
import collections
import json
from pathlib import Path
import socket
import struct
import time

from churn import dense_options, frame, name, verify
from harness import Switch, metrics, physical_cpus, until


def fill(peers, target, generation, duration=.12):
    sent = 0
    sequence = collections.Counter()
    for index in range(10):
        peers[index].setblocking(False)
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        progress = False
        for source in range(10):
            sequence[source] += 1
            wire = frame(source, target, generation, sequence[source], size=9000)
            try:
                assert peers[source].send(wire) == len(wire)
                sent += 1
                progress = True
            except BlockingIOError:
                pass
        if not progress:
            time.sleep(.0002)
    for index in range(10):
        peers[index].settimeout(8)
    return sent


def healthy(peers, generation):
    for sequence in range(1, 31):
        peers[10].sendall(frame(10, 11, generation, sequence, size=1500))
        assert peers[11].recv(70000) == frame(10, 11, generation, sequence, True, 1500)


def quiet(peer):
    peer.settimeout(.05)
    try:
        value = peer.recv(70000)
        raise AssertionError(f"old-generation frame in replacement: {value[:64]!r}")
    except TimeoutError:
        pass
    finally:
        peer.settimeout(8)


def outstanding(stats):
    # Once sources stop and output is blocked, frame counters describe the
    # persistent queue ownership. A pool scan can also see a temporary buffer
    # held by an idle RX between acquire() and recv(EAGAIN).
    assert stats["route_misses"] == stats["target_disconnected"] == stats["send_errors"] == 0
    return (stats["frames_rx"] - stats["frames_tx"] - stats["queue_full_drops"]
            - stats["reconfiguration_drops"])


def run(args, adapters, workers):
    tag = f"pressure-{args.variant}-20-{adapters}-w{workers}"
    options = dense_options() + ["--workers", str(workers), "--pool-size", "16", "--queue-size", "4"]
    with Switch(args.switch, options, physical_cpus(), args.output / (tag + ".log")) as switch:
        peers = {index: switch.connect(name(index)) for index in range(20 + adapters)}
        initial, before = switch.stats(), metrics(switch.process.pid)
        accepted = fill(peers, 20, 1)
        until(lambda: switch.stats()["frames_rx"] == accepted)
        until(lambda: switch.stats()["send_eagain"] > 0)
        blocked = switch.stats()
        assert blocked["buffers_in_use"] > 0
        # Both expand and shrink with every pending pointer still owned.
        extra = switch.connect("trunk0")
        healthy(peers, 2)
        extra.close()
        until(lambda: switch.stats()["connections_current"] == 20 + adapters)
        assert switch.stats()["reconfiguration_drops"] == 0
        peers[20].settimeout(.1)
        delivered = 0
        previous = collections.defaultdict(int)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            try:
                wire = peers[20].recv(70000)
                verify(wire, 20, previous)
                delivered += 1
            except TimeoutError:
                stats = switch.stats()
                if stats["buffers_in_use"] == 0:
                    break
        else:
            raise AssertionError("POLLOUT did not drain the blocked adapter")
        peers[20].settimeout(8)
        drained = switch.stats()
        assert accepted == delivered + drained["queue_full_drops"]
        assert drained["frames_tx"] == delivered + 30
        assert drained["frames_rx"] == accepted + 30
        assert drained["reconfiguration_drops"] == 0

        # Replace each adapter with old kernel output deliberately left unread.
        replacements = []
        for adapter in range(adapters):
            target = 20 + adapter
            base = switch.stats()
            sent = fill(peers, target, 10 + adapter)
            until(lambda: switch.stats()["frames_rx"] == base["frames_rx"] + sent)
            time.sleep(.01)
            held = switch.stats()
            queued = outstanding(held)
            assert queued > 0 and held["buffers_in_use"] >= queued
            replacement = switch.connect(name(target))
            quiet(replacement)
            until(lambda: switch.stats()["buffers_in_use"] == 0)
            after = switch.stats()
            assert after["reconfiguration_drops"] - held["reconfiguration_drops"] == queued, (held, after)
            peers[target].close()
            peers[target] = replacement
            peers[0].sendall(frame(0, target, 30 + adapter, 1, size=65535))
            assert replacement.recv(70000) == frame(0, target, 30 + adapter, 1, True, 65535)
            healthy(peers, 40 + adapter)
            replacements.append(dict(target=target, sent=sent, held=queued, pool_snapshot=held["buffers_in_use"], after=after))

        # Invalid frames must not poison a registered port or its pool.
        good = frame(19, 20, 90, 11, size=65535) # Valid maximum body and eight labels.
        malformed = [b"short", bytes([2]) + good[1:], good[:2] + b"\x01" + good[3:],
                     good[:3] + b"\x00" + good[4:], good[:3] + b"\x09" + good[4:],
                     good[:1] + b"\x03" + good[2:], good[:-1],
                     good[:4] + struct.pack("!I", len(good) + 1) + good[8:]]
        errors_before = switch.stats()["malformed_frames"]
        for packet in malformed:
            peers[19].sendall(packet)
        until(lambda: switch.stats()["malformed_frames"] == errors_before + len(malformed))
        peers[19].sendall(good)
        assert peers[20].recv(70000) == frame(19, 20, 90, 11, True, 65535)
        peers[19].sendall(b"X" * 70000)
        until(lambda: switch.stats()["connections_current"] == 19 + adapters)
        peers[19].close()
        peers[19] = switch.connect(name(19))
        peers[19].sendall(frame(19, 20, 91, 1, size=64))
        assert peers[20].recv(70000) == frame(19, 20, 91, 1, True, 64)
        until(lambda: switch.stats()["buffers_in_use"] == 0)
        final, after = switch.stats(), metrics(switch.process.pid)
        assert final["send_errors"] == 0
        assert after["fd_count"] <= before["fd_count"] + 1
        row = dict(adapters=adapters, workers=workers, initial=initial, blocked=blocked, drained=drained,
                   replacements=replacements, final=final, before=before, after=after)
        (args.output / (tag + ".json")).write_text(json.dumps(row, indent=2) + "\n")
        print(f"PASS {tag}: adapter pressure/migration/replacement, 65535B+8 labels, malformed/oversize, FD reclaim", flush=True)


def exhausted(args, workers):
    tag = f"exhausted-{args.variant}-20-3-w{workers}"
    options = dense_options(23) + ["--workers", str(workers), "--pool-size", "4", "--queue-size", "8"]
    with Switch(args.switch, options, physical_cpus(), args.output / (tag + ".log")) as switch:
        peers = {index: switch.connect(name(index)) for index in range(23)}
        initial = switch.stats()
        fill(peers, 20, 1, .2)
        until(lambda: switch.stats()["pool_stalls"] > 0)
        healthy(peers, 2)
        time.sleep(.01)
        held = until(lambda: (value if outstanding(value := switch.stats()) == 40 else False),
                     description="ten full four-buffer pools")
        assert held["buffers_in_use"] >= 40, held
        for source in range(10):
            peers[source].close()
        until(lambda: switch.stats()["connections_current"] == 13)
        until(lambda: switch.stats()["buffers_in_use"] == 0)
        final = switch.stats()
        assert final["reconfiguration_drops"] - initial["reconfiguration_drops"] == 40
        healthy(peers, 3)
        for source in range(10):
            peers[source] = switch.connect(name(source))
        healthy(peers, 4)
        (args.output / (tag + ".json")).write_text(json.dumps(dict(initial=initial, held=held,
            final=switch.stats()), indent=2) + "\n")
        print(f"PASS {tag}: ten exhausted pools, source HUP, live sibling traffic, all slots released", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--switch", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--variant", default="native")
    parser.add_argument("--workers", type=int, nargs="+", default=[1, 2, 3, 4, 6, 8])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for workers in args.workers:
        for adapters in (1, 2, 3):
            run(args, adapters, workers)
        exhausted(args, workers)


if __name__ == "__main__":
    main()
