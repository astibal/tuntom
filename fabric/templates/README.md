# SAS Tunel Endpoint — contract V3

Generate a review directory locally:

```sh
python3 fabric/sas_te.py \
  --template fabric/templates/sas-te.example.json \
  --location fabric/templates/sas-location.example.json \
  --output /tmp/sas-te-review
```

Example identities, addresses, IDs and paths are placeholders, not reservations.
Services → SAS Endpoints offers read-only Resolve, a downloadable package and
Deploy for managed V3 instances. Deploy revalidates the location, capabilities,
rootfs ABI, ownership and empty slot; stages the client; prepares and starts the
collector-side listener; then registers the service. SAS alone starts the client.
Progress and errors are persisted and shown in the UI. Refresh status observes
SAS startup (normally at the next 60-second scan). Admin role and write mode are
required for mutations. No real workload is started by Resolve or package creation.

Current live support is deliberately limited to a directly connected origin,
one Fabric Tuntom payload per instance, schema-1 SAS rootfs with tuntom UID/GID
65532, the default content-addressed rootfs library, and a local observed switch.
Other profiles are rejected with an error. Use Undeploy before replacing a
payload; concurrent/shared-payload updates are not implemented. A restart during
an operation leaves an attention record for status/verified cleanup, not an
assumed success. Keys are generated for deployment, never included in downloads.
Undeploy withdraws registration, verifies exact-run SAS stop, removes the listener,
and removes owned installation files; it preserves other owners and stable locks.
Unknown state or stop failure leaves files intact and registration withdrawn.

## Optional interface configuration

Addresses are CIDR strings; routes contain `destination`, optional `via`, and
optional `metric` (default 100). Empty arrays make no network changes. Explicit
configuration creates a persistent TUN in the instance namespace, tagged with
`fabric:sas:<deployment-id>`, before SAS registration. This preserves addresses
and routes across SAS process restarts without an extra supervisor. Foreign
interfaces and routes through other interfaces are rejected, not overwritten.
After verified SAS stop, Undeploy deletes only the tagged TUN; its addresses and
routes disappear with it. SAS runtime directories remain untouched.

## Authoritative lookup

`sas_which_instance UUID` must return `microservice_contract_version: 3` and an
absolute `microservices_dir` separate from appliance `work_dir`. Missing fields,
V2 responses, overlapping paths and traversal are rejected. Fabric never derives
an installation path from `work_dir` and never falls back to V2.

Resolve shows origin, namespace, installation and appliance workspace separately.
Host sanity checks inspect the installation directory, conservative root-owned
non-writable ancestors, binary presence, accounts and tools. They do not prove
SAS mount isolation or ABI compatibility. Origin verification currently supports
the entry host itself; other-origin access remains a live integration gate.
Re-query the destination and validate directory identity under locks before any
future installation. Lookup is not a lifetime lease.

## Package and runtime

```text
payload/microservices/       -> authoritative microservices_dir
  10-start.sh
  tuntom/
    tuntom                  # included by the UI package operation
    version.txt
    version-commit.txt
registration.row            # proposal only; never activated by package creation
```

The UI uses the installed Fabric-side Tuntom binary, reading `--version` and
recording SHA-256. Commit metadata is optional. Offline rendering contains no
binary. No archive contains the PSK; provision the protected key at the path in
`plan.json`, with the same key on the server side. Editing inputs invalidates
previous previews. The manifest covers the binary and all generated files.

SAS supplies a read-only installation mount (normally `/microservices`) and a
private writable per-run runtime (normally `/run/sas-microservice`). Plain mode
also needs SAS's private mount sandbox; unrestricted host execution is not a V3
fallback. The script uses the supplied installation cwd and these variables:

- `SAS_INSTANCE_NETNS`
- `SAS_MICROSERVICE_PREFIX`
- `SAS_MICROSERVICE_RUN_ID`
- `SAS_MICROSERVICE_DIR`
- `SAS_MICROSERVICE_RUNTIME_DIR`
- `SAS_MICROSERVICE_PID_FILE`

It fails if required variables are absent, cwd/prefix/PID path disagree, or runtime
and installation overlap. It atomically publishes `$$` in private runtime then
execs `./tuntom/tuntom`. Its CONTROL socket also lives in private runtime.
It never writes PID into the installation directory. SAS controls runtime
permissions, process containment, retries and cleanup. Fabric must not delete or
edit runtime/PID files, signal PIDs, or enumerate descendants as a stop mechanism.

`--tun-netns "$SAS_INSTANCE_NETNS"` targets the SAS-mounted instance namespace handle; V3 does
not change routing or create namespaces. Transport stays in the execution netns
selected by the agreed SAS network profile.

Future update/undeploy must preserve other registry owners and lock inodes,
withdraw all affected registrations, and require verified exact-run SAS V3 stop
before modifying installation files. The installer registers last and starts
nothing. V2 files must not be promoted or moved automatically; coordinated
migration requires verified stops and freshly provisioned trusted paths.
