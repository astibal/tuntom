#!/usr/bin/env bash
set -euo pipefail

#
# tuntom-net.sh
#
# Idempotent networking helper for tuntom lifecycle hooks.
#
# Required environment:
#   TUNTOM_ID
#   TUNTOM_IF
#
# Optional:
#   TUNTOM_MARK_MASK   default: 0x00ff0000
#   TUNTOM_MARK        default: TUNTOM_ID << 16
#   TUNTOM_TABLE       default: 10000 + TUNTOM_ID
#   TUNTOM_CHAIN       default: TUNTOM_<id>
#   TUNTOM_MSS_CLAMP   default: 1
#   TUNTOM_SNAT        default: 0
#
# This file is meant to be sourced by mk_tunnel.sh or custom hooks.
#

: "${TUNTOM_ID:?TUNTOM_ID is required}"
: "${TUNTOM_IF:?TUNTOM_IF is required}"

if ! [[ "$TUNTOM_ID" =~ ^[0-9]+$ ]] || (( TUNTOM_ID < 1 || TUNTOM_ID > 255 )); then
    echo "TUNTOM_ID must be in range 1..255" >&2
    exit 1
fi

TUNTOM_MARK_MASK="${TUNTOM_MARK_MASK:-0x00ff0000}"
TUNTOM_MARK="${TUNTOM_MARK:-$((TUNTOM_ID << 16))}"
TUNTOM_TABLE="${TUNTOM_TABLE:-$((10000 + TUNTOM_ID))}"
TUNTOM_CHAIN="${TUNTOM_CHAIN:-TUNTOM_${TUNTOM_ID}}"

# Defaults:
# - MSS clamping ON
# - MASQUERADE OFF
TUNTOM_MSS_CLAMP="${TUNTOM_MSS_CLAMP:-1}"
TUNTOM_SNAT="${TUNTOM_SNAT:-0}"

iptables_ensure_chain() {
    local table="$1"
    local chain="$2"

    iptables -t "$table" -N "$chain" 2>/dev/null || true
}

iptables_flush_chain() {
    local table="$1"
    local chain="$2"

    iptables -t "$table" -F "$chain"
}

iptables_ensure_jump() {
    local table="$1"
    local parent="$2"
    local child="$3"

    if ! iptables -t "$table" -C "$parent" -j "$child" 2>/dev/null; then
        iptables -t "$table" -A "$parent" -j "$child"
    fi
}

iptables_delete_jump() {
    local table="$1"
    local parent="$2"
    local child="$3"

    while iptables -t "$table" -C "$parent" -j "$child" 2>/dev/null; do
        iptables -t "$table" -D "$parent" -j "$child"
    done
}

iptables_delete_chain() {
    local table="$1"
    local chain="$2"

    iptables -t "$table" -F "$chain" 2>/dev/null || true
    iptables -t "$table" -X "$chain" 2>/dev/null || true
}

ip_rule_delete() {
    local mark="$1"
    local mask="$2"
    local table="$3"

    while ip rule del fwmark "${mark}/${mask}" table "$table" 2>/dev/null; do
        :
    done
}

tuntom_net_up() {
    local mangle_chain="${TUNTOM_CHAIN}_M"
    local forward_chain="${TUNTOM_CHAIN}_F"
    local nat_chain="${TUNTOM_CHAIN}_N"
    local snat_chain="${TUNTOM_CHAIN}_S"

    #
    # PREROUTING mark handling
    #
    iptables_ensure_chain mangle "$mangle_chain"
    iptables_flush_chain mangle "$mangle_chain"
    iptables_ensure_jump mangle PREROUTING "$mangle_chain"

    iptables -t mangle -A "$mangle_chain" \
        -j CONNMARK --restore-mark \
        --nfmask "$TUNTOM_MARK_MASK" \
        --ctmask "$TUNTOM_MARK_MASK"

    iptables -t mangle -A "$mangle_chain" \
        -i "$TUNTOM_IF" \
        -m conntrack --ctstate NEW \
        -j CONNMARK --set-xmark "${TUNTOM_MARK}/${TUNTOM_MARK_MASK}"

    iptables -t mangle -A "$mangle_chain" \
        -i "$TUNTOM_IF" \
        -j CONNMARK --restore-mark \
        --nfmask "$TUNTOM_MARK_MASK" \
        --ctmask "$TUNTOM_MARK_MASK"

    #
    # FORWARD helpers
    #
    iptables_ensure_chain mangle "$forward_chain"
    iptables_flush_chain mangle "$forward_chain"
    iptables_ensure_jump mangle FORWARD "$forward_chain"

    if [[ "$TUNTOM_MSS_CLAMP" == "1" ]]; then
        iptables -t mangle -A "$forward_chain" \
            -o "$TUNTOM_IF" \
            -p tcp \
            --tcp-flags SYN,RST SYN \
            -j TCPMSS --clamp-mss-to-pmtu

        iptables -t mangle -A "$forward_chain" \
            -i "$TUNTOM_IF" \
            -p tcp \
            --tcp-flags SYN,RST SYN \
            -j TCPMSS --clamp-mss-to-pmtu
    fi

    #
    # DNAT hook chain
    #
    iptables_ensure_chain nat "$nat_chain"
    iptables_flush_chain nat "$nat_chain"
    iptables_ensure_jump nat PREROUTING "$nat_chain"

    #
    # Optional lab SNAT / MASQUERADE
    #
    iptables_ensure_chain nat "$snat_chain"
    iptables_flush_chain nat "$snat_chain"
    iptables_ensure_jump nat POSTROUTING "$snat_chain"

    if [[ "$TUNTOM_SNAT" == "1" ]]; then
        iptables -t nat -A "$snat_chain" \
            -o "$TUNTOM_IF" \
            -j MASQUERADE
    fi

    #
    # Policy routing for reply traffic
    #
    ip_rule_delete "$TUNTOM_MARK" "$TUNTOM_MARK_MASK" "$TUNTOM_TABLE"
    ip rule add fwmark "${TUNTOM_MARK}/${TUNTOM_MARK_MASK}" table "$TUNTOM_TABLE"

    ip route replace table "$TUNTOM_TABLE" default dev "$TUNTOM_IF"
}

tuntom_net_down() {
    local mangle_chain="${TUNTOM_CHAIN}_M"
    local forward_chain="${TUNTOM_CHAIN}_F"
    local nat_chain="${TUNTOM_CHAIN}_N"
    local snat_chain="${TUNTOM_CHAIN}_S"

    ip_rule_delete "$TUNTOM_MARK" "$TUNTOM_MARK_MASK" "$TUNTOM_TABLE"
    ip route flush table "$TUNTOM_TABLE" 2>/dev/null || true

    iptables_delete_jump mangle PREROUTING "$mangle_chain"
    iptables_delete_chain mangle "$mangle_chain"

    iptables_delete_jump mangle FORWARD "$forward_chain"
    iptables_delete_chain mangle "$forward_chain"

    iptables_delete_jump nat PREROUTING "$nat_chain"
    iptables_delete_chain nat "$nat_chain"

    iptables_delete_jump nat POSTROUTING "$snat_chain"
    iptables_delete_chain nat "$snat_chain"
}

tuntom_nat_chain() {
    echo "${TUNTOM_CHAIN}_N"
}

tuntom_snat_chain() {
    echo "${TUNTOM_CHAIN}_S"
}

tuntom_mangle_chain() {
    echo "${TUNTOM_CHAIN}_M"
}

tuntom_forward_chain() {
    echo "${TUNTOM_CHAIN}_F"
}
