#!/usr/bin/env bash
set -euo pipefail

#
# tuntom honeynet hook example
# ============================
#
# The hook file exists ONLY on the caller/local host.
# mk_tunnel.sh runs this same file twice:
#
#   TUNTOM_SIDE=local    on the core/caller host
#   TUNTOM_SIDE=remote   on the public/remote probe host (streamed over SSH)
#
#
# Example topology
# ----------------
#
#                         Internet
#                            |
#                            | TCP/443
#                            v
#                    +----------------+
#                    | remote probe   |
#                    | WAN: ens3      |
#                    | TUN: ut42s     |
#                    +-------+--------+
#                            |
#                            | tuntom 42
#                            |
#                    +-------+--------+
#                    | core / local   |
#                    | TUN: ut42c     |
#                    | br-honeynet    |
#                    +-------+--------+
#                            |
#                      10.66.42.0/24
#                            |
#                       10.66.42.10
#                       honeypot :443
#
#
# Desired flow
# ------------
#
#   client 1.2.3.4
#        |
#        | dst remote-public-ip:443
#        v
#   remote/ens3
#        |
#        | DNAT -> 10.66.42.10:443
#        | route 10.66.42.0/24 via $TUNTOM_IF
#        v
#   tuntom
#        |
#        v
#   core/local
#        |
#        | route 10.66.42.0/24 via br-honeynet
#        v
#   10.66.42.10:443
#
# Reply:
#
#   10.66.42.10 -> 1.2.3.4
#        |
#        | source-policy route -> tuntom
#        v
#   remote
#        |
#        | conntrack reverses DNAT
#        v
#   Internet client
#
#
# Why the policy routing on the core side matters
# ------------------------------------------------
#
# We preserve the real Internet source address; there is no SNAT on the
# remote probe. Therefore the honeypot sees the original client IP.
#
# The core must consequently know that replies sourced from HONEYNET_CIDR
# belong back through this tunnel instead of its ordinary default route.
#
# We also add an iif rule for packets entering from the TUN. The tuntom
# routing table already has "default dev $TUNTOM_IF"; we add the directly
# reachable honeynet route to that same table. This makes the routing view
# internally consistent for traffic entering the tunnel and is suitable for
# strict reverse-path validation.
#
#
# Supported environment variables
# -------------------------------
#
# TUNTOM_ID
#     Numeric tunnel ID.
#
# TUNTOM_ACTION
#     up | down
#
# TUNTOM_PHASE
#     pre | post
#
# TUNTOM_SIDE
#     local | remote
#
# TUNTOM_IF
#     TUN interface on the side where this hook is currently running.
#
# TUNTOM_LOCAL_IP
#     Tunnel IP on the side where this hook is currently running.
#
# TUNTOM_PEER_IP
#     Tunnel IP on the opposite side.
#
# TUNTOM_CLIENT_IP
#     Caller/local-side tunnel IP.
#
# TUNTOM_SERVER_IP
#     Remote-side tunnel IP.
#
# TUNTOM_UDP_PORT
#     UDP transport port used by this tunnel.
#
# TUNTOM_MTU
#     Tunnel MTU.
#
# TUNTOM_SNAT
#     0 | 1. Whether tuntom's standard MASQUERADE/SNAT is enabled.
#
# TUNTOM_MSS_CLAMP
#     0 | 1. Whether tuntom TCP MSS clamping is enabled.
#
# TUNTOM_MARK
#     fwmark value reserved for this tunnel.
#
# TUNTOM_MARK_MASK
#     fwmark mask reserved for tuntom.
#
# TUNTOM_TABLE
#     Policy-routing table number assigned to this tunnel.
#
# TUNTOM_CHAIN
#     Base per-tunnel chain name, e.g. TUNTOM_42.
#
# TUNTOM_NAT_CHAIN
#     Per-tunnel nat/PREROUTING chain, e.g. TUNTOM_42_N.
#
# TUNTOM_SNAT_CHAIN
#     Per-tunnel nat/POSTROUTING chain, e.g. TUNTOM_42_S.
#
# TUNTOM_MANGLE_CHAIN
#     Per-tunnel mangle/PREROUTING chain, e.g. TUNTOM_42_M.
#
# TUNTOM_FORWARD_CHAIN
#     Per-tunnel mangle/FORWARD chain, e.g. TUNTOM_42_F.
#
#
# Site-specific configuration
# ---------------------------
#
# These are deliberately ordinary shell variables: copy this example and
# edit them for the site.
#

