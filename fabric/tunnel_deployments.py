"""Validated tunnel deployment intents and human-readable runtime runbooks."""
from __future__ import annotations
from binary_bundles import BinaryBundles

from datetime import datetime, timezone
import hashlib
import io
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import sqlite3
import threading
import tarfile

from errors import APIError
from deployment_cleanup import erase_secret
from deployment_diagnostics import safe_output


RUNTIME_ACCESS_CHECK = '''runuser -u tuntom -g tuntom -- python3 - "$root" <<'PYACCESS'
import os
import pathlib
import sys
root = pathlib.Path(sys.argv[1])
for directory in reversed((root, *root.parents)):
    if not os.access(directory, os.X_OK):
        sys.exit("Runtime access denied: user tuntom cannot traverse " + str(directory) +
                 ". Automatic ACL preparation did not make this directory accessible.")
for name in ("run", "log"):
    directory = root / name
    if not os.access(directory, os.W_OK | os.X_OK):
        sys.exit("Runtime access denied: user tuntom cannot write to " + str(directory))
print("RUNTIME_ACCESS_READY")
PYACCESS
'''


def timestamp():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def shell(*values):
    return " ".join(shlex.quote(str(value)) for value in values)


def clean_name(value, field, maximum=64):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,%d}" % (maximum - 1), value):
        raise ValueError(f"{field} must contain 1..{maximum} safe name characters")
    return value


def clean_path(value, field):
    if not isinstance(value, str) or not value.startswith("/") or len(value) > 1024 or "\x00" in value:
        raise ValueError(f"{field} must be an absolute path")
    if any(part in {"", ".", ".."} for part in Path(value).parts[1:]):
        raise ValueError(f"{field} must be a normalized absolute path")
    return value


def instance(base, index):
    return str(base) if index == 0 else f"{base}_{index}"


def validate_side(value, field):
    allowed = {"endpoint_id", "role", "peer_address", "attachment", "runtime", "switch_id", "autostart"}
    if not isinstance(value, dict) or set(value) - allowed:
        raise ValueError(f"invalid {field} fields")
    switch_id = value.get("switch_id")
    endpoint_id = value.get("endpoint_id")
    if switch_id is not None:
        if endpoint_id is not None or not isinstance(switch_id, str) or not re.fullmatch(r"[0-9a-f-]{36}:[0-9]+:[0-9]+", switch_id):
            raise ValueError(f"{field}.switch_id must identify a local observed switch")
        endpoint_id = "switch:" + switch_id
    if switch_id is None and (not isinstance(endpoint_id, str) or not re.fullmatch(r"[0-9a-f]{32}", endpoint_id)):
        raise ValueError(f"{field}.endpoint_id is invalid")
    role = value.get("role")
    if role not in {"initiator", "listener"}:
        raise ValueError(f"{field}.role must be initiator or listener")
    peer = value.get("peer_address")
    if role == "initiator":
        try: peer = str(ipaddress.ip_address(peer))
        except (ValueError, TypeError) as error: raise ValueError(f"{field}.peer_address must be an IP address") from error
    elif peer not in {None, ""}:
        raise ValueError(f"{field}.peer_address belongs only to the initiator")
    attachment = value.get("attachment", {})
    if not isinstance(attachment, dict) or attachment.get("type") not in {"tun", "switch"}:
        raise ValueError(f"{field}.attachment.type must be tun or switch")
    if attachment["type"] == "switch":
        if set(attachment) - {"type", "switch_socket", "port_id", "label"}:
            raise ValueError(f"invalid {field}.attachment fields")
        if switch_id is not None:
            if "switch_socket" in attachment: raise ValueError("observed switch socket is resolved by the collector")
        else:
            clean_path(attachment.get("switch_socket"), f"{field}.attachment.switch_socket")
        clean_name(attachment.get("port_id"), f"{field}.attachment.port_id", 48)
        label = attachment.get("label")
        if isinstance(label, str) and re.fullmatch(r"[0-9]{1,20}", label): label = int(label)
        if not isinstance(label, int) or isinstance(label, bool) or not 0 <= label <= 2**64 - 1:
            raise ValueError(f"{field}.attachment.label must be uint64")
        attachment = {**attachment, "label": str(label)}
    elif set(attachment) != {"type"}:
        raise ValueError(f"invalid {field}.attachment fields")
    if switch_id is not None and attachment["type"] != "switch": raise ValueError("existing switch requires switch attachment")
    runtime = value.get("runtime", "screen")
    if runtime not in {"screen", "systemd"}:
        raise ValueError(f"{field}.runtime must be screen or systemd")
    autostart = value.get("autostart", False)
    if not isinstance(autostart, bool) or (autostart and runtime != "systemd"):
        raise ValueError(f"{field}.autostart requires systemd and a boolean")
    return {"endpoint_id": endpoint_id, "role": role, "peer_address": peer if role == "initiator" else None,
            "attachment": dict(attachment), "runtime": runtime, **({"autostart": autostart} if runtime == "systemd" else {}), **({"switch_id": switch_id} if switch_id else {})}


def check_switch_ports(side, count, ports, via=None):
    base = side["attachment"]["port_id"]
    requested = {base + (f"_{index}" if index else "") for index in range(count)}
    if via:
        from via_deployments import relay_port
        requested = {relay_port(base, i, count, via) for i in range(count)}
    collisions = requested & set(ports)
    if collisions: raise ValueError("switch ports are already occupied: " + ", ".join(sorted(collisions)))


