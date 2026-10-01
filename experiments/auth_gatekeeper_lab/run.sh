#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
LAB=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD=${TUNTOM_LAB_BUILD:-/tmp/tuntom-auth-gatekeeper-build}
STATE=$(mktemp -d /tmp/tuntom-auth-gatekeeper.XXXXXX)
SECRET=00112233445566778899aabbccddeeff
export TUNTOM_SECRET=$SECRET

pids=""
cleanup() {
    for pid in $pids; do kill "$pid" 2>/dev/null || true; done
    for pid in $pids; do wait "$pid" 2>/dev/null || true; done
    rm -rf "$STATE"
}
trap cleanup EXIT INT TERM

cmake -S "$ROOT" -B "$BUILD" -DBUILD_TESTING=OFF >/dev/null
cmake --build "$BUILD" -j2 --target tuntom tuntom-gatekeeper tuntom-switch tuntomctl >/dev/null

chmod +x "$LAB/password_verifier.py" "$LAB/password_responder.py"
cat >"$STATE/gatekeeper.conf" <<EOF
auth-command=$LAB/password_verifier.py
auth-timeout=5

[users]
label_prefix=1001,13

[user alice]
label=77
port-id=vpn-alice
address4=10.8.0.2/24
dns4=10.8.0.1
route4=10.9.0.0/24
mtu=1400
EOF

"$BUILD/tuntom-switch" --socket "$STATE/switch.sock" --control-socket "$STATE/switch.control" \
    >"$STATE/switch.log" 2>&1 &
pids="$! $pids"

i=0
while [ ! -S "$STATE/switch.sock" ]; do
    i=$((i + 1)); [ "$i" -lt 100 ] || { echo "switch failed" >&2; exit 1; }
    sleep 0.02
done

"$BUILD/tuntom" server 232 - \
    --switch-socket "$STATE/switch.sock" --switch-port-id auth-listener \
    --switch-stack 1001,13 \
    --auth-command "$BUILD/tuntom-gatekeeper" --auth-config "$STATE/gatekeeper.conf" \
    --auth-timeout 5 --no-stats --no-pmtud >"$STATE/server.log" 2>&1 &
pids="$! $pids"

"$BUILD/tuntom" client 232 - 127.0.0.1 \
    --relay-listen "$STATE/client-relay.sock" \
    --auth-username alice --auth-response-command "$LAB/password_responder.py" \
    --auth-timeout 5 --no-stats --no-pmtud >"$STATE/client.log" 2>&1 &
pids="$! $pids"

i=0
while :; do
    if "$BUILD/tuntomctl" "$STATE/switch.control" show stats 2>/dev/null \
        | grep -q 'port_vpn-alice-'; then
        break
    fi
    for pid in $pids; do kill -0 "$pid" 2>/dev/null || { echo "lab process failed; logs: $STATE" >&2; trap - EXIT; exit 1; }; done
    i=$((i + 1)); [ "$i" -lt 200 ] || { echo "AUTH/RELOCATE timeout; logs: $STATE" >&2; trap - EXIT; exit 1; }
    sleep 0.05
done

echo "AUTH lab is up"
echo "  user:          alice"
echo "  switch stack:  [1001,13,77]"
echo "  client CONFIG: 10.8.0.2/24, DNS 10.8.0.1, MTU 1400"
echo "  runtime:       $STATE"
"$BUILD/tuntomctl" "$STATE/switch.control" show stats | grep -E 'port_(auth-listener|vpn-alice-)'

if [ "${1:-}" = "--check" ]; then exit 0; fi
echo "Press Ctrl-C to stop."
while :; do sleep 10; done
