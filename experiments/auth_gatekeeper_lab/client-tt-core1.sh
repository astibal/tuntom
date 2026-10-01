#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUILD=${TUNTOM_CLIENT_BUILD:-/tmp/tuntom-auth-gatekeeper-build}
HOST=${TUNTOM_SERVER_HOST:-192.168.155.169}
TUNNEL_ID=${TUNTOM_TUNNEL_ID:-33}
SECRET_FILE=${TUNTOM_SECRET_FILE:-$HOME/.config/tuntom/tt-core1-auth.secret}
RUNTIME=${TUNTOM_CLIENT_RUNTIME:-/tmp/tuntom-tt-core1-client}

if [ ! -r "$SECRET_FILE" ]; then
    echo "Missing tunnel secret: $SECRET_FILE" >&2
    echo "Set TUNTOM_SECRET_FILE or copy the secret provisioned by the server." >&2
    exit 1
fi
TUNTOM_SECRET=$(tr -d '\r\n' <"$SECRET_FILE")
export TUNTOM_SECRET
mkdir -p "$RUNTIME"
rm -f "$RUNTIME/relay.sock" "$RUNTIME/control.sock"

cmake -S "$ROOT" -B "$BUILD" -DBUILD_TESTING=OFF >/dev/null
cmake --build "$BUILD" -j2 --target tuntom >/dev/null

echo "Connecting alice to $HOST (Ctrl-C stops the client)"
exec "$BUILD/tuntom" client "$TUNNEL_ID" - "$HOST" \
    --relay-listen "$RUNTIME/relay.sock" \
    --control-socket "$RUNTIME/control.sock" \
    --auth-username alice \
    --auth-response-command "$ROOT/experiments/auth_gatekeeper_lab/password_responder.py" \
    --auth-timeout 5 --no-stats --no-pmtud
