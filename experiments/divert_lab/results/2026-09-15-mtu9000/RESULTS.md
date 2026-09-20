# Divert router lab — PASS

Inner MTU: 9000; cookie: `HeM`.

Uses real Linux namespaces/TUN routing, tuntom encryption and the production exit adapter.

The rootless tuntom launcher changes only privilege dropping; this is not a CPU benchmark.

- existing TCP bypasses router
- new TCP traverses both divert TUNs
- Linux decrements SYN and SYN/ACK TTL
- stable origin 123 restored
- exact normal rules see original stack
- exit adapter restores DVRT
- client gets original stack
- source IP/port unchanged
- bidirectional echo hashes match

## TCP exchanges

| Exchange | Bytes each direction | Client port |
| --- | ---: | ---: |
| before_divert | 4096 | 40000 |
| existing_after_activation | 32768 | 40000 |
| new_diverted | 1048576 | 40001 |
| existing_after_new_flow | 32768 | 40000 |
| second_diverted | 131072 | 40002 |
