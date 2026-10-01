# AUTH gatekeeper lab

This unprivileged localhost lab starts:

```text
client -- AUTH_RESPONSE --> listener -- gatekeeper --> password_verifier.py
                                |
                             AUTH_OK
                                |
                             RELOCATE
                                v
                    child switch port [1001,13,77]
```

It uses relay mode on the client and therefore creates no TUN device and needs
no root privileges. The demo credential is `alice` / `laboratory-secret`.

Run it from the repository:

```sh
./experiments/auth_gatekeeper_lab/run.sh
```

For a finite smoke test:

```sh
./experiments/auth_gatekeeper_lab/run.sh --check
```

`password_verifier.py` demonstrates the external credential boundary. It sees
the binary AUTH request on stdin and communicates only accept/reject through
its exit status. It cannot choose the switch label or CONFIG. Those values live
in the generated gatekeeper configuration inside the lab's temporary runtime
directory.

The client intentionally has no `--config-command`: CONFIG is decoded and
authenticated but not applied to the host OS. Production clients provide a
privileged configurator for addresses, routes and DNS.

## tt-core1 client

`client-tt-core1.sh` connects to the lab listener on `tt-core1`. It expects the
shared transport secret in `~/.config/tuntom/tt-core1-auth.secret` (or the file
named by `TUNTOM_SECRET_FILE`) and builds only the local client:

```sh
./experiments/auth_gatekeeper_lab/client-tt-core1.sh
```

The client uses a local relay socket rather than creating a TUN. The checked-in
default destination is the current UDP address of `tt-core1`,
`192.168.155.169`, and the deployment uses tunnel ID `33` (UDP 40033).
Override these with `TUNTOM_SERVER_HOST` and `TUNTOM_TUNNEL_ID` if needed.

For a real Linux TUN and root-applied CONFIG, run the privileged launcher. It
configures and rebuilds the client from the current checkout before connecting:

```sh
sudo ./experiments/auth_gatekeeper_lab/client-tt-core1-tun.sh
```

It creates `ttauth0`, assigns `10.8.0.2/24`, routes the safe lab prefix
`10.9.0.0/24`, sets MTU 1400 and brings the link up. The helper also knows how
to preserve the UDP endpoint route before a future full-tunnel default route.
A root sidecar performs only fixed `ip` operations for authenticated CONFIG
received from the locally dropped-privilege `tuntom` process. DNS CONFIG is not
applied by this lab helper.
