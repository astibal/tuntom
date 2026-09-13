#!/usr/bin/env python3
"""Check real codec output, slot reuse, queue ordering, and tag rejection."""
import argparse
from bench import invoke, physical_cpus


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='/tmp/tuntom-ascon-workers')
    args = parser.parse_args()
    cpus = physical_cpus()[:5]
    variants = [w for w in (0, 1, 2, 4) if w < len(cpus)]
    checks = 0
    reordered = 0
    for size in (1, 15, 16, 17, 1400):
        for mode in ('tx', 'rx', 'mixed', 'copy'):
            checksum = None
            for workers in variants:
                for batch in ([1] if workers == 0 else [1, 3, 8]):
                    row = invoke(args.binary, cpus, mode, size, workers, batch, 521,
                                 slots=48, warmup=31, verify=True)
                    assert row['rejected'] == 0
                    if checksum is None:
                        checksum = row['checksum']
                    assert checksum == row['checksum'], (size, mode, workers, batch)
                    if workers <= 1:
                        assert row['out_of_order_observed'] == 0
                    checks += 1
    for size in (64, 1400, 9000):
        for mode in ('rx', 'mixed'):
            for workers in variants:
                row = invoke(args.binary, cpus, mode, size, workers, 1, 1031,
                             slots=17, warmup=19, verify=True, tamper=True, delay=50)
                expected = sum((n % 17 == 0) and (mode == 'rx' or n % 2 == 1) for n in range(1031))
                assert row['rejected'] == expected
                reordered += row['out_of_order_observed']
                checks += 1
    if any(w > 1 for w in variants):
        assert reordered > 0, 'delayed workers did not exercise out-of-order completion'
    if 1 in variants:
        invoke(args.binary, cpus, 'mixed', 1400, 1, 1, 1031,
               slots=1, warmup=0, verify=True)
        checks += 1
    print(f'{checks} correctness cases passed; worker counts: {variants}')


if __name__ == '__main__':
    main()
