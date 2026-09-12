#!/usr/bin/env python3
"""Reproducible, serial MP test matrix; append every completed case immediately."""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import signal
import sys
import time
import traceback

from harness import load_case, physical_cpus, until


def write_line(path, value):
    with open(path, "a") as output:
        output.write(json.dumps(value) + "\n")
        output.flush()


def cases(args):
    if args.phase == "correctness":
        sizes = (64, 1500, 9000, 0, 65535)
        for index, (tunnels, adapters, workers) in enumerate(itertools.product((10, 12, 16, 20), (1, 2, 3), (1, 2, 3, 4, 6, 8))):
            yield args.variant, args.switch, dict(tunnels=tunnels, adapters=adapters, workers=workers,
                size=sizes[index % len(sizes)], rate=6000, duration=args.duration or 1.5,
                verify_all=True, switch_cores=8, direction="duplex", shape="equal")
        # Direction-specific and hot-adapter cases explicitly include uneven 20/3.
        for adapters, direction, shape in itertools.product((1, 2, 3), ("up", "down", "duplex"), ("equal", "hot")):
            yield args.variant, args.switch, dict(tunnels=20, adapters=adapters, workers=8,
                size=0, rate=12000, duration=args.duration or 1.5, verify_all=True,
                switch_cores=8, direction=direction, shape=shape)
    elif args.phase == "performance":
        for tunnels, adapters, size, rate in itertools.product((10, 12, 16, 20), (1, 2, 3), (64, 1500, 9000, 0), (160000, 240000, 0)):
            for repeat in range(args.repeats):
                options = dict(tunnels=tunnels, adapters=adapters, size=size, rate=rate,
                               duration=args.duration or 4, verify_all=False, switch_cores=6,
                               direction="duplex", shape="equal", repeat=repeat)
                variants = [("mp", args.switch)]
                if args.reference and size == 9000:
                    variants = [("single", args.reference), *variants]
                for variant, binary in variants if repeat % 2 == 0 else variants[::-1]:
                    yield variant, binary, options
    elif args.phase == "driver-scaling":
        for adapters, rate, driver_threads in itertools.product((1, 2, 3), (240000, 0), (1, 2)):
            for repeat in range(args.repeats):
                options = dict(tunnels=20, adapters=adapters, size=9000, rate=rate,
                               duration=args.duration or 4, verify_all=False, switch_cores=4,
                               driver_threads=driver_threads, direction="duplex", shape="equal", repeat=repeat)
                variants = [("mp", args.switch)]
                if args.reference:
                    variants.insert(0, ("single", args.reference))
                for variant, binary in variants if repeat % 2 == 0 else variants[::-1]:
                    yield variant, binary, options
    elif args.phase == "soak":
        yield args.variant, args.switch, dict(tunnels=20, adapters=3, workers=8,
            size=0, rate=120000 if args.variant == "native" else 12000,
            duration=args.duration or (900 if args.variant == "native" else 300),
            verify_all=args.variant != "native", switch_cores=8, direction="duplex", shape="hot")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=("correctness", "performance", "driver-scaling", "soak"), required=True)
    parser.add_argument("--switch", required=True)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--reference")
    parser.add_argument("--variant", default="native")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--duration", type=float)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--skip", type=int, default=0)
    parser.add_argument("--soak-churn-interval", type=float, default=30)
    args = parser.parse_args()
    if args.duration is not None and args.duration <= 0 or args.repeats <= 0:
        parser.error("positive duration and repeats required")
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    args.output.mkdir(parents=True, exist_ok=True)
    cpus = physical_cpus()[:8]
    assert len(cpus) == 8
    prefix = f"{args.phase}-{args.variant}"
    sources = list(Path("src/switch_mp").glob("*.hpp")) + [Path("src/switch_mp/main.cpp")]
    sources += list(Path(__file__).parent.glob("*.py")) + [Path(__file__).parent / "load.cpp"]
    binaries = [args.switch, args.driver] + ([args.reference] if args.reference else [])
    metadata = dict(started=time.time(), command=sys.argv, cpus=cpus,
                    sha256={str(path): hashlib.sha256(Path(path).read_bytes()).hexdigest() for path in [*sources, *binaries]})
    (args.output / f"{prefix}.metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    matrix = list(cases(args))
    began = time.monotonic()
    for index, (variant, binary, case) in enumerate(matrix):
        if index < args.skip:
            continue
        tag = f"{prefix}-{index:04d}-{variant}"
        print(f"START {index+1}/{len(matrix)} {tag} {case}", flush=True)
        try:
            extra_options, during = [], None
            if args.phase == "soak":
                extra_options = ["--trunk-port", "soak-trunk0", "--trunk-port", "soak-trunk1"]
                temporary = []
                last_slot = [-1]
                def during(switch, elapsed):
                    slot = int(elapsed / args.soak_churn_interval)
                    finish = elapsed >= case["duration"] - 5
                    if slot == last_slot[0] and not (finish and temporary):
                        return
                    last_slot[0] = slot
                    if temporary:
                        for peer in temporary:
                            peer.close()
                        temporary.clear()
                        until(lambda: switch.stats()["connections_current"] == case["tunnels"] + case["adapters"],
                              description="soak trunk removal")
                    elif not finish and slot > 0:
                        temporary.extend(switch.connect(f"soak-trunk{item}") for item in range(2))
                    snapshot = switch.stats()
                    if snapshot["workers_pool"] == 8:
                        assert snapshot["role_RX_shards"] == (2 if temporary else 3), snapshot
                        assert snapshot["role_RXa_shards"] == (2 if temporary else 1), snapshot
                    write_line(args.output / (tag + ".topology.jsonl"), dict(elapsed=elapsed, stats=snapshot))
                    print(f"SOAK {tag}: {elapsed:.0f}s, RX={snapshot['frames_rx']}, TX={snapshot['frames_tx']}, "
                          f"version={snapshot['scheduler_version']}, ports={snapshot['connections_current']}", flush=True)
            row = load_case(binary, args.driver, case, cpus, args.output / "logs", tag,
                            extra_options=extra_options, during=during)
            row.update(variant=variant, index=index, phase=args.phase, finished=time.time())
            write_line(args.output / f"{prefix}.jsonl", row)
            (args.output / "progress.json").write_text(json.dumps(dict(phase=prefix, completed=index+1,
                total=len(matrix), elapsed=time.monotonic()-began, last_tag=tag), indent=2) + "\n")
            print(f"PASS {tag}: {row['received']} frames, {row['pps']:.0f} pps, "
                  f"CPU={row['cpu_cores']:.2f}, P99={row['latency_p99_us']:.1f} us, "
                  f"offered-loss={row['offered_loss_percent']:.3f}%, "
                  f"switch-loss={row['switch_loss_percent']:.3f}%", flush=True)
        except BaseException:
            write_line(args.output / "failures.jsonl", dict(tag=tag, case=case, binary=binary,
                error=traceback.format_exc(), time=time.time()))
            raise
    print(f"COMPLETE {prefix}: {len(matrix)-args.skip} cases in {time.monotonic()-began:.1f} s", flush=True)


if __name__ == "__main__":
    main()
