#!/usr/bin/env bash
set -euo pipefail
umask 0077

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/tools/mk_local.sh"

usage() {
    cat <<EOF
Usage: $0 <ifname> --switch-socket <path> --switch-port-id <port> [options]
Build and restart one local TUN adapter; no SSH or tunnel secret is needed.

  --control-socket <path>  Default /run/tuntom/<ifname>.control
  --mtu <n>               Default TUNTOM_MTU or 1500 (576..65535)
  --l4-capacity <n>        Default 1000000 (1..100000000)
  --l3-capacity <n>        Default 250000 (1..100000000)
  --l4-timeout <seconds>   Default 120 (1..86400)
  --l3-timeout <seconds>   Default 30 (1..86400)
  --pre-hook <path>        Default /etc/tuntom/adapter-pre.sh
  --post-hook <path>       Default /etc/tuntom/adapter-post.sh
  --socket-owner <u:g>     Default tuntom:tuntom; socket mode is 0660
  --stop                  Only <ifname> is required; uses saved endpoints
  -h, --help

Both pre/up and post/up run with the TUN up, ready for addresses and routes.
TUNTOM_RUN_DIR and TUNTOM_STATE_DIR override /run/tuntom and /run/tuntom-mk.
TUNTOM_BIN_DIR overrides /var/lib/tuntom-mk (must allow execution).
EOF
}

main() {
    local_init adapter "$@"
    shift
    while (( $# )); do
        case "$1" in
            --switch-socket) local_value "$@"; switch_socket="$2"; shift ;;
            --switch-port-id) local_value "$@"; port_id="$2"; shift ;;
            --mtu) local_value "$@"; mtu="$2"; shift ;;
            --l4-capacity|--l3-capacity|--l4-timeout|--l3-timeout)
                local_value "$@"
                if [[ "$1" == *capacity ]]; then
                    local_number "$1" "$2" 1 100000000
                else
                    local_number "$1" "$2" 1 86400
                fi
                service_args+=("$1" "$2"); shift ;;
            *) local_option "$@"; shift "$option_shift" ;;
        esac
        shift
    done
    local_validate
    if (( ! stop_requested )); then
        local LC_ALL=C
        [[ -n "$switch_socket" && -n "$port_id" ]] ||
            local_die "--switch-socket and --switch-port-id are required"
        [[ ${#port_id} -le 63 && "$port_id" != *[[:space:]]* ]] ||
            local_die "Invalid switch port ID (1..63 bytes, no whitespace)"
        local_number --mtu "$mtu" 576 65535
    fi
    local_run
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
