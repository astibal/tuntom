# tuntom

`tuntom` is a tiny self-contained TUN-over-UDP tunnel for Linux.

It was written by **Ales Stibal and ChatGPT** :)

The main goal is simplicity:

- one C++17 source file
- one bootstrap shell script
- no external runtime dependencies
- source is copied to the remote host through SSH, compiled there, and started
- NAT-friendly client/server model
- Linux TUN interfaces on both ends
- authenticated protocol v2 with replay protection
- optional compatibility with protocol v1

The tunnel is intentionally small and dumb. 
Routing, policy routing, firewalling, transparent proxying, integration, and similar logic are left to normal Linux networking.

## Requirements

Both hosts need:

- Linux
- `g++` with C++17 support
- `iproute2`
- `/dev/net/tun`

The local machine also needs:

- `ssh`
- working SSH key authentication to the remote host

The default remote SSH user is `root`.

## Usage

Set a shared 128-bit secret:

```bash
export TUNTOM_SECRET=0123456789abcdef0123456789abcdef
```

Create tunnel 42 to host `sx2`:

```bash
sudo -E ./mk_tunnel.sh 42 sx2
```

This is equivalent to connecting to:

```text
root@sx2
```

You can also specify a user explicitly:

```bash
sudo -E ./mk_tunnel.sh 42 user@sx2
```

Tunnel ID determines the interface names, addresses, and UDP port.

For tunnel `42`:

```text
client interface: ut42c
server interface: ut42s

client address:   10.254.42.1
server address:   10.254.42.2

UDP port:         40042
MTU:              1400
```

After setup, this should work:

```bash
ping -I ut42c 10.254.42.2
```

Loopback testing is supported:

```bash
sudo -E ./mk_tunnel.sh 42 localhost
```

## Logs

The bootstrap script writes logs to:

```text
/tmp/udp_tun-42-client.log
/tmp/udp_tun-42-server.log
```

Debug packet dumps are currently enabled intentionally.
Project comes with Wireshark dissector [tuntom.lua](tuntom.lua).

## Files

```text
udp_tun.cpp     self-contained C++17 implementation
mk_tunnel.sh   build/deploy/start helper
README.md       quick usage
DETAILS.md      implementation notes
LICENSE.md      BSD 3-Clause license
```
