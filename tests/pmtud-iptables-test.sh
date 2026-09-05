#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage: pmtud-iptables-test.sh --size <bytes> --interface <name> [--remove]

Silently drops IPv4 UDP packets larger than <bytes> on INPUT and OUTPUT.
Run again with --remove and the same arguments to delete the rules.
EOF
}

size=""
interface=""
action="add"

while (( $# > 0 )); do
    case "$1" in
        --size)
            (( $# >= 2 )) || { usage; exit 1; }
            size="$2"
            shift 2
            ;;
        --interface)
            (( $# >= 2 )) || { usage; exit 1; }
            interface="$2"
            shift 2
            ;;
        --remove)
            action="remove"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            exit 1
            ;;
    esac
done

if ! [[ "$size" =~ ^[0-9]+$ ]] || (( size < 68 || size >= 65535 )); then
    echo "--size must be an integer in range 68..65534" >&2
    exit 1
fi

if [[ -z "$interface" ]]; then
    echo "--interface is required" >&2
    exit 1
fi

if ! ip link show dev "$interface" >/dev/null 2>&1; then
    echo "Network interface does not exist: $interface" >&2
    exit 1
fi

if ! command -v iptables >/dev/null 2>&1; then
    echo "iptables is not installed" >&2
    exit 1
fi

if (( EUID == 0 )); then
    root_cmd=()
else
    root_cmd=(sudo)
fi

length_range="$((size + 1)):65535"
comment="tuntom-pmtud-test"

input_rule=(
    INPUT -i "$interface"
    -p udp
    -m length --length "$length_range"
    -m comment --comment "$comment"
    -j DROP
)

output_rule=(
    OUTPUT -o "$interface"
    -p udp
    -m length --length "$length_range"
    -m comment --comment "$comment"
    -j DROP
)

add_rule() {
    local -n rule="$1"

    if "${root_cmd[@]}" iptables -C "${rule[@]}" 2>/dev/null; then
        return
    fi

    "${root_cmd[@]}" iptables -I "${rule[@]}"
}

remove_rule() {
    local -n rule="$1"

    while "${root_cmd[@]}" iptables -C "${rule[@]}" 2>/dev/null; do
        "${root_cmd[@]}" iptables -D "${rule[@]}"
    done
}

if [[ "$action" == "add" ]]; then
    add_rule input_rule
    add_rule output_rule
    echo "Dropping IPv4 UDP packets larger than ${size} bytes on ${interface} INPUT/OUTPUT"
else
    remove_rule input_rule
    remove_rule output_rule
    echo "Removed PMTUD test rules for ${interface} and size ${size}"
fi