HONEYNET_CIDR="10.66.42.0/24"
HONEYPOT_IP="10.66.42.10"
HONEYPOT_PORT="443"

# Interface on the LOCAL/core host leading to the honeynet.
HONEYNET_IF="br-honeynet"

# Public/non-tuntom ingress interface on the REMOTE probe.
REMOTE_INGRESS_IF="ens3"

# Port exposed by the REMOTE probe.
REMOTE_PUBLIC_PORT="443"

# Rule priorities. Keep them deterministic per tunnel.
RULE_PREF_IIF=$((20000 + TUNTOM_ID * 2))
RULE_PREF_SRC=$((20001 + TUNTOM_ID * 2))


rule_del() {
    # Delete all exact occurrences, making repeated down/start safe.
    while ip rule del "$@" 2>/dev/null; do
        :
    done
}


local_up() {
    #
    # tuntom-net.sh already created:
    #
    #   table $TUNTOM_TABLE:
    #       default dev $TUNTOM_IF
    #
    # Add the route that must NOT go back into the tunnel.
    #
    ip route replace table "$TUNTOM_TABLE" \
        "$HONEYNET_CIDR" dev "$HONEYNET_IF"

    #
    # Packets arriving FROM the remote probe should use the tunnel-specific
    # routing view. This also gives reverse-path validation a routing table
    # in which arbitrary Internet sources resolve back through $TUNTOM_IF.
    #
    rule_del pref "$RULE_PREF_IIF"
    ip rule add pref "$RULE_PREF_IIF" \
        iif "$TUNTOM_IF" \
        lookup "$TUNTOM_TABLE"

    #
    # Replies originating inside the honeynet must return through this
    # particular remote probe/tunnel, not through the core's normal default.
    #
    rule_del pref "$RULE_PREF_SRC"
    ip rule add pref "$RULE_PREF_SRC" \
        from "$HONEYNET_CIDR" \
        lookup "$TUNTOM_TABLE"

    #
    # Keep strict RPF useful instead of simply disabling it.
    #
    sysctl -w net.ipv4.conf.all.src_valid_mark=1 >/dev/null
}


local_down() {
    rule_del pref "$RULE_PREF_IIF"
    rule_del pref "$RULE_PREF_SRC"

    # tuntom-net.sh will flush/delete the tunnel table afterwards, but
    # explicitly removing our route makes this hook safe on its own too.
    ip route del table "$TUNTOM_TABLE" \
        "$HONEYNET_CIDR" dev "$HONEYNET_IF" 2>/dev/null || true
}


remote_up() {
    #
    # Remote probe must know that the honeynet lives behind tuntom.
    # This route also makes RPF for packets returning from the honeynet
    # consistent: source 10.66.42.x resolves through the TUN.
    #
    ip route replace "$HONEYNET_CIDR" dev "$TUNTOM_IF"

    #
    # Internet-facing DNAT.
    #
    # TUNTOM_NAT_CHAIN is created and hooked into nat/PREROUTING by
    # tuntom-net.sh before post/up is called.
    #
    # Original source address is preserved deliberately.
    #
    iptables -t nat -A "$TUNTOM_NAT_CHAIN" \
        -i "$REMOTE_INGRESS_IF" \
        -p tcp \
        --dport "$REMOTE_PUBLIC_PORT" \
        -j DNAT \
        --to-destination "${HONEYPOT_IP}:${HONEYPOT_PORT}"

    #
    # IP forwarding is needed on the probe.
    #
    sysctl -w net.ipv4.ip_forward=1 >/dev/null
}


remote_down() {
    #
    # DNAT cleanup is automatic: tuntom-net.sh removes TUNTOM_NAT_CHAIN.
    # Only the ordinary route belongs to us explicitly.
    #
    ip route del "$HONEYNET_CIDR" dev "$TUNTOM_IF" 2>/dev/null || true
}


#
# Lifecycle
# ---------
#
# post/up:
#   tuntom-net.sh has already created routes/chains, so this is where the
#   honeynet policy is installed.
#
# pre/down:
#   remove policy while the TUN and tuntom routing table still exist.
#

case "${TUNTOM_SIDE}:${TUNTOM_ACTION}:${TUNTOM_PHASE}" in
    local:up:post)
        local_up
        ;;

    local:down:pre)
        local_down
        ;;

    remote:up:post)
        remote_up
        ;;

    remote:down:pre)
        remote_down
        ;;

    *)
        # Nothing to do for the other lifecycle combinations.
        ;;
esac
