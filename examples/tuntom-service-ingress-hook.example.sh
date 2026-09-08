#!/usr/bin/env bash
set -euo pipefail

# tuntom: publish a private HTTPS service through a public edge gateway
# ===================================================================
#
# A small site hosts an HTTPS service on a private VLAN. A public gateway
# accepts client connections and forwards them over tuntom. TLS terminates
# at the service, which retains the original client IP in its access logs.
#
#   Internet client (203.0.113.25)
#                 |
#                 | HTTPS to 198.51.100.42:443
#                 v
#   +-------------------------------------+
#   | Public edge gateway (remote/server) |
#   | ens3: 198.51.100.42                  |
#   | DNAT -> 10.80.20.10:443              |
#   | ut42s: 10.254.42.2                   |
#   +-----------------+-------------------+
#                     | tuntom 42 / UDP
#   +-----------------+-------------------+
#   | Site router (local/client)          |
#   | ut42c: 10.254.42.1                   |
#   | br-services: 10.80.20.1/24           |
#   +-----------------+-------------------+
#                     | service VLAN: 10.80.20.0/24
#                     v
#             HTTPS service: 10.80.20.10:443
#             default gateway: 10.80.20.1
#
# Request: client -> edge DNAT -> tunnel -> site router -> HTTPS service.
# Reply:   service -> site policy table -> tunnel -> edge conntrack, which
#          reverses DNAT -> client. No SNAT is used on the tunnel path.
#
# The service host uses this edge for off-subnet traffic. Other hosts on
# the VLAN retain the site's normal routing. Add routes to the tunnel's
# policy table if the service also needs access to other private subnets.
# This example dedicates one edge/tunnel to the service; multiple ingress
# gateways need a connection-aware return policy instead of this source rule.
#
# Before use:
# - Replace the documentation addresses and interface names below. The edge
#   address must be assigned to ens3; DNAT matches that destination exactly.
# - Configure the service VLAN, router address and service default gateway.
# - Allow the tunnel UDP port on the edge (40042 for tunnel 42) and forwarded
#   HTTPS traffic plus established replies in both hosts' filter policies.
#   The hook adds routing and DNAT, not filter/FORWARD accept rules.
# - Ensure the hosts' reverse-path filtering permits this routed topology.
#   Forwarding and mark-aware source validation enabled here persist on stop.
#
# Run on the site router from the repository directory. The SAME local file
# must be configured for both phases; mk_tunnel.sh streams it over SSH for
# execution on the edge, so no remote copy is needed:
#
#   TUNTOM_PRE_HOOK="$PWD/examples/tuntom-service-ingress-hook.example.sh" \
#   TUNTOM_POST_HOOK="$PWD/examples/tuntom-service-ingress-hook.example.sh" \
#     ./mk_tunnel.sh 42 root@198.51.100.42 --no-snat
#
# Use the same hook settings with --stop so pre/down removes custom policy.
# post/up runs after tuntom-net.sh creates its table and NAT chain; pre/down
# runs before their removal. All other lifecycle combinations are no-ops.
# Hook context is documented in docs/DETAILS.md under "Lifecycle hooks".

SERVICE_CIDR="10.80.20.0/24"
SERVICE_IP="10.80.20.10"
SERVICE_PORT="443"
SITE_SERVICE_IF="br-services"

EDGE_WAN_IF="ens3"
EDGE_PUBLIC_IP="198.51.100.42"
EDGE_PUBLIC_PORT="443"

# Reserve these priorities for this tunnel, ahead of the main routing table.
RULE_PREF_IIF=$((20000 + TUNTOM_ID * 2))
RULE_PREF_SRC=$((20001 + TUNTOM_ID * 2))

rule_del() {
    # Remove only exact matching rules, including duplicates from prior runs.
    while ip rule del "$@" 2>/dev/null; do
        :
    done
}

local_up() {
    # The helper supplies "default dev $TUNTOM_IF". Keep the service VLAN
    # reachable on the site side when this policy table is selected.
    ip route replace table "$TUNTOM_TABLE" \
        "$SERVICE_CIDR" dev "$SITE_SERVICE_IF"

    rule_del pref "$RULE_PREF_IIF" iif "$TUNTOM_IF" lookup "$TUNTOM_TABLE"
    ip rule add pref "$RULE_PREF_IIF" \
        iif "$TUNTOM_IF" lookup "$TUNTOM_TABLE"

    # Only the published service uses this edge as its off-subnet return path.
    rule_del pref "$RULE_PREF_SRC" from "${SERVICE_IP}/32" lookup "$TUNTOM_TABLE"
    ip rule add pref "$RULE_PREF_SRC" \
        from "${SERVICE_IP}/32" lookup "$TUNTOM_TABLE"

    sysctl -w net.ipv4.ip_forward=1 >/dev/null
    sysctl -w net.ipv4.conf.all.src_valid_mark=1 >/dev/null
}

local_down() {
    rule_del pref "$RULE_PREF_IIF" iif "$TUNTOM_IF" lookup "$TUNTOM_TABLE"
    rule_del pref "$RULE_PREF_SRC" from "${SERVICE_IP}/32" lookup "$TUNTOM_TABLE"
    ip route del table "$TUNTOM_TABLE" \
        "$SERVICE_CIDR" dev "$SITE_SERVICE_IF" 2>/dev/null || true
}

remote_up() {
    # Route only the published service through this tunnel.
    ip route replace "${SERVICE_IP}/32" dev "$TUNTOM_IF"

    # The helper already attached this per-tunnel chain to nat/PREROUTING.
    # Restrict publication to one address, interface and TCP port.
    iptables -t nat -A "$TUNTOM_NAT_CHAIN" \
        -i "$EDGE_WAN_IF" -d "$EDGE_PUBLIC_IP" \
        -p tcp --dport "$EDGE_PUBLIC_PORT" \
        -j DNAT --to-destination "${SERVICE_IP}:${SERVICE_PORT}"

    sysctl -w net.ipv4.ip_forward=1 >/dev/null
}

remote_down() {
    # tuntom-net.sh removes the per-tunnel DNAT chain after pre/down.
    ip route del "${SERVICE_IP}/32" dev "$TUNTOM_IF" 2>/dev/null || true
}

if [[ "$TUNTOM_ACTION:$TUNTOM_PHASE" == "up:post" && "$TUNTOM_SNAT" != "0" ]]; then
    echo "Service ingress requires --no-snat to preserve client addresses." >&2
    exit 1
fi

case "${TUNTOM_SIDE}:${TUNTOM_ACTION}:${TUNTOM_PHASE}" in
    local:up:post)    local_up ;;
    local:down:pre)   local_down ;;
    remote:up:post)   remote_up ;;
    remote:down:pre)  remote_down ;;
    *)               : ;;
esac
