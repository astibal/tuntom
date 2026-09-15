# Integrated divert — network verification

Verified production binaries: `tuntom-switch`, `tomtom-switch-mp`,
`tuntom-divert-adapter`, and `tuntom-switch-adapter`. The encrypted tuntom path
ran through the lab's single-UID launcher, which adjusts only privilege dropping.

| Scenario | Result | Bytes in each direction | Diverted flows |
| --- | --- | ---: | ---: |
| st-1500 | PASS | 1,249,280 | 2 |
| mp-1500 | PASS | 1,249,280 | 2 |
| mp-9000-vrf | PASS | 1,249,280 | 2 |

Each run verified:

- The TCP connection opened before activation remains on the bypass path even
  after new flows have transferred data.
- New connections traverse both TUNs and a real Linux router.
- Captured SYN and SYN/ACK packets lose exactly one TTL unit across the router.
- The 1 MiB and 128 KiB echo transfers have correct contents; the server sees
  the original client IP and ports.
- Exact `[17,42] -> [99,42]` rules work while preserving return context.
- Adapter and switch drop counters are zero.
- MP uses IPC V2; the jumbo run also uses tuntom transport fragmentation.
- In the VRF scenario, both TUNs belong to routing table 4123 inside the router namespace.

Reproduction and manual commands: [docs/DIVERT.md](../../../../docs/DIVERT.md).
These results do not measure CPU use or verify actual smithproxy/TPROXY integration.

## Regression tests

- Full CTest suite: 50/51 PASS; `mk_switch_replacement_test` exceeded its overall
  300 s limit after successfully verifying migration of a legacy MP instance.
- The same helper test, run independently with the new binaries: **PASS**,
  including saving format-1 rules, ST/MP replacement, and final cleanup.
- A control run of the helper test with binaries built from the original HEAD:
  **PASS**, also slow. This change does not modify helper scripts or their timeout.
- After adding the exit adapter's `--l4-only` option, all six related tests passed
  when rerun (exit cache, reconnect, classifier, IPC path, and recovery).
- Expanded divert tests passed for ST, MP inline, and MP mmap; they cover ECMP,
  policy, 1500/9000/65535-byte payloads, origin reconnects, and admission boundaries.

To rerun the slow test independently from the repository root:

```bash
python3 tests/mk_switch_replacement_test.py \
  /tmp/tuntom-divert-build/tuntom-switch \
  /tmp/tuntom-divert-build/tomtom-switch-mp \
  /tmp/tuntom-divert-build/tuntomctl \
  /tmp/tuntom-divert-build/switch_mp_plan_fixture
```
