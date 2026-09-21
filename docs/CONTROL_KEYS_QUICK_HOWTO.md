# CONTROL keys: quick how-to

Use a dedicated authority key pair. The originating daemon keeps the private key;
nodes executing its commands receive only the public key and local grants.
For the security boundaries, see [CONTROL plane security](CONTROL_PLANE_SECURITY.md).

## 1. Generate a key pair

```sh
tuntomctl auth-keygen authority.key authority.pub 63 7
```

Arguments are `PRIVATE PUBLIC CAPS LEVEL`. This example grants all six current
operation capabilities (`63`, or `0x3f`) at level `7`. For a read-only authority,
use `1` instead of `63`. Level is a policy number, not a cryptographic strength.

Both files must be new; the tool refuses to overwrite existing files. The private
file is created with mode `0600`. Keep it readable only by the authority daemon's
account. Install the public file on targets through a trusted administrative path;
it is not secret, but unauthorized changes to it would change who is trusted.

The examples below assume installation at `/etc/tuntom/authority.key` on the
origin and `/etc/tuntom/authority.pub` on targets. Adapt paths and ownership to
your service account. These are dedicated tuntom key files, not SSH keys.

## 2. Configure the originating daemon

Typically, the authority is the **switch aggregating the tunnels**: it holds the
private authority key and originates CONTROL requests through those tunnels.
In a hub topology without a switch, **each hub-side tunnel daemon** takes this
role and loads an authority private key. Targets must pin the public key of the
authority used to control them. This is a typical deployment layout, not a
protocol requirement.

Append these options to its existing startup command:

```text
--allow-control-trusted
--control-authority-key /etc/tuntom/authority.key
--control-socket /run/tuntom/authority.control
```

The daemon loads and uses the private key; `tuntomctl` does not load it when
submitting a remote command. Restrict access to the local control socket to
operators allowed to use this authority. Loading a private key does not authorize
incoming commands; add public-key pins separately if this daemon must also be a
target.

## 3. Configure each target and relay

On each node that should execute the authority's commands, append:

```text
--allow-control-trusted
--control-trust-key /etc/tuntom/authority.pub
--control-require-level 3
```

The level requirement is optional (default `0`); the example key's level `7`
satisfies `3`. Options are supported by tunnels, both switch implementations and
exit/divert adapters. Restart daemons after changing key files or startup options.

A transit-only relay needs just:

```text
--allow-control-trusted
```

Every origin, relay and target on the route must enable network CONTROL. A relay
needs no authority keys for forwarding. Without pins it cannot authorize local
incoming operations in trusted mode.

## 4. Send a command

From an originating tunnel daemon to its peer:

```sh
tuntomctl remote --socket /run/tuntom/authority.control --- show stats
```

From an originating switch through its `tunnel42` port to that tunnel's peer:

```sh
tuntomctl switch /run/tuntom/switch.control --port tunnel42 --peer --- show stats
```

Use the socket of the daemon holding the private authority key. Challenges,
proofs and command retries are automatic. Challenges are request-driven; none is
sent automatically after initial handshake or rekey.
More routing examples are in [CONTROL v2](CONTROL_V2.md).

Local diagnostics do not require network CONTROL enablement:

```sh
tuntomctl /run/tuntom/authority.control show stats
```

## Using Fabric and the collector (current integration)

These instructions describe today's integration; the Fabric/collector split may
change. CONTROL itself works without either component.

- For ordinary local telemetry, let the collector access the daemon's local
  control socket. No CONTROL key or `--allow-control*` flag is needed for those
  local reads.
- For routed requests submitted through Fabric, install the private key on the
  **selected originating daemon**, using step 2 above. Install public pins on
  targets and enable CONTROL along the route as in step 3. Do not pass CONTROL
  private keys to `server.py`, `collector.py` or the browser: they currently have
  no CONTROL key configuration or challenge/proof implementation.
- The collector calls the local daemon socket directly; it does not need to run
  `tuntomctl`. The daemon performs the network challenge exchange on its behalf.
  A separate collector is optional; without `server.py --collector SOCKET`, the
  collection layer runs inside the web process.
- Keep the Fabric HTTP token and collector socket/UID permissions configured
  separately. They authorize access to Fabric and the collector, not authority
  identity on remote nodes. Syspiper credentials are separate too.

