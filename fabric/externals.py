"""Live, non-persistent observations from a Fabric Peek endpoint."""
from __future__ import annotations

import json
import hashlib
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen
from syspiper import PATHS, clean_snapshot


MAX_REPLY = 2 * 1024 * 1024
MAX_TARGETS_PER_REQUEST = 64


class PeekError(Exception):
    pass


def observe(services: list[dict], base_url: str | None, token: str | None, timeout: float = 25) -> dict:
    targets, context = [], {}
    for service in services:
        for target in service.get("peek_targets", []):
            target_id = service["id"]+":"+hashlib.sha256(target["url"].encode()).hexdigest()[:16]
            targets.append({"id": target_id, "url": target["url"],"interval":target["interval"]})
            context[target_id] = {
                "service_id": service["id"], "service_name": service["name"],
                "service_kind": service["kind"], "service_labels": service.get("labels", []),
                "service_description": service.get("description", ""), "interval": target["interval"],
            }
    if not targets:
        return {"configured": bool(base_url and token), "sampled_at": None, "observations": []}
    if not base_url or not token:
        raise PeekError("Peek není nakonfigurovaný ve Fabric serveru")
    observations, sampled_at, history = [], None, {}
    for offset in range(0, len(targets), MAX_TARGETS_PER_REQUEST):
        body = json.dumps({"targets": targets[offset:offset + MAX_TARGETS_PER_REQUEST]}, separators=(",", ":")).encode()
        request = Request(base_url.rstrip("/") + "/v1/probe", data=body, headers={
            "Authorization": "Bearer " + token, "Content-Type": "application/json",
        })
        try:
            with urlopen(request, timeout=timeout) as response:
                raw = response.read(MAX_REPLY + 1)
        except HTTPError as error:
            raise PeekError(f"Peek odmítl požadavek (HTTP {error.code})") from error
        except (URLError, TimeoutError, OSError) as error:
            raise PeekError(f"Peek není dostupný: {error}") from error
        if len(raw) > MAX_REPLY:
            raise PeekError("odpověď Peek překročila 2 MiB")
        try:
            result = json.loads(raw)
            rows = result["results"]
            if not isinstance(rows, list):
                raise ValueError
            observations.extend({**context[row["id"]], **row} for row in rows if row.get("id") in context)
            for target_id,samples in result.get("history",{}).items():
                if target_id in context:history[target_id]=samples
            sampled_at = result.get("observed_at") or sampled_at
        except (ValueError, KeyError, TypeError) as error:
            raise PeekError("Peek vrátil neplatnou odpověď") from error
    return {"configured": True, "sampled_at": sampled_at, "observations": observations,"history":history}


def peek_health(base_url: str | None, timeout: float = 5) -> dict:
    if not base_url:return {"configured":False,"status":"not_configured"}
    try:
        with urlopen(base_url.rstrip("/")+"/healthz",timeout=timeout) as response:raw=response.read(64*1024)
        data=json.loads(raw)
        if not isinstance(data,dict) or data.get("status")!="ok":raise ValueError
        return {"configured":True,"endpoint":base_url,"status":"ok",**data}
    except (URLError,TimeoutError,OSError,ValueError,json.JSONDecodeError) as error:
        return {"configured":True,"endpoint":base_url,"status":"unavailable","error":str(error)}


def peek_syspiper(base_url: str | None, token: str | None) -> dict:
    health=peek_health(base_url)
    if health.get("status")!="ok" or not health.get("capabilities",{}).get("syspiper_self"):
        return {**health,"syspiper":None}
    results,errors={},{}
    for path in PATHS:
        request=Request(base_url.rstrip("/")+"/syspiper/self/"+path,headers={"Authorization":"Bearer "+token,"Accept":"application/json"})
        try:
            with urlopen(request,timeout=15 if path=="apt" else 5) as response:data=json.load(response)
            if not isinstance(data,dict) or data.get("status") not in ("ok","partial"):raise ValueError
            results[path]=data
        except HTTPError as error:errors[path]="unauthorized" if error.code in (401,403) else "unsupported" if error.code==404 else "http_error"
        except (URLError,TimeoutError,OSError,ValueError,json.JSONDecodeError):errors[path]="unavailable"
    try:values=clean_snapshot(results)
    except (ValueError,TypeError,AttributeError,OverflowError):values={};errors["response"]="invalid_response"
    return {**health,"syspiper":{"status":"ok" if results and not errors else "partial" if results else "unavailable","values":values,"errors":errors}}
