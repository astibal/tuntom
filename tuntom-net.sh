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
#   TUNTOM_XTABLES_WAIT default: 5 seconds
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
TUNTOM_XTABLES_WAIT="${TUNTOM_XTABLES_WAIT:-5}"

iptables() {
    command iptables -w "$TUNTOM_XTABLES_WAIT" "$@"
}

iptables_ensure_chain() {
    local table="$1"
    local chain="$2"

    if iptables -t "$table" -nL "$chain" >/dev/null 2>&1; then
        return 0
    fi

    if iptables -t "$table" -N "$chain"; then
        return 0
    fi

    # Another process may have created it between the check and -N.
    iptables -t "$table" -nL "$chain" >/dev/null 2>&1
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

    if ! iptables -t "$table" -C "$parent" -j "$child" >/dev/null 2>&1; then
        iptables -t "$table" -A "$parent" -j "$child"
    fi
}

iptables_delete_jump() {
    local table="$1"
    local parent="$2"
    local child="$3"

    while iptables -t "$table" -C "$parent" -j "$child" >/dev/null 2>&1; do
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

    while ip rule del fwmark "${mark}/${mask}" table "$table" >/dev/null 2>&1; do
        :
    done
}

tuntom_net_up() {
    local mangle_chain="${TUNTOM_CHAIN}_M"
    local forward_chain="${TUNTOM_CHAIN}_F"
    local nat_chain="${TUNTOM_CHAIN}_N"
    local snat_chain="${TUNTOM_CHAIN}_S"

    #
    # PREROUTING connection ownership and reply mark handling
    #
    # Every tracked packet received from the TUN makes this tunnel the return
    # path for its connection, regardless of conntrack state. The most recent
    # TUN ingress therefore wins if a connection is seen through more than one
    # tunnel.
    #
    # Keep the packet mark clear on the ingress packet itself so it follows
    # normal routing towards its destination. Only packets in the other
    # direction restore the connection mark and enter tunnel policy routing.
    #
    iptables_ensure_chain mangle "$mangle_chain"
    iptables_flush_chain mangle "$mangle_chain"
    iptables_ensure_jump mangle PREROUTING "$mangle_chain"

    iptables -t mangle -A "$mangle_chain" \
        -i "$TUNTOM_IF" \
        -m conntrack --ctstate INVALID,UNTRACKED \
        -j DROP

    iptables -t mangle -A "$mangle_chain" \
        -i "$TUNTOM_IF" \
        -j CONNMARK --set-xmark "${TUNTOM_MARK}/${TUNTOM_MARK_MASK}"

    iptables -t mangle -A "$mangle_chain" \
        -i "$TUNTOM_IF" \
        -j MARK --set-xmark "0/${TUNTOM_MARK_MASK}"

    iptables -t mangle -A "$mangle_chain" \
        ! -i "$TUNTOM_IF" \
        -m connmark --mark "${TUNTOM_MARK}/${TUNTOM_MARK_MASK}" \
        -j CONNMARK --restore-mark \
        --nfmask "$TUNTOM_MARK_MASK" \
        --ctmask "$TUNTOM_MARK_MASK"

    if ! iptables -t mangle -C OUTPUT \
        -m connmark --mark "${TUNTOM_MARK}/${TUNTOM_MARK_MASK}" \
        -j CONNMARK --restore-mark \
        --nfmask "$TUNTOM_MARK_MASK" \
        --ctmask "$TUNTOM_MARK_MASK" >/dev/null 2>&1; then
        iptables -t mangle -A OUTPUT \
            -m connmark --mark "${TUNTOM_MARK}/${TUNTOM_MARK_MASK}" \
            -j CONNMARK --restore-mark \
            --nfmask "$TUNTOM_MARK_MASK" \
            --ctmask "$TUNTOM_MARK_MASK"
    fi

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

    while iptables -t mangle -C OUTPUT \
        -m connmark --mark "${TUNTOM_MARK}/${TUNTOM_MARK_MASK}" \
        -j CONNMARK --restore-mark \
        --nfmask "$TUNTOM_MARK_MASK" \
        --ctmask "$TUNTOM_MARK_MASK" >/dev/null 2>&1; do
        iptables -t mangle -D OUTPUT \
            -m connmark --mark "${TUNTOM_MARK}/${TUNTOM_MARK_MASK}" \
            -j CONNMARK --restore-mark \
            --nfmask "$TUNTOM_MARK_MASK" \
            --ctmask "$TUNTOM_MARK_MASK"
    done

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
