#!/usr/bin/env bash
set -euo pipefail
umask 0077

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/tools/mk_local.sh"

usage() {
    cat <<EOF
Usage: $0 <name> [options]
Build and restart a local tomtom-switch-mp instance.

  --socket <path>          Listener (default /run/tuntom/<name>.sock)
  --control-socket <path>  Stats socket (default /run/tuntom/<name>.control)
  --route <in>:<label>=<out>:<label>   Repeatable static flow rule
  --exit-port <port>       Repeatable adapter port (EXIT delivery)
  --trunk-port <port>      Repeatable aggregate port (SWITCH delivery)
  --default-back=off|on    Default off
  --max-ports <n>          Registered ports (default 256)
  --max-pending <n>        Pending registrations (default 16)
  --workers <n>           Data worker cap; also caps --auto-pool
  --work-per-thread <n>   Weighted split threshold (default 8)
  --rx-weight <n>          RX cost multiplier (default 1)
  --tx-weight <n>          TX cost multiplier (default 1)
  --adapter-weight <n>     Adapter cost (default 2; auto default 4)
  --trunk-weight <n>       Trunk cost (default 4; auto default 8)
  --pool-size <n>          Buffers per ingress (default 128)
  --queue-size <n>         Pointers per RX-TX pair (default 128)
                          All numbers above: 1..65535

  --auto-pool             Inspect affinity, physical cores and cgroup v2 quota;
                          choose worker cap and aggregate weights after pre/up
  --reserve-cpus <n>      With --auto-pool: leave N CPUs outside the worker budget
                          (0..65535; default half rounded up, at least 1 worker)
  --dry-run               Print CPU/worker plan and arguments; no sudo, hooks,
                          daemon startup or changes to managed instances

  --rules-file <path>      Seed rules (route / exit-port / trunk-port / default-back)
  --pre-hook <path>        Default /etc/tuntom/switch-pre.sh
  --post-hook <path>       Default /etc/tuntom/switch-post.sh
  --socket-owner <u:g>     Default tuntom:tuntom; socket mode 0660
  --stop                  Stop this named switch, either implementation
  -h, --help

Explicit weights override auto weights. Auto scheduling uses declared ports,
not measured traffic. The printed role plan assumes those ports are connected;
the switch assigns roles as ports actually register. Reserve is a worker-count
budget, not CPU isolation. No automatic affinity or NUMA pinning is performed.
State, binaries and the instance lock are shared with mk_switch.sh by name.
Starting this helper replaces the named switch after build/config validation.
TUNTOM_RUN_DIR, TUNTOM_STATE_DIR and TUNTOM_BIN_DIR override standard paths.
TUNTOM_SWITCH_RULES_FILE and TUNTOM_SWITCH_{PRE,POST}_HOOK are supported.
Dry-run needs a C++17 compiler and an executable TMPDIR (default /tmp).
EOF
}

mp_compile_planner() {
    "${CXX:-g++}" -std=c++17 -pthread -O2 -Wall -Wextra -Wconversion -pedantic \
        "${script_dir}/tools/switch_mp_plan.cpp" -o "${stage}/planner"
    [[ -x "${stage}/planner" ]] || local_die "Planner was not built"
}

mp_plan() {
    local -a planner_args=(--socket "$switch_socket" "${service_args[@]}")
    (( ! auto_pool )) || planner_args+=(--auto-pool)
    [[ -z "$reserve_cpus" ]] || planner_args+=(--reserve-cpus "$reserve_cpus")
    "${stage}/planner" "${planner_args[@]}" > "${stage}/worker-plan" ||
        local_die "MP worker planning failed; current instance was kept running"
    echo "MP plan (assuming all configured ports are connected):"
    cat "${stage}/worker-plan"
    local key value
    while IFS='=' read -r key value; do
        case "$key" in
            option.workers|option.work-per-thread|option.rx-weight|option.tx-weight|option.adapter-weight|option.trunk-weight)
                local_number "$key" "$value" 1 65535
                service_args+=("--${key#option.}" "$value") ;;
        esac
    done < "${stage}/worker-plan"
}

local_prepare_switch() {
    if (( auto_pool )); then
        mp_compile_planner
        mp_plan
    fi
}

mp_dry_run() {
    stage="$(mktemp -d "${TMPDIR:-/tmp}/tuntom-mp-plan.XXXXXXXX")"
    trap 'rm -rf -- "$stage"' EXIT
    mp_compile_planner
    if [[ -n "$rules_source" ]]; then
        rules_file="$rules_source"
        local_read_rules
    fi
    mp_plan
    echo "Dry run: pre/up hooks are not executed; generated rules may change this plan."
    printf 'Command:'
    printf ' %q' "${bin_root}/switch-${name}/main" --socket "$switch_socket" \
        --control-socket "$control_socket" "${service_args[@]}"
    printf '\n'
}

main() {
    local_init switch "$@"
    helper_kind=switch_mp; source_kind=switch_mp
    shift
    auto_pool=0; dry_run=0; reserve_cpus=""
    rules_source="${TUNTOM_SWITCH_RULES_FILE:-}"
    while (( $# )); do
        case "$1" in
            --socket) local_value "$@"; switch_socket="$2"; shift ;;
            --rules-file) local_value "$@"; rules_source="$2"; shift ;;
            --route|--exit-port|--trunk-port)
                local_value "$@"; service_args+=("$1" "$2"); shift ;;
            --max-ports|--max-pending|--workers|--work-per-thread|--rx-weight|--tx-weight|--adapter-weight|--trunk-weight|--pool-size|--queue-size)
                local_value "$@"; local_number "$1" "$2" 1 65535
                service_args+=("$1" "$2"); shift ;;
            --reserve-cpus)
                local_value "$@"; local_number "$1" "$2" 0 65535
                reserve_cpus="$((10#$2))"; shift ;;
            --auto-pool) auto_pool=1 ;;
            --dry-run) dry_run=1 ;;
            --default-back=off|--default-back=on) service_args+=("$1") ;;
            *) local_option "$@"; shift "$option_shift" ;;
        esac
        shift
    done
    local_validate
    [[ -z "$reserve_cpus" || "$auto_pool" == 1 ]] || local_die "--reserve-cpus requires --auto-pool"
    (( ! dry_run || ! stop_requested )) || local_die "--dry-run cannot be combined with --stop"
    if (( dry_run )); then mp_dry_run; else local_run; fi
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then main "$@"; fi
