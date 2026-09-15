"""Small, stateless interpretations of two adjacent control samples."""
from __future__ import annotations

import re
import math


def counter(key):
    return bool(re.search(r"(?:^drops_|_(?:packets|bytes|errors|drops|disconnects|reconnects|poll_calls|cpu_ns)$)", key)
                or key in {"frames_rx", "frames_tx", "bytes_rx", "bytes_tx", "route_misses",
                           "target_disconnected", "malformed_frames", "pool_stalls", "reconfigurations",
                           "rtt_lost", "init_timestamp_rejected", "init_nonce_capacity_rejected"})


def changes(metrics, previous, seconds):
    result = {"ready": bool(previous and seconds and seconds > 0),
              "interval_seconds": round(seconds, 3) if seconds else None,
              "counters": {}, "resets": []}
    if not result["ready"]:
        return result
    for key, value in metrics.items():
        before = previous.get(key, "")
        if not counter(key) or not value.isdecimal() or not before.isdecimal() or max(len(value), len(before)) > 20:
            continue
        port = re.match(r"(port_\d+)_", key)
        if port and any(metrics.get(port[1] + suffix) != previous.get(port[1] + suffix)
                        for suffix in ("_name", "_generation")):
            result["resets"].append(key)
            continue
        if int(value) < int(before):
            result["resets"].append(key)
        else:
            result["counters"][key] = str(int(value) - int(before))
    return result


def is_error(key):
    # Deliberate policy decisions and scheduler reconfiguration are not faults.
    if key in {"policy_drops", "rewrite_drops", "reconfiguration_drops", "shutdown_drops"}:
        return False
    return bool(key.startswith("drops_") or key.endswith(("_drops", "_errors", "_disconnects"))
                or key in {"route_misses", "target_disconnected", "malformed_frames", "pool_stalls", "rtt_lost"})


def health(endpoint, sample):
    metrics, delta = sample.get("metrics", {}), sample.get("changes", {})
    checks = [{"key": "process", "state": "warn" if endpoint.state in {"T", "t", "D"} else "ok",
               "code": "process_blocked" if endpoint.state in {"T", "t", "D"} else "process_running",
               "value": endpoint.state}]
    if sample["status"] != "reachable":
        checks += [{"key": key, "state": "unknown", "code": "telemetry_missing"}
                   for key in ("session", "traffic", "errors")]
    else:
        flags = ["session_ready"] if endpoint.kind == "tunnel" else []
        if endpoint.switch_socket and endpoint.kind != "switch":
            flags += ["divert_in_connected", "divert_out_connected"] if endpoint.kind == "divert" else ["switch_connected"]
        values = {key: metrics.get(key) for key in flags}
        connected = all(value == "1" for value in values.values())
        disconnected = any(value == "0" for value in values.values())
        checks.append({"key": "session", "state": "warn" if disconnected else "ok" if connected else "unknown",
                       "code": "session_down" if disconnected else "session_up" if flags and connected else
                               "session_unknown" if flags else "session_na", "values": values})
        prefix = "udp" if endpoint.kind == "tunnel" else "switch"
        rates = [metrics.get(f"{prefix}_{direction}_bps_5s") for direction in ("rx", "tx")]
        def valid_rate(value):
            try:
                return math.isfinite(float(value)) and float(value) >= 0
            except (TypeError, ValueError):
                return False
        known = all(valid_rate(value) for value in rates)
        active = known and any(float(value) > 0 for value in rates)
        checks.append({"key": "traffic", "state": "ok" if known else "unknown",
                       "code": "traffic_active" if active else "traffic_idle" if known else "traffic_unknown"})
        faults = {key: value for key, value in delta.get("counters", {}).items() if is_error(key) and int(value) > 0}
        checks.append({"key": "errors", "state": "warn" if faults else "ok" if delta.get("ready") else "unknown",
                       "code": "errors_growing" if faults else "errors_clear" if delta.get("ready") else "delta_waiting",
                       "counters": faults})
    level = "warn" if any(c["state"] == "warn" for c in checks) else (
        "unknown" if any(c["state"] == "unknown" for c in checks) else "ok")
    return {"level": level, "checks": checks}


def switch_detail(metrics, delta):
    def grouped(prefix):
        groups = {}
        for key, value in metrics.items():
            match = re.fullmatch(prefix + r"_(\d{1,6})_(.+)", key)
            if match:
                groups.setdefault(int(match[1]), {"index": int(match[1])})[match[2]] = value
        return [groups[index] for index in sorted(groups)]
    workers = grouped("worker")
    for worker in workers:
        elapsed = delta.get("interval_seconds")
        cpu = delta.get("counters", {}).get(f"worker_{worker['index']}_cpu_ns")
        worker["cpu_percent"] = round(int(cpu) / (elapsed * 1e9) * 100, 2) if cpu is not None and elapsed else None
    return {"ports": grouped("port"), "workers": workers,
            "queues": {key: metrics[key] for key in ("matrix_queues", "buffers_in_use", "queue_full_drops",
                       "pool_stalls", "send_backpressure_drops", "workers_active", "workers_pool") if key in metrics},
            "queue_occupancy_available": False}
