#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUILD=${TUNTOM_CLIENT_BUILD:-/tmp/tuntom-auth-gatekeeper-build}
HOST=${TUNTOM_SERVER_HOST:-192.168.155.169}
TUNNEL_ID=${TUNTOM_TUNNEL_ID:-33}
INTERFACE=${TUNTOM_INTERFACE:-ttauth0}
SECRET_FILE=${TUNTOM_SECRET_FILE:-/home/astib/.config/tuntom/tt-core1-auth.secret}
RUNTIME=${TUNTOM_CLIENT_RUNTIME:-/run/tuntom-auth-client}
HELPER=$ROOT/experiments/auth_gatekeeper_lab/config_root_helper.py
RESPONDER=$ROOT/experiments/auth_gatekeeper_lab/password_responder.py

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this launcher with sudo:" >&2
    echo "  sudo $0" >&2
    exit 1
fi
test -r "$SECRET_FILE" || { echo "Missing secret: $SECRET_FILE" >&2; exit 1; }
cmake -S "$ROOT" -B "$BUILD" -DBUILD_TESTING=OFF >/dev/null
cmake --build "$BUILD" -j2 --target tuntom >/dev/null
TUNTOM_SECRET=$(tr -d '\r\n' <"$SECRET_FILE")
export TUNTOM_SECRET
TUNTOM_CONFIG_SOCKET=$RUNTIME/config.sock
export TUNTOM_CONFIG_SOCKET
install -d -o root -g tuntom -m 0750 "$RUNTIME"
rm -f "$TUNTOM_CONFIG_SOCKET" "$RUNTIME/client.control"

"$HELPER" daemon "$TUNTOM_CONFIG_SOCKET" "$INTERFACE" "$HOST" >"$RUNTIME/config.log" 2>&1 &
helper_pid=$!
cleanup() { kill "$helper_pid" 2>/dev/null || true; wait "$helper_pid" 2>/dev/null || true; }
trap cleanup EXIT INT TERM
i=0
while [ ! -S "$TUNTOM_CONFIG_SOCKET" ]; do i=$((i + 1)); [ "$i" -lt 50 ] || exit 1; sleep 0.02; done

echo "Connecting $INTERFACE to $HOST using tunnel ID $TUNNEL_ID"
"$BUILD/tuntom" client "$TUNNEL_ID" "$INTERFACE" "$HOST" \
    --control-socket "$RUNTIME/client.control" \
    --auth-username alice --auth-response-command "$RESPONDER" \
    --config-command "$HELPER" --auth-timeout 5 --no-stats --no-pmtud