def validate(data):
    allowed = {"name", "tunnel_id", "count", "side_a", "side_b", "software", "secret", "via"}
    if not isinstance(data, dict) or set(data) - allowed:
        raise ValueError("invalid tunnel deployment fields")
    name = clean_name(data.get("name"), "name", 64)
    tunnel_id = data.get("tunnel_id")
    if tunnel_id is not None and (not isinstance(tunnel_id, int) or isinstance(tunnel_id, bool) or not 1 <= tunnel_id <= 255):
        raise ValueError("tunnel_id must be null (automatic) or 1..255")
    count = data.get("count", 1)
    if not isinstance(count, int) or isinstance(count, bool) or not 1 <= count <= 64:
        raise ValueError("count must be 1..64")
    a, b = validate_side(data.get("side_a"), "side_a"), validate_side(data.get("side_b"), "side_b")
    if a.get("switch_id") and b.get("switch_id"): raise ValueError("one side must be a Controlled Endpoint")
    if a["role"] == b["role"]:
        raise ValueError("exactly one side must be initiator and one listener")
    if a["endpoint_id"] == b["endpoint_id"]:
        raise ValueError("both tunnel sides cannot use the same Controlled Endpoint")
    software = data.get("software", {})
    if not isinstance(software, dict) or software.get("source") not in {"binary", "git_build", "bundle"}:
        raise ValueError("software.source must be binary, git_build or bundle")
    if software["source"] == "git_build":
        if set(software) - {"source", "revision"}:
            raise ValueError("invalid git software fields")
        revision = software.get("revision", "main")
        if not isinstance(revision, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._/-]{0,127}", revision) or ".." in revision:
            raise ValueError("software.revision is invalid")
        software = {"source": "git_build", "revision": revision}
    elif software["source"] == "bundle":
        if set(software) != {"source", "bundle_id"} or not isinstance(software["bundle_id"], str) or not re.fullmatch(r"[0-9a-f]{32}", software["bundle_id"]):
            raise ValueError("invalid binary bundle ID")
        software = dict(software)
    else:
        if set(software) - {"source", "sha256"}:
            raise ValueError("invalid binary software fields")
        digest = software.get("sha256", "")
        if not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
            raise ValueError("software.sha256 must contain 64 hex characters")
        software = {"source": "binary", "sha256": digest.lower()}
    secret = data.get("secret", {})
    if not isinstance(secret, dict) or secret.get("mode") not in {"generate", "provided"} or set(secret) - {"mode", "value"}:
        raise ValueError("secret.mode must be generate or provided")
    supplied = secret.get("value", "")
    if secret["mode"] == "provided" and not re.fullmatch(r"[0-9a-fA-F]{32}", supplied):
        raise ValueError("provided PSK must contain 32 hex characters")
    if secret["mode"] == "generate" and supplied:
        raise ValueError("generated PSK must not include a value")
    from via_deployments import validate_via
    via = validate_via(data.get("via"), a, b, count, software)
    return {**({"via": via} if via else {}), "name": name, "tunnel_id": tunnel_id, "count": count, "side_a": a, "side_b": b,
            "software": software, "secret": {"mode": secret["mode"]}}, supplied.lower() if supplied else None


def undeploy_bundle(config):
    root = f"/opt/tuntom/deployments/{config['name']}"
    program = Path(__file__).with_name("deployment_cleanup.py").read_text()
    files = {}
    for side_name in ("side_a", "side_b"):
        role = "client" if config[side_name]["role"] == "initiator" else "server"
        members = [instance(config["tunnel_id"], index) for index in range(config["count"])]
        command = shell("python3", "-", root, config["deployment_id"], role, *members)
        files[f"{side_name}/scripts/95-undeploy.sh"] = "#!/bin/bash\nset -euo pipefail\n" + command + " <<'FABRIC_CLEANUP'\n" + program + "\nFABRIC_CLEANUP\n"
    return files