The current asynchronous API accepts `stats`, `flows`, `show` (rules show) and
`discover`, with a route relative to a discovered local process. The web UI uses
this API for DISCOVER; explicit routed reads are available through the API.
Even an empty route in this API uses routed submission and its enablement checks;
it is not the same path as ordinary local telemetry. Trusted-mode DISCOVER obtains a
separate challenge from every remote node before that node reports itself and
expands the branch. Pin the authority with read access on every node to be
traversed; a transit-only relay without pins cannot expand discovery. No additional
Fabric key configuration is needed. Discovery authenticates authority access and
protects responses, but does not independently certify target identities.

Current Fabric rule writes use the synchronous local socket API and require
`--allow-write` on the web and, when separate, the collector. That flag does not
enable network CONTROL, add capabilities to a key, or enable asynchronous routed
writes. A remote target authorizes the originating daemon's key, not an individual
Fabric user. Restrict collector access to authority-daemon sockets accordingly.

See [Fabric setup and API](../fabric/README.md) for collector startup and request
examples, and [the trust boundaries](CONTROL_PLANE_SECURITY.md#fabric-and-collector-current-integration)
for their security implications.

## Adjust permissions

Public files contain records in this form:

```text
x25519 PUBLIC_HEX CAPS LEVEL
```

Private files use:

```text
x25519-secret SECRET_HEX CAPS LEVEL
```

`PUBLIC_HEX` and `SECRET_HEX` stand for 64 lowercase hexadecimal digits, not literal
text to copy. CAPS and LEVEL are unsigned 64-bit values, decimal or `0x` hexadecimal.
Empty lines and lines starting with `#` are supported.

| Capability | Bitfield value |
| --- | --- |
| Read | 1 |
| Classifier validation | 2 |
| Classifier modification | 4 |
| Rules validation | 8 |
| Rules modification | 16 |
| Divert modification | 32 |
| All six above | 63 |

Combine capabilities with bitwise OR (for distinct bits, sum their values).
For example, read plus rules validation is `9`. One record assigns the same level
to all its bits. Multiple records for a key merge grants, taking the maximum per
capability; adding a lower-level record does not reduce an existing grant.

The target's public file is authoritative. You can restrict its CAPS/LEVEL even
if the origin's private file advertises broader grants. Private-file values are
selection hints; keep them consistent with intended use so the origin can choose
a suitable key.

Additional target options:

```text
--control-require-caps 0x100
--control-require-level 3
--control-require-authority PUBLIC_HEX
```

The first option adds required capability bit 8 to the command's own requirement;
grant that bit in the key records before using this example. Every required bit
must meet the configured minimum level. The last option selects one exact pinned
public key, taken from the second field of its public file; it does not replace
`--control-trust-key` or bypass capability checks. These options are independent.

Repeat `--control-trust-key PATH` to trust multiple authority files, or
`--control-authority-key PATH` to load multiple private authority files. A file
can also contain multiple key records.

## Rotate or revoke a key

For a planned rotation:

1. Generate a new pair under new filenames.
2. Add its public file alongside the old pin on targets and restart them. Update
   any `--control-require-authority` constraint as part of the coordinated cutover.
3. Switch originating daemons to the new private file and restart them. Verify a
   remote command using the new key.
4. Remove the old pin from every target and restart them; retire the old private key.

For revocation, remove all public records/pins for the compromised key on every
affected target and restart those daemons. Removing only the origin's private
file does not revoke copies held elsewhere. There is no live key reload or online
revocation service.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Network CONTROL disabled | Add `--allow-control-trusted` on the origin and every relay/target; key options alone do not enable it |
| Local stats work, remote command does not | Local diagnostics are independent; check network enablement, routing and keys |
| No successful authorization | Target pin matches origin's public key; grants cover all required bits and levels; origin's selection hints permit the challenge |
| Requested authority cannot be used | `--control-require-authority` names a pinned public key whose private key is loaded at the origin |
| Edited key file has no effect | Restart the daemon that loads it |
| Key generation refuses a path | Choose new filenames; existing files are never overwritten |
| Missing DISCOVER branches | Check read grants and pins on every traversed node; the two-second snapshot may also be incomplete due to loss, latency or resource limits |

Do not use `--allow-control-all` to fix authentication: it is **DEBUG / AT OWN
RISK**, accepts unsigned commands and bypasses configured trust/grant restrictions.
It is mutually exclusive with `--allow-control-trusted`.
