#!/usr/bin/env bash
set -euo pipefail
umask 0077

usage() {
    cat >&2 <<EOF
Usage: $0 <id 1..255> <host|user@host> [options]

Tunnel options:
  --count <1..64>  Run ID, ID_1, ID_2, ... as one group (default: saved count or 1)
  --snat | --no-snat
  --mss-clamp | --no-mss-clamp
  --no-address  Skip TUN IPv4/IPv6 addresses, peer routes and tunnel pings
  --crypto-auth-only | --no-stats | --all-tools | --stop

Build options:
  --all-tools  Also build/install tuntom-switch, tuntom-switch-adapter and
               tuntomctl on both local and remote hosts

Switch ports (the tuntom-switch listener must already be running):
  --client-switch <socket> <port-id> <label>
  --server-switch <socket> <port-id> <label>
  --client-switch-exit-node
  --server-switch-exit-node
  --client-switch-ipc <auto|v1|inline>
  --server-switch-ipc <auto|v1|inline>
  --client-switch-ipc-batch <1..16>
  --server-switch-ipc-batch <1..16>
  --client-classifier-file <path>  Classifier file on the local host
  --server-classifier-file <path>  Classifier file already on the remote host
EOF
}

if (( $# < 2 )); then
    usage
    exit 1
fi

id="$1"
remote="$2"
shift 2

tuntom_snat=0
tuntom_mss_clamp=1
no_address=0
member_count=1
count_requested=0
member_index=0
instance="$id"
instance_key="$id"
hook_scope=member
group_members="$id"
local_manifest=""
remote_manifest=""
stop_requested=0
all_tools=0
crypto_option=""
stats_option=""
client_switch_socket=""
client_switch_port_id=""
client_switch_label=""
client_switch_exit_node=0
server_switch_socket=""
server_switch_port_id=""
server_switch_label=""
server_switch_exit_node=0
client_ipc_args=()
server_ipc_args=()
client_classifier_file=""
server_classifier_file=""

while (( $# > 0 )); do
    case "$1" in
        --count)
            if (( $# < 2 )) || ! [[ "$2" =~ ^([1-9]|[1-5][0-9]|6[0-4])$ ]]; then
                echo "--count requires a value in 1..64" >&2
                exit 1
            fi
            member_count="$2"
            count_requested=1
            shift
            ;;
        --snat)
            tuntom_snat=1
            ;;
        --no-snat)
            tuntom_snat=0
            ;;
        --mss-clamp)
            tuntom_mss_clamp=1
            ;;
        --no-mss-clamp)
            tuntom_mss_clamp=0
            ;;
        --no-address)
            no_address=1
            ;;
        --no-stats)
            stats_option="--no-stats"
            ;;
        --crypto-auth-only)
            crypto_option="--crypto-auth-only"
            ;;
        --all-tools)
            all_tools=1
            ;;
        --client-switch|--server-switch)
            switch_side="${1#--}"
            if (( $# < 4 )); then
                echo "$1 requires <socket> <port-id> <label>" >&2
                usage
                exit 1
            fi
            switch_socket="$2"
            switch_port_id="$3"
            switch_label="$4"
            if [[ -z "$switch_socket" || -z "$switch_port_id" ||
                  "$switch_port_id" == *[[:space:]]* || ${#switch_port_id} -gt 63 ]]; then
                echo "Invalid ${switch_side} socket or port ID" >&2
                exit 1
            fi
            if ! [[ "$switch_label" =~ ^(0[xX][0-9A-Fa-f]+|[0-9]+)$ ]]; then
                echo "Invalid ${switch_side} label: ${switch_label}" >&2
                exit 1
            fi
            if [[ "$1" == "--client-switch" ]]; then
                client_switch_socket="$switch_socket"
                client_switch_port_id="$switch_port_id"
                client_switch_label="$switch_label"
            else
                server_switch_socket="$switch_socket"
                server_switch_port_id="$switch_port_id"
                server_switch_label="$switch_label"
            fi
            shift 3
            ;;
        --client-switch-ipc|--server-switch-ipc|--client-switch-ipc-batch|--server-switch-ipc-batch)
            if (( $# < 2 )); then echo "$1 requires a value" >&2; exit 1; fi
            ipc_option="--switch-ipc"
            if [[ "$1" == *-batch ]]; then
                ipc_option="--switch-ipc-batch"
                if ! [[ "$2" =~ ^([1-9]|1[0-6])$ ]]; then echo "IPC batch must be 1..16" >&2; exit 1; fi
            elif [[ "$2" != auto && "$2" != v1 && "$2" != inline ]]; then
                echo "IPC mode must be auto, v1 or inline" >&2; exit 1
            fi
            if [[ "$1" == --client-* ]]; then client_ipc_args+=("$ipc_option" "$2")
            else server_ipc_args+=("$ipc_option" "$2"); fi
            shift ;;
        --client-classifier-file|--server-classifier-file)
            if (( $# < 2 )) || [[ -z "$2" ]]; then echo "$1 requires a path" >&2; exit 1; fi
            if [[ "$1" == --client-* ]]; then client_classifier_file="$2"
            else server_classifier_file="$2"; fi
            shift ;;
        --client-switch-exit-node)
            client_switch_exit_node=1
            ;;
        --server-switch-exit-node)
            server_switch_exit_node=1
            ;;
        --stop)
            stop_requested=1
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            exit 1
            ;;
    esac

    shift
done

if (( client_switch_exit_node )) && [[ -z "$client_switch_socket" ]]; then
    echo "--client-switch-exit-node requires --client-switch" >&2
    exit 1
fi
if (( server_switch_exit_node )) && [[ -z "$server_switch_socket" ]]; then
    echo "--server-switch-exit-node requires --server-switch" >&2
    exit 1
fi

if (( ${#client_ipc_args[@]} )) && [[ -z "$client_switch_socket" ]]; then
    echo "Client IPC options require --client-switch" >&2; exit 1
fi
if (( ${#server_ipc_args[@]} )) && [[ -z "$server_switch_socket" ]]; then
    echo "Server IPC options require --server-switch" >&2; exit 1
fi
if [[ -n "$client_classifier_file" && -z "$client_switch_socket" ]]; then
    echo "--client-classifier-file requires --client-switch" >&2; exit 1
fi
if [[ -n "$server_classifier_file" && -z "$server_switch_socket" ]]; then
    echo "--server-classifier-file requires --server-switch" >&2; exit 1
fi
client_has_tun=1
server_has_tun=1
if [[ -n "$client_switch_socket" ]] && (( ! client_switch_exit_node )); then
    client_has_tun=0
fi
if [[ -n "$server_switch_socket" ]] && (( ! server_switch_exit_node )); then
    server_has_tun=0
fi

shell_join() {
    local output="" quoted argument
    for argument in "$@"; do
        printf -v quoted '%q' "$argument"
        output+=" ${quoted}"
    done
    printf '%s' "$output"
}

client_switch_args=("${client_ipc_args[@]}")
server_switch_args=("${server_ipc_args[@]}")
if [[ -n "$client_classifier_file" ]]; then client_switch_args+=(--classifier-file "$client_classifier_file"); fi
if [[ -n "$server_classifier_file" ]]; then server_switch_args+=(--classifier-file "$server_classifier_file"); fi
if [[ -n "$client_switch_socket" ]]; then
    client_switch_args+=(
        --switch-socket "$client_switch_socket"
        --switch-port-id "$client_switch_port_id"
        --switch-label "$client_switch_label")
    if (( client_switch_exit_node )); then
        client_switch_args+=(--switch-exit-node)
    fi
fi
if [[ -n "$server_switch_socket" ]]; then
    server_switch_args+=(
        --switch-socket "$server_switch_socket"
        --switch-port-id "$server_switch_port_id"
        --switch-label "$server_switch_label")
    if (( server_switch_exit_node )); then
        server_switch_args+=(--switch-exit-node)
    fi
fi
client_switch_options="$(shell_join "${client_switch_args[@]}")"
server_switch_options="$(shell_join "${server_switch_args[@]}")"
switch_enabled=0
if [[ -n "$client_switch_socket" || -n "$server_switch_socket" ]]; then
    switch_enabled=1
fi

if ! [[ "$id" =~ ^[1-9][0-9]{0,2}$ ]] || (( id > 255 )); then
    echo "Tunnel id must be in range 1..255" >&2
    exit 1
fi
id="$((10#$id))"
instance="$id"
instance_key="$id"

if [[ "$remote" != *@* ]]; then
    remote="root@${remote}"
fi

if (( ! stop_requested )) && [[ -z "${TUNTOM_SECRET:-}" ]]; then
    echo "TUNTOM_SECRET is not set" >&2
    echo "Use a 128-bit key encoded as exactly 32 hex characters." >&2
    exit 1
fi

if (( ! stop_requested )) && ! [[ "$TUNTOM_SECRET" =~ ^[0-9A-Fa-f]{32}$ ]]; then
    echo "TUNTOM_SECRET must contain exactly 32 hex characters" >&2
    exit 1
fi

if (( ! stop_requested )) && [[ "${TUNTOM_STATS_FORMAT:-txt}" != "txt" ]]; then
    echo "Unsupported TUNTOM_STATS_FORMAT: ${TUNTOM_STATS_FORMAT}" >&2
    echo "Currently supported: txt" >&2
    exit 1
fi

export TUNTOM_SECRET="${TUNTOM_SECRET:-}"

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source_dir="${script_dir}/src"
net_file="${script_dir}/tuntom-net.sh"

client_if="ut${id}c"
server_if="ut${id}s"
tuntom_prefix16="${TUNTOM_PREFIX16:-10.254}"

client_ip=""
server_ip=""
client_ipv6=""
server_ipv6=""
if (( ! no_address && ! stop_requested )); then
    if ! [[ "$tuntom_prefix16" =~ ^([0-9]{1,3})\.([0-9]{1,3})$ ]] ||
       (( 10#${BASH_REMATCH[1]:-999} > 255 || 10#${BASH_REMATCH[2]:-999} > 255 )); then
        echo "TUNTOM_PREFIX16 must contain two IPv4 octets, for example 10.254" >&2
        exit 1
    fi

    client_ip="${tuntom_prefix16}.${id}.1"
    server_ip="${tuntom_prefix16}.${id}.2"
    ipv6_prefix16="${tuntom_prefix16//./:}"
    client_ipv6="fd42::${ipv6_prefix16}:${id}:1"
    server_ipv6="fd42::${ipv6_prefix16}:${id}:2"
fi

mtu="${TUNTOM_MTU:-1500}"
transport_mtu="${TUNTOM_TRANSPORT_MTU:-1400}"
udp_port="$((40000 + id))"

tuntom_mark_mask="${TUNTOM_MARK_MASK:-0x00ff0000}"
tuntom_mark="${TUNTOM_MARK:-$((id << 16))}"
tuntom_table="${TUNTOM_TABLE:-$((10000 + id))}"
tuntom_chain="${TUNTOM_CHAIN:-TUNTOM_${id}}"

# Optional hook sources live on the caller. The group coordinator snapshots
# them for teardown; remote execution streams the snapshot to bash -s.
# Missing hook files are silently ignored.
tuntom_pre_hook="${TUNTOM_PRE_HOOK:-/etc/tuntom/tuntom-pre.sh}"
tuntom_post_hook="${TUNTOM_POST_HOOK:-/etc/tuntom/tuntom-post.sh}"

local_bin="/tmp/tuntom_${id}c"
remote_bin="/tmp/tuntom_${id}s"
local_switch_bin="/tmp/tuntom-switch"
local_adapter_bin="/tmp/tuntom-switch-adapter"
local_control_bin="/tmp/tuntomctl"
remote_switch_bin="/tmp/tuntom-switch"
remote_adapter_bin="/tmp/tuntom-switch-adapter"
remote_control_bin="/tmp/tuntomctl"

# Build into separate staging files so a failed or slow compilation never
# touches the binaries used by the currently running tunnel.
local_stage="${local_bin}.new.$$"
remote_stage="${remote_bin}.new.$$"
local_switch_stage="${local_switch_bin}.new.$$"
local_adapter_stage="${local_adapter_bin}.new.$$"
local_control_stage="${local_control_bin}.new.$$"
remote_switch_stage="${remote_switch_bin}.new.$$"
remote_adapter_stage="${remote_adapter_bin}.new.$$"
remote_control_stage="${remote_control_bin}.new.$$"

remote_net_file="/tmp/tuntom-net-${id}.sh"

local_log="/tmp/tuntom_${id}c.log"
remote_log="/tmp/tuntom_${id}s.log"

run_dir="/run/tuntom"

local_pid_file="${run_dir}/${id}c.pid"
remote_pid_file="${run_dir}/${id}s.pid"
local_stats_file="${run_dir}/${id}c.stats"
remote_stats_file="${run_dir}/${id}s.stats"
local_control_file="${run_dir}/${id}c.control"
remote_control_file="${run_dir}/${id}s.control"
stats_format="${TUNTOM_STATS_FORMAT:-txt}"

runtime_user="tuntom"
runtime_group="tuntom"

if [[ ! -f "$net_file" ]]; then
    echo "Missing network helper: $net_file" >&2
    exit 1
fi

if [[ $EUID -eq 0 ]]; then
    root_cmd=()
else
    # Named preservation works with both classic sudo and sudo-rs.
    # Pass only TUNTOM_* names; secret values stay out of the command line.
    root_cmd=(sudo)
    for env_name in "${!TUNTOM_@}"; do
        root_cmd+=("--preserve-env=$env_name")
    done
fi

mk_lock_file="${run_dir}/mk_${id}.lock"

acquire_mk_lock() {
    local remote_lock_command="$1"
    local coordinator_pid="$BASHPID"
    local coordinator_stat coordinator_start
    local -a coordinator_fields
    IFS= read -r coordinator_stat < "/proc/$coordinator_pid/stat"
    read -r -a coordinator_fields <<< "${coordinator_stat##*) }"
    coordinator_start="${coordinator_fields[19]}"
    "${root_cmd[@]}" mkdir -p "$run_dir"
    # One coprocess holds both locks. Closing its stdin releases the local
    # flock, then closes SSH stdin and releases the remote flock as well.
    coproc TUNTOM_MK_LOCK {
        "${root_cmd[@]}" bash -c '
            exec 9>"$1"
            flock -n 9 || exit 75
            printf "LOCAL_LOCKED\n"
            cat >/dev/null
        ' bash "$mk_lock_file" | {
            local_status=""
            if IFS= read -r local_status && [[ "$local_status" == LOCAL_LOCKED ]]; then
                ssh -T -o BatchMode=yes -o ServerAliveInterval=15 -o ServerAliveCountMax=3 \
                    "$remote" "$remote_lock_command" | {
                    remote_status=""
                    if IFS= read -r remote_status && [[ "$remote_status" == LOCKED ]]; then
                        printf 'LOCKED\n'
                        cat >/dev/null
                        # Unexpected EOF means the server flock was lost. The
                        # coordinator's TERM trap performs startup rollback.
                        # Redirect stderr first: normal shutdown may already
                        # have removed the coordinator's /proc entry.
                        if IFS= read -r coordinator_stat 2>/dev/null < "/proc/$coordinator_pid/stat"; then
                            read -r -a coordinator_fields <<< "${coordinator_stat##*) }"
                            if [[ "${coordinator_fields[19]-}" == "$coordinator_start" ]]; then
                                kill -TERM "$coordinator_pid" 2>/dev/null || true
                            fi
                        fi
                    else
                        printf 'FAILED\n'
                    fi
                }
            else
                printf 'FAILED\n'
            fi
        }
    }
    local lock_status=""
    if ! IFS= read -r lock_status <&"${TUNTOM_MK_LOCK[0]}" || [[ "$lock_status" != LOCKED ]]; then
        echo "Cannot lock tunnel group $id on both hosts (busy or SSH failed)" >&2
        return 1
    fi
    echo "  group lock: $id (local + remote)"
}

ensure_runtime_account_local() {
    if ! getent group "$runtime_group" >/dev/null 2>&1; then
        "${root_cmd[@]}" groupadd --system "$runtime_group"
    fi

    if ! id -u "$runtime_user" >/dev/null 2>&1; then
        "${root_cmd[@]}" useradd \
            --system \
            --gid "$runtime_group" \
            --no-create-home \
            --home-dir /nonexistent \
            --shell /usr/sbin/nologin \
            "$runtime_user"
    fi

    local expected_gid actual_gid
    expected_gid="$(getent group "$runtime_group" | cut -d: -f3)"
    actual_gid="$(id -g "$runtime_user")"

    if [[ "$actual_gid" != "$expected_gid" ]]; then
        echo "Runtime user ${runtime_user} does not use group ${runtime_group}" >&2
        exit 1
    fi
}

ensure_runtime_account_remote() {
    ssh "$remote" "
        if ! getent group '${runtime_group}' >/dev/null 2>&1; then
            groupadd --system '${runtime_group}'
        fi

        if ! id -u '${runtime_user}' >/dev/null 2>&1; then
            useradd \
                --system \
                --gid '${runtime_group}' \
                --no-create-home \
                --home-dir /nonexistent \
                --shell /usr/sbin/nologin \
                '${runtime_user}'
        fi

        expected_gid=\$(getent group '${runtime_group}' | cut -d: -f3)
        actual_gid=\$(id -g '${runtime_user}')

        if [ \"\$actual_gid\" != \"\$expected_gid\" ]; then
            echo 'Runtime user ${runtime_user} does not use group ${runtime_group}' >&2
            exit 1
        fi
    "
}

stage_active=0

cleanup_staging() {
    if (( stage_active )); then
        rm -f "$local_stage" "$local_switch_stage" "$local_adapter_stage" \
            "$local_control_stage" 2>/dev/null || true
        ssh "$remote" "rm -f '${remote_stage}' '${remote_switch_stage}' \
            '${remote_adapter_stage}' '${remote_control_stage}'" >/dev/null 2>&1 || true
    fi
}

run_hook_local() {
    local hook="$1"
    local phase="$2"
    local action="$3"
    local side="$4"
    local tuntom_if="$5"
    local local_ip="$6"
    local peer_ip="$7"

    if ! "${root_cmd[@]}" test -f "$hook"; then
        return 0
    fi

    echo "  hook ${phase}/${action}/${side}: ${hook}"

    "${root_cmd[@]}" env \
        TUNTOM_ID="$id" \
        TUNTOM_GROUP_ID="$id" \
        TUNTOM_INSTANCE="$instance" \
        TUNTOM_INSTANCE_KEY="$instance_key" \
        TUNTOM_MEMBER_INDEX="$member_index" \
        TUNTOM_MEMBER_COUNT="$member_count" \
        TUNTOM_MEMBERS="$group_members" \
        TUNTOM_SCOPE="$hook_scope" \
        TUNTOM_GROUP_MANIFEST="$local_manifest" \
        TUNTOM_ACTION="$action" \
        TUNTOM_PHASE="$phase" \
        TUNTOM_SIDE="$side" \
        TUNTOM_IF="$tuntom_if" \
        TUNTOM_NO_ADDRESS="$no_address" \
        TUNTOM_LOCAL_IP="$local_ip" \
        TUNTOM_PEER_IP="$peer_ip" \
        TUNTOM_CLIENT_IP="$client_ip" \
        TUNTOM_SERVER_IP="$server_ip" \
        TUNTOM_CLIENT_IPV6="$client_ipv6" \
        TUNTOM_SERVER_IPV6="$server_ipv6" \
        TUNTOM_UDP_PORT="$udp_port" \
        TUNTOM_MTU="$mtu" \
        TUNTOM_TRANSPORT_MTU="$transport_mtu" \
        TUNTOM_SNAT="$tuntom_snat" \
        TUNTOM_MSS_CLAMP="$tuntom_mss_clamp" \
        TUNTOM_MARK="$tuntom_mark" \
        TUNTOM_MARK_MASK="$tuntom_mark_mask" \
        TUNTOM_TABLE="$tuntom_table" \
        TUNTOM_CHAIN="$tuntom_chain" \
        TUNTOM_NAT_CHAIN="${tuntom_chain:+${tuntom_chain}_N}" \
        TUNTOM_SNAT_CHAIN="${tuntom_chain:+${tuntom_chain}_S}" \
        TUNTOM_MANGLE_CHAIN="${tuntom_chain:+${tuntom_chain}_M}" \
        TUNTOM_FORWARD_CHAIN="${tuntom_chain:+${tuntom_chain}_F}" \
        bash "$hook"
}

run_hook_remote() {
    local hook="$1"
    local phase="$2"
    local action="$3"
    local side="$4"
    local tuntom_if="$5"
    local local_ip="$6"
    local peer_ip="$7"

    if ! "${root_cmd[@]}" test -f "$hook"; then
        return 0
    fi

    echo "  hook ${phase}/${action}/${side}: ${hook} -> ${remote}"

    "${root_cmd[@]}" cat "$hook" | ssh "$remote" \
        "TUNTOM_ID='${id}' \
         TUNTOM_GROUP_ID='${id}' \
         TUNTOM_INSTANCE='${instance}' \
         TUNTOM_INSTANCE_KEY='${instance_key}' \
         TUNTOM_MEMBER_INDEX='${member_index}' \
         TUNTOM_MEMBER_COUNT='${member_count}' \
         TUNTOM_MEMBERS='${group_members}' \
         TUNTOM_SCOPE='${hook_scope}' \
         TUNTOM_GROUP_MANIFEST='${remote_manifest}' \
         TUNTOM_ACTION='${action}' \
         TUNTOM_PHASE='${phase}' \
         TUNTOM_SIDE='${side}' \
         TUNTOM_IF='${tuntom_if}' \
         TUNTOM_NO_ADDRESS='${no_address}' \
         TUNTOM_LOCAL_IP='${local_ip}' \
         TUNTOM_PEER_IP='${peer_ip}' \
         TUNTOM_CLIENT_IP='${client_ip}' \
         TUNTOM_SERVER_IP='${server_ip}' \
         TUNTOM_CLIENT_IPV6='${client_ipv6}' \
         TUNTOM_SERVER_IPV6='${server_ipv6}' \
         TUNTOM_UDP_PORT='${udp_port}' \
         TUNTOM_MTU='${mtu}' \
         TUNTOM_TRANSPORT_MTU='${transport_mtu}' \
         TUNTOM_SNAT='${tuntom_snat}' \
         TUNTOM_MSS_CLAMP='${tuntom_mss_clamp}' \
         TUNTOM_MARK='${tuntom_mark}' \
         TUNTOM_MARK_MASK='${tuntom_mark_mask}' \
         TUNTOM_TABLE='${tuntom_table}' \
         TUNTOM_CHAIN='${tuntom_chain}' \
         TUNTOM_NAT_CHAIN='${tuntom_chain:+${tuntom_chain}_N}' \
         TUNTOM_SNAT_CHAIN='${tuntom_chain:+${tuntom_chain}_S}' \
         TUNTOM_MANGLE_CHAIN='${tuntom_chain:+${tuntom_chain}_M}' \
         TUNTOM_FORWARD_CHAIN='${tuntom_chain:+${tuntom_chain}_F}' \
         bash -s"
}

hook_pre_down_local() {
    run_hook_local "$tuntom_pre_hook" pre down local "$client_if" "$client_ip" "$server_ip"
}

hook_post_down_local() {
    run_hook_local "$tuntom_post_hook" post down local "$client_if" "$client_ip" "$server_ip"
}

hook_pre_down_remote() {
    run_hook_remote "$tuntom_pre_hook" pre down remote "$server_if" "$server_ip" "$client_ip"
}

hook_post_down_remote() {
    run_hook_remote "$tuntom_post_hook" post down remote "$server_if" "$server_ip" "$client_ip"
}

# Run this function with the same privileges used to launch the tunnel.
# PID files are only bookkeeping: older/orphaned instances may not be listed.
member_process_exists() {
    local binary="$1" role="$2" tunnel_id="$3" interface="$4" proc
    local -a argv
    for proc in /proc/[0-9]*/cmdline; do
        argv=()
        mapfile -d '' -t argv 2>/dev/null < "$proc" || continue
        if [[ "${argv[0]-}" == "$binary" && "${argv[1]-}" == "$role" &&
              "${argv[2]-}" == "$tunnel_id" && "${argv[3]-}" == "$interface" ]]; then
            return 0
        fi
    done
    return 1
}

stop_matching_processes() {
    local binary="$1" role="$2" tunnel_id="$3" interface="$4" pid_file="$5"
    local proc pid stat rest identity state current attempt
    local -a argv fields targets=() identities=()
    for proc in /proc/[0-9]*; do
        pid="${proc##*/}"
        argv=()
        if ! mapfile -d '' -t argv < "$proc/cmdline" 2>/dev/null; then
            continue
        fi
        [[ "${argv[0]-}" == "$binary" && "${argv[1]-}" == "$role" &&
           "${argv[2]-}" == "$tunnel_id" && "${argv[3]-}" == "$interface" ]] || continue
        if ! IFS= read -r stat < "$proc/stat" 2>/dev/null; then continue; fi
        rest="${stat##*) }"
        read -r -a fields <<< "$rest"
        identity="${fields[19]-}"
        [[ "$identity" =~ ^[0-9]+$ ]] || return 1
        targets+=("$pid")
        identities+=("$identity")
    done

    # Recheck process start time before signalling, avoiding stale/reused PIDs.
    for current in "${!targets[@]}"; do
        pid="${targets[current]}"
        identity="${identities[current]}"
        for ((attempt=0; attempt<60; ++attempt)); do
            if [[ ! -e "/proc/$pid/stat" ]]; then break; fi
            if ! IFS= read -r stat < "/proc/$pid/stat"; then
                [[ ! -e "/proc/$pid" ]] && break
                echo "ERROR: cannot inspect PID $pid" >&2
                return 1
            fi
            rest="${stat##*) }"
            read -r -a fields <<< "$rest"
            [[ "${fields[19]-}" == "$identity" ]] || break
            state="${fields[0]-}"
            [[ "$state" == Z || "$state" == X ]] && break
            if (( attempt == 0 )); then
                echo "Stopping $role tunnel $tunnel_id PID $pid"
                kill -TERM "$pid" 2>/dev/null || true
            elif (( attempt == 40 )); then
                kill -KILL "$pid" 2>/dev/null || true
            fi
            sleep 0.05
        done
        if (( attempt == 60 )); then
            echo "ERROR: PID $pid did not stop; refusing to start another instance" >&2
            return 1
        fi
    done
    rm -f -- "$pid_file"
}

stop_local_process() {
    "${root_cmd[@]}" bash -c "$(declare -f stop_matching_processes)
        stop_matching_processes \"\$@\"" -- \
        "$local_bin" client "$instance" "$client_if" "$local_pid_file"
}

stop_remote_process() {
    ssh "$remote" "bash -s -- '${remote_bin}' server '${instance}' '${server_if}' '${remote_pid_file}'" <<< \
        "$(declare -f stop_matching_processes)
        stop_matching_processes \"\$@\""
}

net_down_local() {
    "${root_cmd[@]}" env \
        TUNTOM_ID="$id" \
        TUNTOM_IF="$client_if" \
        TUNTOM_SNAT="$tuntom_snat" \
        TUNTOM_MSS_CLAMP="$tuntom_mss_clamp" \
        TUNTOM_MARK="$tuntom_mark" \
        TUNTOM_MARK_MASK="$tuntom_mark_mask" \
        TUNTOM_TABLE="$tuntom_table" \
        TUNTOM_CHAIN="$tuntom_chain" \
        bash -c "
            source '$net_file'
            tuntom_net_down
        "
}

net_down_remote() {
    ssh "$remote" "
        if [ -f '${remote_net_file}' ]; then
            TUNTOM_ID='${id}' \
            TUNTOM_IF='${server_if}' \
            TUNTOM_SNAT='${tuntom_snat}' \
            TUNTOM_MSS_CLAMP='${tuntom_mss_clamp}' \
            TUNTOM_MARK='${tuntom_mark}' \
            TUNTOM_MARK_MASK='${tuntom_mark_mask}' \
            TUNTOM_TABLE='${tuntom_table}' \
            TUNTOM_CHAIN='${tuntom_chain}' \
            bash -c \"
                source '${remote_net_file}'
                tuntom_net_down
            \"
        fi
    "
}

net_up_local() {
    "${root_cmd[@]}" env \
        TUNTOM_ID="$id" \
        TUNTOM_IF="$client_if" \
        TUNTOM_SNAT="$tuntom_snat" \
        TUNTOM_MSS_CLAMP="$tuntom_mss_clamp" \
        TUNTOM_MARK="$tuntom_mark" \
        TUNTOM_MARK_MASK="$tuntom_mark_mask" \
        TUNTOM_TABLE="$tuntom_table" \
        TUNTOM_CHAIN="$tuntom_chain" \
        bash -c "
            source '$net_file'
            tuntom_net_up
        "
}

net_up_remote() {
    ssh "$remote" "
        TUNTOM_ID='${id}' \
        TUNTOM_IF='${server_if}' \
        TUNTOM_SNAT='${tuntom_snat}' \
        TUNTOM_MSS_CLAMP='${tuntom_mss_clamp}' \
        TUNTOM_MARK='${tuntom_mark}' \
        TUNTOM_MARK_MASK='${tuntom_mark_mask}' \
        TUNTOM_TABLE='${tuntom_table}' \
        TUNTOM_CHAIN='${tuntom_chain}' \
        bash -c \"
            source '${remote_net_file}'
            tuntom_net_up
        \"
    "
}

stop_member() {
    local rc=0
    stop_local_process || rc=1
    stop_remote_process || rc=1
    if (( rc )); then return "$rc"; fi
    "${root_cmd[@]}" rm -f "$local_stats_file" "$local_control_file" 2>/dev/null || true
    ssh "$remote" "rm -f '${remote_stats_file}' '${remote_control_file}'" >/dev/null 2>&1 || true

    if (( client_has_tun )); then
        hook_pre_down_local || rc=1
        net_down_local || rc=1
        hook_post_down_local || rc=1
        if "${root_cmd[@]}" ip link show "$client_if" >/dev/null 2>&1; then
            "${root_cmd[@]}" ip link del "$client_if" || rc=1
        fi
    fi
    if (( server_has_tun )); then
        hook_pre_down_remote || rc=1
        net_down_remote || rc=1
        hook_post_down_remote || rc=1
        ssh "$remote" "if ip link show '$server_if' >/dev/null 2>&1; then ip link del '$server_if'; fi" || rc=1
    fi
    return "$rc"
}

show_member() {
    echo "Tunnel ${instance}"
    echo "  remote:     ${remote}"
    if (( no_address )); then
        echo "  client if:  ${client_if}"
        echo "  server if:  ${server_if}"
        echo "  addresses:  disabled (--no-address)"
    else
        echo "  client if:  ${client_if} ${client_ip} -> ${server_ip}"
        echo "  server if:  ${server_if} ${server_ip} -> ${client_ip}"
        echo "  client IPv6: ${client_ipv6} -> ${server_ipv6}"
        echo "  server IPv6: ${server_ipv6} -> ${client_ipv6}"
    fi
    echo "  UDP port:   ${udp_port}"
    echo "  TUN MTU:    ${mtu}"
    echo "  xport MTU:  ${transport_mtu}"
    echo "  SNAT:       ${tuntom_snat}"
    echo "  MSS clamp:  ${tuntom_mss_clamp}"
    if [[ -n "$client_switch_socket" ]]; then
        echo "  client switch: ${client_switch_socket} port=${member_client_port} label=${client_switch_label} exit=${client_switch_exit_node}"
    fi
    if [[ -n "$server_switch_socket" ]]; then
        echo "  server switch: ${server_switch_socket} port=${member_server_port} label=${server_switch_label} exit=${server_switch_exit_node}"
    fi
    echo "  stats:      ${stats_format} -> ${run_dir}/${instance}{c,s}.stats"
    echo "  pre hook:   ${tuntom_pre_hook} (local file, runs local+remote)"
    echo "  post hook:  ${tuntom_post_hook} (local file, runs local+remote)"
    echo "  protocol:   v5 / Ascon auth + replay protection + fragmentation"
}

build_staging() {
    stage_active=1
    echo "[1] Compile local staging binary"
    rm -f "$local_stage" "$local_switch_stage" "$local_adapter_stage" "$local_control_stage"
    g++ -std=c++17 -pthread -O3 -march=native -mtune=native -Wall -Wextra -pedantic "$source_dir/main.cpp" -o "$local_stage"
    test -x "$local_stage"
    if (( all_tools )); then
        g++ -std=c++17 -pthread -O3 -march=native -mtune=native -Wall -Wextra -pedantic \
            "$source_dir/switch/main.cpp" -o "$local_switch_stage"
        g++ -std=c++17 -pthread -O3 -march=native -mtune=native -Wall -Wextra -pedantic \
            "$source_dir/adapter/main.cpp" -o "$local_adapter_stage"
        g++ -std=c++17 -pthread -O3 -march=native -mtune=native -Wall -Wextra -pedantic \
            "$source_dir/control/main.cpp" -o "$local_control_stage"
        test -x "$local_switch_stage"
        test -x "$local_adapter_stage"
        test -x "$local_control_stage"
    fi

    echo "[2] Compile remote staging binary"
    ssh -o BatchMode=yes "$remote" "rm -f '${remote_stage}'"
    remote_build_command=$(cat <<'REMOTE_BUILD'
set -eu
build_dir=$(mktemp -d /tmp/tuntom-build.XXXXXXXX)
trap 'rm -rf -- "$build_dir"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
tar -xf - -C "$build_dir"
g++ -std=c++17 -pthread -O3 -march=native -mtune=native -Wall -Wextra -pedantic "$build_dir/src/main.cpp" -o "$stage"
test -x "$stage"
if [ "$all_tools" = 1 ]; then
    g++ -std=c++17 -pthread -O3 -march=native -mtune=native -Wall -Wextra -pedantic "$build_dir/src/switch/main.cpp" -o "$switch_stage"
    g++ -std=c++17 -pthread -O3 -march=native -mtune=native -Wall -Wextra -pedantic "$build_dir/src/adapter/main.cpp" -o "$adapter_stage"
    g++ -std=c++17 -pthread -O3 -march=native -mtune=native -Wall -Wextra -pedantic "$build_dir/src/control/main.cpp" -o "$control_stage"
    test -x "$switch_stage"
    test -x "$adapter_stage"
    test -x "$control_stage"
fi
REMOTE_BUILD
    )
    tar -C "$script_dir" -cf - src | \
        ssh -o BatchMode=yes "$remote" \
            "stage='${remote_stage}'; all_tools='${all_tools}'; \
             switch_stage='${remote_switch_stage}'; adapter_stage='${remote_adapter_stage}'; \
             control_stage='${remote_control_stage}'; ${remote_build_command}"
}

prepare_runtime() {
    echo "[3] Deploy network helper"
    ssh "$remote" "cat > '${remote_net_file}' && chmod 700 '${remote_net_file}'" < "$net_file"

    ensure_runtime_account_local
    ensure_runtime_account_remote

    "${root_cmd[@]}" mkdir -p "$run_dir"
    "${root_cmd[@]}" chown root:"$runtime_group" "$run_dir"
    "${root_cmd[@]}" chmod 2770 "$run_dir"

    ssh "$remote" "
        mkdir -p '${run_dir}'
        chown root:'${runtime_group}' '${run_dir}'
        chmod 2770 '${run_dir}'
    "
}

install_companion_tools() {
    if (( all_tools )); then
        "${root_cmd[@]}" mv -f "$local_switch_stage" "$local_switch_bin"
        "${root_cmd[@]}" mv -f "$local_adapter_stage" "$local_adapter_bin"
        "${root_cmd[@]}" mv -f "$local_control_stage" "$local_control_bin"
        ssh "$remote" \
            "mv -f '${remote_switch_stage}' '${remote_switch_bin}' && \
             mv -f '${remote_adapter_stage}' '${remote_adapter_bin}' && \
             mv -f '${remote_control_stage}' '${remote_control_bin}'"
    fi
}

start_member() {
    echo "[7] Start remote server"
    printf '%s\n' "$TUNTOM_SECRET" | ssh "$remote" "
        read -r TUNTOM_SECRET
        export TUNTOM_SECRET
        nohup '${remote_bin}' server '${instance}' '${server_if}' \
            --mtu '${mtu}' \
            --transport-mtu '${transport_mtu}' \
            --stats-format '${stats_format}' \
            --stats-file '${remote_stats_file}' --control-socket '${remote_control_file}' ${crypto_option} ${stats_option}${server_switch_options} \
            >'${remote_log}' 2>&1 </dev/null &
        echo \$! > '${remote_pid_file}'
    "

    if (( server_has_tun )); then
        for _ in $(seq 1 20); do
            if ssh "$remote" "ip link show '${server_if}' >/dev/null 2>&1"; then
                break
            fi
            sleep 0.1
        done

        ssh "$remote" "
            set -e
            if [ '${no_address}' = 0 ]; then
                ip address add '${server_ip}' peer '${client_ip}' dev '${server_if}'
                ip -6 address add '${server_ipv6}' peer '${client_ipv6}' dev '${server_if}' nodad
            fi
            ip link set dev '${server_if}' mtu '${mtu}' up
            if [ '${no_address}' = 0 ]; then
                ip -6 route replace '${client_ipv6}/128' dev '${server_if}' metric 256
            fi
        "

        echo "[8] Configure remote networking"
        run_hook_remote "$tuntom_pre_hook" pre up remote "$server_if" "$server_ip" "$client_ip"
        net_up_remote
        run_hook_remote "$tuntom_post_hook" post up remote "$server_if" "$server_ip" "$client_ip"
    else
        echo "[8] Skip remote TUN/networking (switch port)"
    fi

    echo "[9] Start local client"
    "${root_cmd[@]}" sh -c \
        "nohup '${local_bin}' client '${instance}' '${client_if}' '${remote#*@}' --mtu '${mtu}' --transport-mtu '${transport_mtu}' --stats-format '${stats_format}' --stats-file '${local_stats_file}' --control-socket '${local_control_file}' ${crypto_option} ${stats_option}${client_switch_options} >'${local_log}' 2>&1 </dev/null & echo \$! > '${local_pid_file}'"

    if (( client_has_tun )); then
        for _ in $(seq 1 20); do
            if "${root_cmd[@]}" ip link show "$client_if" >/dev/null 2>&1; then
                break
            fi
            sleep 0.1
        done

        if (( ! no_address )); then
            "${root_cmd[@]}" ip address add "$client_ip" peer "$server_ip" dev "$client_if"
            "${root_cmd[@]}" ip -6 address add "$client_ipv6" peer "$server_ipv6" dev "$client_if" nodad
        fi
        "${root_cmd[@]}" ip link set dev "$client_if" mtu "$mtu" up
        if (( ! no_address )); then
            "${root_cmd[@]}" ip -6 route replace "${server_ipv6}/128" dev "$client_if" metric 256
        fi

        echo "[10] Configure local networking"
        run_hook_local "$tuntom_pre_hook" pre up local "$client_if" "$client_ip" "$server_ip"
        net_up_local
        run_hook_local "$tuntom_post_hook" post up local "$client_if" "$client_ip" "$server_ip"
    else
        echo "[10] Skip local TUN/networking (switch port)"
    fi
}

check_member_processes() {
    local_pid="$("${root_cmd[@]}" cat "$local_pid_file" 2>/dev/null || true)"
    if ! [[ "$local_pid" =~ ^[0-9]+$ ]] ||
       ! "${root_cmd[@]}" kill -0 "$local_pid" 2>/dev/null ||
       ! ssh "$remote" "pid=\$(cat '${remote_pid_file}' 2>/dev/null || true); case \"\$pid\" in ''|*[!0-9]*) exit 1;; esac; kill -0 \"\$pid\""; then
        echo "Tunnel process health check failed"
        echo "Local log:  $local_log"
        echo "Remote log: $remote_log"
        return 2
    fi
}

test_member() {
    echo "[11] Test"
    ping_failure=""
    if (( ! no_address && client_has_tun && server_has_tun )); then
        if ! "${root_cmd[@]}" ping -c 3 "$server_ip"; then
            ping_failure="IPv4 ping into the tunnel failed"
        fi

        ipv6_ping=ping
        if command -v ping6 >/dev/null 2>&1; then
            ipv6_ping=ping6
        fi

        if [[ -z "$ping_failure" ]] && \
           "${root_cmd[@]}" "$ipv6_ping" -c 3 "$server_ipv6"; then
            echo "Tunnel is UP (IPv4 + IPv6)"
        else
            if [[ -z "$ping_failure" ]]; then
                ping_failure="IPv6 ping into the tunnel failed"
            fi
            if (( switch_enabled )); then
                echo "WARNING: ${ping_failure}, but this is probably expected when using the switch"
            else
                echo "$ping_failure"
                if [[ "$ping_failure" == IPv6* ]]; then
                    echo "IPv6 address state:"
                    ip -6 address show dev "$client_if" || true
                    echo "IPv6 route state:"
                    ip -6 route get "$server_ipv6" || true
                fi
                echo "Local log:  $local_log"
                echo "Remote log: $remote_log"
                return 2
            fi
        fi
    fi

    if [[ -n "$ping_failure" ]] || (( no_address || ! client_has_tun || ! server_has_tun )); then
        sleep 1
        check_member_processes
        if [[ -n "$ping_failure" ]]; then
            echo "Tunnel processes are UP (switch mode; tunnel ping failure accepted)"
        elif (( no_address )); then
            echo "Tunnel processes are UP (--no-address; ping skipped)"
        else
            echo "Tunnel processes are UP (switch mode; ping skipped)"
        fi
    fi
}

source "${script_dir}/tools/mk_tunnel_group.sh"
group_main