def render_bundle(config, endpoint_addresses, git_url, root=None):
    root = root or f"/opt/tuntom/deployments/{config['name']}"
    scripts = {}
    members = [instance(config["tunnel_id"], index) for index in range(config["count"])]
    for side_name in ("side_a", "side_b"):
        side = config[side_name]
        suffix = "c" if side["role"] == "initiator" else "s"
        for index, member in enumerate(members):
            ident = member + suffix
            interface = "-" if side["attachment"]["type"] == "switch" else f"ut{ident}"
            args = [f"{root}/bin/tuntom", "client" if suffix == "c" else "server", member, interface]
            if suffix == "c": args.append(side["peer_address"])
            args += ["--mtu", "1500", "--transport-mtu", "1400", "--control-socket", f"{root}/run/{ident}.control", "--no-stats"]
            if side["attachment"]["type"] == "switch":
                attachment = side["attachment"]
                port = attachment["port_id"] + (f"_{index}" if index else "")
                args += ["--switch-socket", attachment["switch_socket"], "--switch-port-id", port,
                         "--switch-label", str(attachment["label"])]
            body = f'''#!/bin/bash
set -euo pipefail
umask 0077
root={shlex.quote(root)}
install -d -o tuntom -g tuntom -m 0770 "$root/run" "$root/log"
exec 9>"$root/run/{ident}.lock"
flock -n 9 || {{ echo {shlex.quote(ident + ' is already running')} >&2; exit 1; }}
rm -f -- "$root/run/{ident}.control"
source "$root/secrets/master.env"
export TUNTOM_SECRET
exec {shell(*args)}
'''
            scripts[f"{side_name}/runtime/run-{ident}.sh"] = body
        sessions = [(f"tuntom-{config['name']}-{member}{suffix}", f"run-{member}{suffix}.sh", f"{member}{suffix}.log") for member in members]
        starts = "\n".join(f"screen -L -Logfile \"$root/log/{log}\" -dmS {shell(session)} \"$root/runtime/{runner}\"" for session, runner, log in sessions)
        stops = "\n".join(f"screen -S {shell(session)} -p 0 -X stuff $'\\003' || true" for session, _, _ in reversed(sessions))
        checks = "\n".join(f'''for attempt in {{1..50}}; do
  timeout 1 "$root/bin/tuntomctl" "$root/run/{member}{suffix}.control" show stats >/dev/null 2>&1 && break
  sleep 0.1
done
timeout 1 "$root/bin/tuntomctl" "$root/run/{member}{suffix}.control" show stats >/dev/null''' for member in members)
        scripts[f"{side_name}/scripts/40-start.sh"] = f'''#!/bin/bash
set -euo pipefail
root={shlex.quote(root)}
{starts}
echo STARTED
'''
        scripts[f"{side_name}/scripts/50-verify.sh"] = f'''#!/bin/bash
set -euo pipefail
root={shlex.quote(root)}
{checks}
echo READY
'''
        scripts[f"{side_name}/scripts/90-stop.sh"] = f'''#!/bin/bash
set -euo pipefail
{stops}
'''
        scripts[f"{side_name}/scripts/80-status.sh"] = f'''#!/bin/bash
set -euo pipefail
root={shlex.quote(root)}
{checks}
screen -ls
'''
        scripts[f"{side_name}/scripts/99-rollback.sh"] = f'''#!/bin/bash
set -euo pipefail
root={shlex.quote(root)}
"$root/scripts/90-stop.sh"
echo STOPPED_FOR_ROLLBACK
'''
        address = endpoint_addresses[side["endpoint_id"]]
        preflight_ports = " ".join(str(40000 + config["tunnel_id"] + 256 * index) for index in range(config["count"]))
        tun_check = "test -c /dev/net/tun\n" if side["attachment"]["type"] == "tun" else ""
        scripts[f"{side_name}/scripts/00-preflight.sh"] = f'''#!/bin/bash
set -euo pipefail
test "$(id -u)" -eq 0
command -v screen >/dev/null
command -v flock >/dev/null
command -v ip >/dev/null
{tun_check}command -v ss >/dev/null
getent passwd tuntom >/dev/null || true
for port in {preflight_ports}; do
  if ss -H -lun "sport = :$port" | grep -q .; then echo "UDP port $port is occupied" >&2; exit 20; fi
done
echo PREFLIGHT_OK
'''
        if config["software"]["source"] == "git_build":
            revision = config["software"]["revision"]
            dependencies = '''apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y git g++ make screen iproute2 acl
'''
            install = f'''install -d -m 0755 "$root/source" "$root/bin"
if test -d "$root/source/.git"; then git -C "$root/source" fetch --tags origin; else git clone {shell(git_url)} "$root/source"; fi
git -C "$root/source" checkout --detach {shell(revision)}
g++ -std=c++17 -pthread -O2 -march=native -mtune=native -Wall -Wextra -pedantic "$root/source/src/main.cpp" -o "$root/bin/tuntom.new"
g++ -std=c++17 -pthread -O2 -Wall -Wextra -pedantic "$root/source/src/control/main.cpp" -o "$root/bin/tuntomctl.new"
install -m 0755 "$root/bin/tuntom.new" "$root/bin/tuntom"
install -m 0755 "$root/bin/tuntomctl.new" "$root/bin/tuntomctl"
'''
        else:
            digest = config["software"]["sha256"]
            dependencies = '''apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y screen iproute2 acl
'''
            install = f'''test "$(sha256sum "$root/incoming/tuntom" | cut -d' ' -f1)" = {shell(digest)}
install -D -m 0755 "$root/incoming/tuntom" "$root/bin/tuntom"
test -f "$root/incoming/tuntomctl"
install -m 0755 "$root/incoming/tuntomctl" "$root/bin/tuntomctl"
'''
        if config["software"].get("ctl_sha256"):
            install = f'''test "$(sha256sum "$root/incoming/tuntomctl" | cut -d' ' -f1)" = {shell(config["software"]["ctl_sha256"])}\n''' + install
        scripts[f"{side_name}/scripts/10-install-dependencies.sh"] = f'''#!/bin/bash
set -euo pipefail
{dependencies}getent group tuntom >/dev/null || groupadd --system tuntom
id -u tuntom >/dev/null 2>&1 || useradd --system --gid tuntom --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin tuntom
echo DEPENDENCIES_READY
'''
        scripts[f"{side_name}/scripts/20-install-software.sh"] = f'''#!/bin/bash
set -euo pipefail
root={shlex.quote(root)}
{install}echo SOFTWARE_READY
'''
        scripts[f"{side_name}/scripts/30-install-config.sh"] = f'''#!/bin/bash
set -euo pipefail
root={shlex.quote(root)}
test -x "$root/bin/tuntom"
test -x "$root/bin/tuntomctl"
test -s "$root/secrets/master.env"
install -d -o tuntom -g tuntom -m 0770 "$root/run" "$root/log"
chown root:tuntom "$root"
chmod 0750 "$root" "$root/bin" "$root/runtime" "$root/scripts"
chmod 0600 "$root/secrets/master.env"
chmod 0755 "$root"/runtime/run-*.sh "$root"/scripts/*.sh
echo CONFIG_READY
'''
        if not side.get("switch_id"):
            scripts[f"{side_name}/scripts/deployment-access.py"] = Path(__file__).with_name("deployment_access.py").read_text()
            scripts[f"{side_name}/scripts/30-install-config.sh"] = scripts[f"{side_name}/scripts/30-install-config.sh"].replace("echo CONFIG_READY", 'python3 "$root/scripts/deployment-access.py" "$root"\n' + RUNTIME_ACCESS_CHECK + "echo CONFIG_READY")
        if side.get("switch_id"):
            # Collector host uses its installed binaries; never apt/build here.
            prefix = f"{side_name}/"
            for key in list(scripts):
                if key.startswith(prefix):
                    scripts[key] = scripts[key].replace("-o tuntom -g tuntom", "-o root -g root").replace("root:tuntom", "root:root")
            scripts[prefix + "scripts/10-install-dependencies.sh"] = "#!/bin/bash\nset -euo pipefail\ncommand -v screen\ncommand -v flock\ncommand -v ss\n"
            scripts[prefix + "scripts/20-install-software.sh"] = "#!/bin/bash\nset -euo pipefail\n" + shell("install", "-d", "-m", "0750", root + "/bin") + "\n" + "\n".join(
                shell("install", "-m", "0755", side["local_binaries"][name], root + "/bin/" + name) for name in ("tuntom", "tuntomctl")) + "\n"
            scripts[prefix + "scripts/00-preflight.sh"] += shell("test", "-S", side["attachment"]["switch_socket"]) + "\n"
        scripts[f"{side_name}/README.txt"] = f'''Tuntom deployment {config['name']} / {side_name}
Management endpoint: {address}
Role: {side['role']}
Members: {', '.join(members)}

Run in order as root:
  scripts/00-preflight.sh
  scripts/10-install-dependencies.sh
  scripts/20-install-software.sh
  scripts/30-install-config.sh
  scripts/40-start.sh
  scripts/50-verify.sh

Inspect with scripts/80-status.sh and stop with scripts/90-stop.sh.
The PSK is intentionally absent from this bundle; create secrets/master.env mode 0600.
'''
    if config.get("via"):
        from via_deployments import extend_bundle
        extend_bundle(scripts, config, root)
    from systemd_deployments import extend_bundle as extend_systemd
    extend_systemd(scripts, config, root)
    scripts.update(undeploy_bundle(config))
    scripts["deployment.json"] = json.dumps(config, indent=2, ensure_ascii=False) + "\n"
    scripts[".fabric-deployment-id"] = config["deployment_id"] + "\n"
    manifest = "".join(f"{hashlib.sha256(content.encode()).hexdigest()}  {name}\n" for name, content in sorted(scripts.items()))
    scripts["manifest.sha256"] = manifest
    return scripts


