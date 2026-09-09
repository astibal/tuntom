#!/usr/bin/env bash
set -euo pipefail
umask 0077

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/tools/mk_local.sh"

usage() {
    cat <<EOF
Usage: $0 <name> [options]
Build and restart one local switch; no SSH or tunnel secret is needed.

  --socket <path>          Listener (default /run/tuntom/<name>.sock)
  --control-socket <path>  Stats socket (default /run/tuntom/<name>.control)
  --route <in>:<label>=<out>:<label>   Repeatable static flow rule
  --exit-port <port>       Repeatable exit adapter port
  --default-back=off|on    Default off
  --max-ports <n>          Registered port limit (default 256, range 1..65535)
  --max-pending <n>        Pending registration limit (default 16, range 1..65535)
  --rules-file <path>      Seed rules for pre/up (see examples/switch.rules)
  --pre-hook <path>        Default /etc/tuntom/switch-pre.sh
  --post-hook <path>       Default /etc/tuntom/switch-post.sh
  --socket-owner <u:g>     Default tuntom:tuntom; socket mode is 0660
  --stop                  Stop by name, using saved endpoints
  -h, --help

pre/up can write TUNTOM_RULES_FILE before configuration validation and restart.
TUNTOM_RUN_DIR and TUNTOM_STATE_DIR override /run/tuntom and /run/tuntom-mk.
TUNTOM_BIN_DIR overrides /var/lib/tuntom-mk (must allow execution).
EOF
}

main() {
    local_init switch "$@"
    shift
    rules_source="${TUNTOM_SWITCH_RULES_FILE:-}"
    while (( $# )); do
        case "$1" in
            --socket) local_value "$@"; switch_socket="$2"; shift ;;
            --rules-file) local_value "$@"; rules_source="$2"; shift ;;
            --route|--exit-port)
                local_value "$@"; service_args+=("$1" "$2"); shift ;;
            --max-ports|--max-pending)
                local_value "$@"; local_number "$1" "$2" 1 65535
                service_args+=("$1" "$2"); shift ;;
            --default-back=off|--default-back=on) service_args+=("$1") ;;
            *) local_option "$@"; shift "$option_shift" ;;
        esac
        shift
    done
    local_validate
    local_run
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
