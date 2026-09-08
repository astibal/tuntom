#!/usr/bin/env bash
set -euo pipefail

# Run with --pre-hook examples/switch-pre.example.sh.
# pre/up runs before the old switch stops. Generate the complete flow table in
# this private staging file; invalid rules leave the old switch running.
if [[ "$TUNTOM_PHASE/$TUNTOM_ACTION" == pre/up ]]; then
    cat > "$TUNTOM_RULES_FILE" <<'RULES'
exit-port internet
route client-42:17=internet:1001
route internet:1001=client-42:44
default-back off
RULES
fi