class TunnelDeployments:
    def __init__(self, path, endpoints, git_url="https://github.com/astibal/tuntom.git", fabric=None):
        self.path = str(path) if path is not None else ":memory:"
        self.endpoints, self.git_url, self.fabric = endpoints, git_url, fabric
        self.lock, self.memory_secrets = threading.RLock(), {}
        self.binary_bundles = BinaryBundles(path)
        self.db = sqlite3.connect(self.path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("""CREATE TABLE IF NOT EXISTS tunnel_deployment(
            id TEXT PRIMARY KEY, name TEXT NOT NULL UNIQUE, generation INTEGER NOT NULL,
            status TEXT NOT NULL, config TEXT NOT NULL, bundle TEXT NOT NULL,
            secret_fingerprint TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
            last_error TEXT)""")
        columns = {row[1] for row in self.db.execute("PRAGMA table_info(tunnel_deployment)")}
        if "last_error" not in columns: self.db.execute("ALTER TABLE tunnel_deployment ADD COLUMN last_error TEXT")
        if "side_states" not in columns:
            self.db.execute("ALTER TABLE tunnel_deployment ADD COLUMN side_states TEXT NOT NULL DEFAULT '{}'")
        self.db.execute("UPDATE tunnel_deployment SET status=CASE WHEN status='undeploying' THEN 'undeploy_failed' ELSE 'failed' END, last_error='Previous operation was interrupted; runtime state needs verification' WHERE status IN ('preparing','starting','undeploying')")
        self.db.commit()
        if self.path != ":memory:":
            self.secret_dir = Path(self.path).parent / "deployment-secrets"
            self.secret_dir.mkdir(mode=0o700, exist_ok=True)
        else: self.secret_dir = None

    def close(self):
        with self.lock: self.db.close()
        self.binary_bundles.close()

    @staticmethod
    def _public(row, include_bundle=False):
        value = dict(row); value["config"] = json.loads(value["config"])
        value["side_states"] = json.loads(value.get("side_states", "{}"))
        bundle = json.loads(value.pop("bundle"))
        # Old deployments gain cleanup runbooks without modifying their saved deploy scripts.
        bundle.update(undeploy_bundle(value["config"]))
        files = {key: content for key, content in bundle.items() if key != "manifest.sha256"}
        bundle["manifest.sha256"] = "".join(f"{hashlib.sha256(content.encode()).hexdigest()}  {name}\n" for name, content in sorted(files.items()))
        value["archives"] = [f"deploy_runbook_{value['name']}.tar.gz", f"undeploy_runbook_{value['name']}.tar.gz"]
        value["scripts"] = sorted(bundle)
        if include_bundle: value["bundle"] = bundle
        return value

    def list(self):
        with self.lock:
            rows = self.db.execute("SELECT * FROM tunnel_deployment ORDER BY lower(name)").fetchall()
            return {"tunnel_deployments": [self._public(row) for row in rows]}

    def get(self, deployment_id, include_bundle=False):
        if not re.fullmatch(r"[0-9a-f]{32}", deployment_id): raise KeyError("unknown tunnel deployment")
        with self.lock: row = self.db.execute("SELECT * FROM tunnel_deployment WHERE id=?", (deployment_id,)).fetchone()
        if not row: raise KeyError("unknown tunnel deployment")
        return self._public(row, include_bundle)

    def create(self, data):
        config, supplied = validate(data)
        config["id_allocation"] = "automatic" if config["tunnel_id"] is None else "manual"
        if config["software"]["source"] == "bundle":
            selected = self.binary_bundles.get(config["software"]["bundle_id"])
            config["software"] = {"source": "binary", "bundle_id": selected["id"],
                "sha256": selected["files"]["tuntom"]["sha256"], "ctl_sha256": selected["files"]["tuntomctl"]["sha256"]}
        endpoint_addresses = {}
        for side in (config["side_a"], config["side_b"]):
            if side.get("switch_id"):
                if self.fabric is None: raise ValueError("collector switch deployment is unavailable")
                resolved = self.fabric.switch_deployment({"action": "inspect", "switch_id": side["switch_id"]})
                check_switch_ports(side, config["count"], resolved.get("ports", []), config.get("via"))
                side["attachment"]["switch_socket"] = resolved["switch_socket"]
                side["local_binaries"] = resolved["binaries"]
                side["switch_name"] = resolved["name"]
                endpoint_addresses[side["endpoint_id"]] = resolved["name"] + " (collector)"
                continue
            endpoint = self.endpoints.get(side["endpoint_id"])
            if endpoint["status"] != "supported": raise ValueError("both Controlled Endpoints must be supported")
            if config["software"].get("bundle_id"):
                self.binary_bundles.compatible(config["software"]["bundle_id"], (endpoint.get("snapshot") or {}).get("system", {}))
            endpoint_addresses[endpoint["id"]] = endpoint["address"]
        with self.lock:
            existing = self.db.execute("SELECT config FROM tunnel_deployment WHERE status != 'undeployed'").fetchall()
        requested = {config["side_a"]["endpoint_id"], config["side_b"]["endpoint_id"]}
        occupied_ids = set()
        if any(side.get("switch_id") for side in (config["side_a"], config["side_b"])):
            for endpoint in self.fabric.snapshot().get("endpoints", []):
                match = re.fullmatch(r"([0-9]+)(?:_[0-9]+)?[cs]", endpoint.get("name", ""))
                if endpoint.get("source", "local") == "local" and endpoint.get("kind") == "tunnel" and match:
                    occupied_ids.add(int(match[1]))
        for row in existing:
            other = json.loads(row["config"])
            occupied = {other["side_a"]["endpoint_id"], other["side_b"]["endpoint_id"]}
            if requested & occupied: occupied_ids.add(other["tunnel_id"])
            if config.get("via"):
                target = config["side_b"]
                for key in ("side_a", "side_b"):
                    other_side = other[key]
                    if other_side["endpoint_id"] != target["endpoint_id"] or other_side["attachment"]["type"] != "switch": continue
                    other_base = other_side["attachment"]["port_id"]
                    if other.get("via"):
                        from via_deployments import relay_port
                        ports = [relay_port(other_base, i, other["count"], other["via"]) for i in range(other["count"])]
                    else:
                        ports = [other_base + (f"_{i}" if i else "") for i in range(other["count"])]
                    check_switch_ports(target, config["count"], ports, config["via"])

        if config["tunnel_id"] is None:
            config["tunnel_id"] = next((value for value in range(1, 256) if value not in occupied_ids), None)
            if config["tunnel_id"] is None: raise RuntimeError("no free tunnel ID remains for these Controlled Endpoints")
        elif config["tunnel_id"] in occupied_ids:
            raise RuntimeError("tunnel ID already belongs to a deployment on this Controlled Endpoint")
        self._resolve_control_trust(config)
        secret = supplied or secrets.token_hex(16)
        deployment_id, now = secrets.token_hex(16), timestamp()
        config["schema"], config["deployment_id"] = 1, deployment_id
        bundle = render_bundle(config, endpoint_addresses, self.git_url)
        fingerprint = "sha256:" + hashlib.sha256(bytes.fromhex(secret)).hexdigest()
        if self.secret_dir:
            path = self.secret_dir / f"{deployment_id}.key"
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "w") as stream: stream.write(secret + "\n")
        else: self.memory_secrets[deployment_id] = secret
        try:
            with self.lock, self.db:
                self.db.execute("INSERT INTO tunnel_deployment(id,name,generation,status,config,bundle,secret_fingerprint,created_at,updated_at,last_error) VALUES(?,?,?,?,?,?,?,?,?,?)",
                    (deployment_id, config["name"], 1, "draft", json.dumps(config), json.dumps(bundle), fingerprint, now, now, None))
                row = self.db.execute("SELECT * FROM tunnel_deployment WHERE id=?", (deployment_id,)).fetchone()
        except Exception:
            if self.secret_dir: (self.secret_dir / f"{deployment_id}.key").unlink(missing_ok=True)
            else: self.memory_secrets.pop(deployment_id, None)
            raise
        return self._public(row)

    def _resolve_control_trust(self, config):
        via = config.get("via")
        if not via or not via.get("trust_key") or via.get("trust_public"): return
        switch = next((config[k].get("switch_id") for k in ("side_a", "side_b") if config[k].get("switch_id")), None)
        if switch and self.fabric is not None:
            result = self.fabric.switch_deployment({"action": "inspect", "switch_id": switch, "trust_key": via["trust_key"]})
            from via_deployments import public_key_records
            via["trust_public"] = public_key_records(result.get("trust_public"))
        else:
            from via_deployments import read_public_key
            via["trust_public"] = read_public_key(via["trust_key"])

    def delete(self, deployment_id):
        with self.lock, self.db:
            value = self.get(deployment_id)
            if value["status"] != "undeployed":
                raise RuntimeError("only undeployed connections can be deleted")
            self.db.execute("DELETE FROM tunnel_deployment WHERE id=? AND status='undeployed'", (deployment_id,))
        return {"id": deployment_id, "deleted": True}

    def script(self, deployment_id, name):
        value = self.get(deployment_id, include_bundle=True)
        if name not in value["bundle"]: raise KeyError("unknown deployment script")
        return value["bundle"][name]

    def archive(self, deployment_id, kind):
        if kind not in {"deploy", "undeploy"}: raise ValueError("invalid archive kind")
        value = self.get(deployment_id, include_bundle=True)
        files = dict(value["bundle"]) if kind == "deploy" else undeploy_bundle(value["config"])
        if kind == "deploy" and value["config"]["software"].get("bundle_id"):
            _, binaries = self.binary_bundles.get(value["config"]["software"]["bundle_id"], files=True)
            for side in ("side_a", "side_b"):
                if not value["config"][side].get("switch_id"):
                    files.update({side + "/incoming/" + name: content for name, content in binaries.items()})
        files["README.txt"] = (f"{kind.upper()} {value['name']}\n"
            "Run each side's scripts on its respective host, as root.\n"
            "Undeploy stops only this deployment and removes its directory and keys.\n"
            "Shared packages, users, switch and other deployments remain.\n"
            "Archives never include the PSK. For manual deploy supply secrets/master.env securely.\n"
            "Run undeploy on initiator first, then listener. Offline cleanup does not update Fabric's recorded state; use Undeploy in UI to reconcile it.\n")
        files["manifest.sha256"] = "".join(f"{hashlib.sha256(content if isinstance(content, bytes) else content.encode()).hexdigest()}  {name}\n" for name, content in sorted(files.items()) if name != "manifest.sha256")
        output = io.BytesIO()
        with tarfile.open(fileobj=output, mode="w:gz") as archive:
            for name, content in sorted(files.items()):
                data = content if isinstance(content, bytes) else content.encode(); info = tarfile.TarInfo(name); info.size = len(data)
                info.mode = 0o755 if name.endswith(".sh") else 0o644
                archive.addfile(info, io.BytesIO(data))
        return f"{kind}_runbook_{value['name']}.tar.gz", output.getvalue()

    def runtime_status(self, deployment_id):
        value = self.get(deployment_id)
        config = value["config"]
        sides = {}
        for side_name in ("side_a", "side_b"):
            side = config[side_name]
            if side["runtime"] != "systemd" or value["status"] in {"draft", "undeployed"}: continue
            try:
                if side.get("switch_id"):
                    output = self._local(config, side_name, "status").get("output", "")
                else:
                    root = f"/opt/tuntom/deployments/{config['name']}"
                    output = self.endpoints.run(side["endpoint_id"], shell("bash", root + "/scripts/80-status.sh"), timeout=45)
                sides[side_name] = {"output": safe_output(output, 16384)}
            except (OSError, RuntimeError, ValueError, KeyError, APIError) as error:
                sides[side_name] = {"error": safe_output(error)}
        return {"id": deployment_id, "sides": sides}

    def _side_state(self, deployment_id, side, state, error=None, step=None, output=None):
        with self.lock, self.db:
            row = self.db.execute("SELECT side_states FROM tunnel_deployment WHERE id=?", (deployment_id,)).fetchone()
            states = json.loads(row[0]); previous = states.get(side, {})
            record = {"state": state, "error": safe_output(error) if error else None, "at": timestamp()}
            if step: record["step"] = step
            if output: record["output"] = safe_output(output)
            history = previous.get("history", [])
            if step: history = (history + [record])[-40:]
            states[side] = {**record, "history": history}
            self.db.execute("UPDATE tunnel_deployment SET side_states=?,updated_at=? WHERE id=?", (json.dumps(states), timestamp(), deployment_id))

    def _step(self, deployment_id, side, step, operation):
        self._side_state(deployment_id, side, "working", step=step)
        try:
            result = operation()
        except (OSError, ValueError, RuntimeError, KeyError, APIError) as error:
            self._side_state(deployment_id, side, "failed", error, step=step)
            message = f"{side} / {step}: {safe_output(error)}"
            if isinstance(error, APIError): raise APIError(error.status, message) from error
            raise RuntimeError(message) from error
        output = result if isinstance(result, str) else json.dumps(result) if result else ''
        self._side_state(deployment_id, side, "succeeded", step=step, output=output)
        return result

    def undeploy(self, deployment_id):
        value = self.get(deployment_id, include_bundle=True)
        if value["status"] == "undeployed": return value
        with self.lock, self.db:
            changed = self.db.execute("UPDATE tunnel_deployment SET status='undeploying',last_error=NULL,updated_at=? WHERE id=? AND status IN ('running','failed','undeploy_failed')", (timestamp(), deployment_id)).rowcount
        if changed != 1: raise RuntimeError("deployment is not undeployable from its current state")
        config = value["config"]; failures = []
        for side_name in sorted(("side_a", "side_b"), key=lambda key: config[key]["role"] != "initiator"):
            if value["side_states"].get(side_name, {}).get("state") == "undeployed": continue
            self._side_state(deployment_id, side_name, "undeploying")
            try:
                if config[side_name].get("switch_id"):
                    self._step(deployment_id, side_name, "collector.undeploy", lambda: self._local(config, side_name, "undeploy"))
                else:
                    script = undeploy_bundle(config)[f"{side_name}/scripts/95-undeploy.sh"]
                    self._step(deployment_id, side_name, "95-undeploy.sh", lambda: self.endpoints.run(config[side_name]["endpoint_id"], "bash -s", script.encode(), 120))
                self._side_state(deployment_id, side_name, "undeployed")
            except (OSError, ValueError, RuntimeError, KeyError, APIError) as error:
                failures.append(side_name + ": " + str(error))
                self._side_state(deployment_id, side_name, "undeploy_failed", error)
        if not failures:
            try:
                if self.secret_dir: erase_secret(self.secret_dir / f"{deployment_id}.key")
                else: self.memory_secrets.pop(deployment_id, None)
            except (OSError, ValueError, RuntimeError) as error: failures.append("collector key: " + str(error))
        if failures:
            self._set_status(deployment_id, "undeploy_failed", "; ".join(failures))
            raise RuntimeError("; ".join(failures))
        self._set_status(deployment_id, "undeployed")
        return self.get(deployment_id)

    def _secret(self, deployment_id):
        if self.secret_dir: return (self.secret_dir / f"{deployment_id}.key").read_text().strip()
        return self.memory_secrets[deployment_id]

    @staticmethod
    def _archive(bundle, side_name, secret, binaries=None):
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode="w:gz") as archive:
            selected = [(name[len(side_name)+1:], content) for name, content in bundle.items()
                        if name.startswith(side_name + "/")]
            selected += [(name, bundle[name]) for name in ("deployment.json", ".fabric-deployment-id")]
            selected.extend(("incoming/" + name, content) for name, content in (binaries or {}).items())
            selected.append(("secrets/master.env", "TUNTOM_SECRET=" + secret + "\n"))
            for name, content in selected:
                data = content if isinstance(content, bytes) else content.encode(); info = tarfile.TarInfo(name); info.size = len(data)
                info.mode = 0o600 if name.startswith("secrets/") else (0o755 if name.endswith(".sh") else 0o644)
                info.uid = info.gid = 0; info.mtime = 0
                archive.addfile(info, io.BytesIO(data))
        return stream.getvalue()

    def _set_status(self, deployment_id, status, error=None):
        with self.lock, self.db:
            self.db.execute("UPDATE tunnel_deployment SET status=?,last_error=?,updated_at=? WHERE id=?",
                            (status, safe_output(error) if error else None, timestamp(), deployment_id))

    def _claim(self, deployment_id):
        with self.lock, self.db:
            changed = self.db.execute("UPDATE tunnel_deployment SET status='preparing',last_error=NULL,updated_at=? "
                "WHERE id=? AND status IN ('draft','failed')", (timestamp(), deployment_id)).rowcount
        if changed != 1: raise RuntimeError("deployment is not deployable from its current state")

    def deploy(self, deployment_id):
        value = self.get(deployment_id, include_bundle=True)
        if value["status"] not in {"draft", "failed"}: raise RuntimeError("deployment is not deployable from its current state")
        config, bundle, secret = value["config"], value["bundle"], self._secret(deployment_id)
        if config.get("via", {}).get("trust_key") and not config["via"].get("trust_public"):
            self._resolve_control_trust(config)
        addresses = {config[k]["endpoint_id"]: config[k].get("switch_name") or self.endpoints.get(config[k]["endpoint_id"])["address"] for k in ("side_a", "side_b")}
        bundle = render_bundle(config, addresses, self.git_url)
        with self.lock, self.db:
            self.db.execute("UPDATE tunnel_deployment SET config=?,bundle=?,updated_at=? WHERE id=?", (json.dumps(config),json.dumps(bundle),timestamp(),deployment_id))
        binaries = None
        if config["software"].get("bundle_id"):
            _, binaries = self.binary_bundles.get(config["software"]["bundle_id"], files=True)
        for side_name in ("side_a", "side_b"):
            if config[side_name].get("switch_id"):
                resolved = self.fabric.switch_deployment({"action": "inspect", "switch_id": config[side_name]["switch_id"]})
                check_switch_ports(config[side_name], config["count"], resolved.get("ports", []), config.get("via"))
                if resolved["switch_socket"] != config[side_name]["attachment"]["switch_socket"] or resolved["binaries"] != config[side_name]["local_binaries"]:
                    raise RuntimeError("switch runtime changed; generate a new runbook")
                continue
            endpoint = self.endpoints.get(config[side_name]["endpoint_id"])
            if binaries is not None:
                self.binary_bundles.compatible(config["software"]["bundle_id"], (endpoint.get("snapshot") or {}).get("system", {}))
            if endpoint["status"] != "supported":
                raise RuntimeError("both Controlled Endpoints must still be supported")
        self._claim(deployment_id)
        root = f"/opt/tuntom/deployments/{config['name']}"
        quoted_root, quoted_id = shlex.quote(root), shlex.quote(deployment_id)
        install = (f"set -e; umask 0077; install -d -m 0755 /opt/tuntom/deployments; "
                   f"if test -e {quoted_root}; then test \"$(cat {quoted_root}/.fabric-deployment-id)\" = {quoted_id}; "
                   f"tar -xzf - -C {quoted_root}; else stage=$(mktemp -d /opt/tuntom/deployments/.{config['name']}.XXXXXX); "
                   f"trap 'rm -rf \"$stage\"' EXIT; tar -xzf - -C \"$stage\"; mv \"$stage\" {quoted_root}; trap - EXIT; fi")
        started = []
        try:
            for side_name in sorted(("side_a", "side_b"), key=lambda key: not bool(config[key].get("switch_id"))):
                side = config[side_name]
                if side.get("switch_id"):
                    self._step(deployment_id, side_name, "collector.prepare", lambda: self._local(config, side_name, "prepare", secret))
                    continue
                self._step(deployment_id, side_name, "upload", lambda: self.endpoints.run(side["endpoint_id"], install, self._archive(bundle, side_name, secret, binaries), 120))
                for step in ("00-preflight.sh", "10-install-dependencies.sh", "20-install-software.sh", "30-install-config.sh"):
                    command = f"cd {quoted_root} && ./scripts/{step}"
                    self._step(deployment_id, side_name, step, lambda: self.endpoints.run(side["endpoint_id"], command, timeout=900))
            listener = "side_a" if config["side_a"]["role"] == "listener" else "side_b"
            initiator = "side_b" if listener == "side_a" else "side_a"
            self._set_status(deployment_id, "starting")
            for side_name in (listener, initiator):
                side = config[side_name]
                started.append(side_name)
                if side.get("switch_id"):
                    self._step(deployment_id, side_name, "collector.start", lambda: self._local(config, side_name, "start"))
                    self._side_state(deployment_id, side_name, "running")
                    continue
                self._step(deployment_id, side_name, "40-start.sh", lambda: self.endpoints.run(side["endpoint_id"], f"cd {quoted_root} && ./scripts/40-start.sh", timeout=120))
                self._step(deployment_id, side_name, "50-verify.sh", lambda: self.endpoints.run(side["endpoint_id"], f"cd {quoted_root} && ./scripts/50-verify.sh", timeout=120))
                self._side_state(deployment_id, side_name, "running")
            for side_name in (listener, initiator):
                side = config[side_name]
                if side.get("autostart"):
                    if side.get("switch_id"):
                        self._step(deployment_id, side_name, "collector.enable", lambda: self._local(config, side_name, "enable"))
                    else:
                        self._step(deployment_id, side_name, "60-enable.sh", lambda: self.endpoints.run(side["endpoint_id"], f"cd {quoted_root} && ./scripts/60-enable.sh", timeout=120))
                    self._side_state(deployment_id, side_name, "running")
            self._set_status(deployment_id, "running")
            return self.get(deployment_id)
        except (OSError, ValueError, RuntimeError, KeyError, APIError) as error:
            rollback_errors = []
            for side_name in reversed(started):
                try:
                    if config[side_name].get("switch_id"):
                        self._step(deployment_id, side_name, "collector.rollback", lambda: self._local(config, side_name, "rollback"))
                    else:
                        self._step(deployment_id, side_name, "99-rollback.sh", lambda: self.endpoints.run(config[side_name]["endpoint_id"], f"cd {quoted_root} && ./scripts/99-rollback.sh", timeout=120))
                    self._side_state(deployment_id, side_name, "rolled_back")
                except (OSError, ValueError, RuntimeError, KeyError, APIError) as rollback_error:
                    rollback_errors.append(safe_output(rollback_error))
                    self._side_state(deployment_id, side_name, "rollback_failed", rollback_error)
            message = safe_output(error)
            if rollback_errors: message += "\nRollback incomplete: " + "; ".join(rollback_errors)
            self._set_status(deployment_id, "failed", message)
            if rollback_errors:
                if isinstance(error, APIError): raise APIError(error.status, message) from error
                raise RuntimeError(message) from error
            raise

    def _local(self, config, side_name, action, secret=None):
        # Send only a typed intent, never scripts or arbitrary execution paths.
        intent = {k: config[k] for k in ("name", "tunnel_id", "count", "software", "secret")}
        if intent["software"]["source"] == "binary":
            intent["software"] = {key:intent["software"][key] for key in ("source","sha256")}
        if config.get("via"): intent["via"] = config["via"]
        for key in ("side_a", "side_b"):
            side = dict(config[key]); side["attachment"] = dict(side["attachment"])
            if side.get("switch_id"):
                for field in ("endpoint_id", "local_binaries", "switch_name"): side.pop(field, None)
                side["attachment"].pop("switch_socket", None)
            intent[key] = side
        intent["secret"] = {"mode": "generate"}
        body = {"action": action, "intent": intent, "deployment_id": config["deployment_id"]}
        if secret is not None: body["secret"] = secret
        return self.fabric.switch_deployment(body)
