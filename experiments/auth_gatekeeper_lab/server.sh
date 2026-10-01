#!/bin/sh
set -eu

. /etc/tuntom/default.conf
: "${TUNTOM_ROOT:?TUNTOM_ROOT is not set}"

BINARY_DIR=${TUNTOM_BINARY_DIR:-$TUNTOM_ROOT/distrib/tuntom/build-lab}
SWITCH_SOCKET=${TUNTOM_SWITCH_SOCKET:-$TUNTOM_ROOT/run/switch.sock}
TUNNEL_ID=${TUNTOM_TUNNEL_ID:-33}
SECRET_FILE=${TUNTOM_SECRET_FILE:-$TUNTOM_ROOT/etc/access-$TUNNEL_ID.env}
CONFIG_FILE=${TUNTOM_AUTH_CONFIG:-$TUNTOM_ROOT/etc/access-$TUNNEL_ID.conf}
RUNTIME=${TUNTOM_SERVER_RUNTIME:-$TUNTOM_ROOT/run}

TUNTOM_SECRET=$(tr -d '\r\n' <"$SECRET_FILE")
export TUNTOM_SECRET
mkdir -p "$RUNTIME"
rm -f "$RUNTIME/server.control"

exec "$BINARY_DIR/tuntom" server "$TUNNEL_ID" - \
    --switch-socket "$SWITCH_SOCKET" --switch-port-id auth-listener \
    --switch-stack 1001,13 \
    --auth-command "$BINARY_DIR/tuntom-gatekeeper" \
    --auth-config "$CONFIG_FILE" \
    --control-socket "$RUNTIME/access-$TUNNEL_ID.control" \
    --auth-timeout 5 --no-stats --no-pmtud
