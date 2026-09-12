#!/usr/bin/env python3
"""Resource failure and reconnect checks against a dense 23-port switch."""
import argparse
import json
import os
from pathlib import Path
import select
import socket
import tempfile
import time

from churn import dense_options, frame, name
from harness import Switch, metrics, physical_cpus, until


def safe_stats(switch):
    deadline = time.monotonic() + 8
    while True:
        try:
            return switch.stats()
        except AssertionError as error:
            # ControlSocket intentionally emits a static error if formatting
            # the stats response itself hits the one injected allocation fault.
            if "resource" not in str(error).lower() and "memory" not in str(error).lower():
                raise
            assert time.monotonic() < deadline
            time.sleep(.01)


def allocation_checks(args):
    with tempfile.TemporaryDirectory(prefix="mp-alloc-fault.") as directory:
        marker = Path(directory) / "allocation"
        environment = dict(os.environ, LD_PRELOAD=str(args.library), TOMTOM_TEST_ALLOC_MARKER=str(marker))
        options = [item.replace("tunnel0", "fault") for item in dense_options(23)]
        options += ["--workers", "8"]
        with Switch(args.switch, options, physical_cpus(), args.output / "allocation-faults.log",
                    env=environment, fd_limit=96) as switch:
            peers = {index: switch.connect("fault" if index == 0 else name(index)) for index in range(23)}
            initial = switch.stats()
            baseline = metrics(switch.process.pid)
            rows = []
            ordinals = list(range(1, 41)) + [48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048]
            for sequence, ordinal in enumerate(ordinals, 1):
                before = safe_stats(switch)
                replacement = switch.connect()
                marker.write_text(str(ordinal))
                replacement.sendall(b"TTP\x01\x05\0\0\0fault")
                def resolved():
                    stats = safe_stats(switch)
                    # The new FD can close just AFTER this stats reply was
                    # produced. Wait for main's counters, not a later HUP paired
                    # with an earlier snapshot of an empty pending list.
                    done = (stats["registrations_ok"] > before["registrations_ok"] or
                            stats["runtime_allocation_errors"] > before["runtime_allocation_errors"])
                    return stats if done and stats["connections_pending"] == 0 else False
                after = until(resolved, description=f"allocation ordinal {ordinal}")
                marker.unlink()
                accepted = after["registrations_ok"] - before["registrations_ok"]
                failures = after["runtime_allocation_errors"] - before["runtime_allocation_errors"]
                assert accepted in (0, 1) and failures in (0, 1), (ordinal, before, after)
                assert accepted or failures == 1, (ordinal, before, after)
                assert after["connections_current"] == 23
                if accepted:
                    assert peers[0].recv(100) == b""
                    peers[0].close()
                    peers[0] = replacement
                else:
                    assert replacement.recv(100) == b""
                    replacement.close()
                # The old or new complete plan must still forward a fresh frame.
                peers[1].sendall(frame(1, 0, 1, sequence, size=1500))
                assert peers[0].recv(70000) == frame(1, 0, 1, sequence, True, 1500)
                peers[2].sendall(frame(2, 20, 1, sequence, size=9000))
                assert peers[20].recv(70000) == frame(2, 20, 1, sequence, True, 9000)
                until(lambda: safe_stats(switch)["buffers_in_use"] == 0)
                sample = metrics(switch.process.pid)
                assert sample["fd_count"] <= baseline["fd_count"] + 1, (ordinal, baseline, sample)
                assert sample["threads"] == baseline["threads"]
                rows.append(dict(ordinal=ordinal, faults=failures, replacement_committed=accepted, metrics=sample))
                print(f"ALLOC {ordinal}: faults={failures}, committed={accepted}, fds={sample['fd_count']}", flush=True)
            final = safe_stats(switch)
            assert final["runtime_allocation_errors"] - initial["runtime_allocation_errors"] >= 40
            assert final["frames_rx"] == final["frames_tx"] == len(ordinals) * 2
            (args.output / "allocation-faults.json").write_text(json.dumps(dict(initial=initial, final=final,
                rows=rows, before=baseline, after=metrics(switch.process.pid)), indent=2) + "\n")


def syscall_checks(args):
    for operation in ("poll", "ppoll", "send", "recv"):
        with tempfile.TemporaryDirectory(prefix="mp-syscall-fault.") as directory:
            marker = Path(directory) / "enabled"
            environment = dict(os.environ, LD_PRELOAD=str(args.library), TOMTOM_TEST_SYSCALL=operation,
                               TOMTOM_TEST_SYSCALL_MARKER=str(marker))
            with Switch(args.switch, dense_options(23), physical_cpus(), args.output / f"syscall-{operation}.log",
                        env=environment, fd_limit=96) as switch:
                peers = {index: switch.connect(name(index)) for index in range(23)}
                initial, before = switch.stats(), metrics(switch.process.pid)
                marker.touch()
                began = time.monotonic()
                for sequence in range(1, 51):
                    source, target = sequence % 20, 20 + sequence % 3
                    peers[source].sendall(frame(source, target, 1, sequence, size=1500))
                    assert peers[target].recv(70000) == frame(source, target, 1, sequence, True, 1500)
                while time.monotonic() - began < .35:
                    switch.stats() # Control must remain responsive during injected failures.
                    time.sleep(.02)
                marker.unlink()
                until(lambda: switch.stats()["buffers_in_use"] == 0)
                final, after = switch.stats(), metrics(switch.process.pid)
                assert final["frames_rx"] == final["frames_tx"] == 50
                assert final["send_errors"] == final["queue_full_drops"] == 0
                if operation == "poll":
                    assert 1 <= final["runtime_poll_errors"] <= 10
                if operation == "ppoll":
                    assert 1 <= final["worker_poll_errors"] <= 8 * 100
                if operation == "send":
                    assert final["send_eagain"] >= 100
                assert after["cpu_s"] - before["cpu_s"] < 1, "fault recovery spin"
                (args.output / f"syscall-{operation}.json").write_text(json.dumps(dict(initial=initial,
                    final=final, before=before, after=after, elapsed=time.monotonic()-began), indent=2) + "\n")
                print(f"PASS syscall {operation}: IPC, control, pool return and bounded recovery", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--switch", required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    allocation_checks(args)
    syscall_checks(args)


if __name__ == "__main__":
    main()
