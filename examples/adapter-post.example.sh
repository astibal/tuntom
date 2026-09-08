#!/usr/bin/env bash
set -euo pipefail

# Run with --post-hook examples/adapter-post.example.sh after adjusting these
# example addresses/routes for your topology. The TUN is already UP on post/up.
case "$TUNTOM_PHASE/$TUNTOM_ACTION" in
    post/up)
        ip address replace 10.253.0.1/32 dev "$TUNTOM_IF"
        ip route replace 10.42.0.0/16 dev "$TUNTOM_IF"
        ;;
    post/down)
        # The nonpersistent TUN and its interface routes disappear on exit.
        # Remove any extra policy rules/firewall state you add here as well.
        ;;
esac
