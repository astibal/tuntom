#!/usr/bin/env bash
set -euo pipefail
umask 0077

usage() {
    cat >&2 <<EOF
Usage: $0 <id 1..255> <host|user@host> [options]

Tunnel options:
  --snat | --no-snat
  --mss-clamp | --no-mss-clamp
  --crypto-auth-only | --no-stats | --stop

Switch ports (the tuntom-switch listener must already be running):
  --client-switch <socket> <port-id> <label>
  --server-switch <socket> <port-id> <label>
  --client-switch-exit-node
  --server-switch-exit-node
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
stop_requested=0
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

while (( $# > 0 )); do
    case "$1" in
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
        --no-stats)
            stats_option="--no-stats"
            ;;
        --crypto-auth-only)
            crypto_option="--crypto-auth-only"
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

client_switch_args=()
server_switch_args=()
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

if ! [[ "$id" =~ ^[0-9]+$ ]] || (( id < 1 || id > 255 )); then
    echo "Tunnel id must be in range 1..255" >&2
    exit 1
fi

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

if [[ "${TUNTOM_STATS_FORMAT:-txt}" != "txt" ]]; then
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

mtu="${TUNTOM_MTU:-1500}"
transport_mtu="${TUNTOM_TRANSPORT_MTU:-1400}"
udp_port="$((40000 + id))"

tuntom_mark_mask="${TUNTOM_MARK_MASK:-0x00ff0000}"
tuntom_mark="${TUNTOM_MARK:-$((id << 16))}"
tuntom_table="${TUNTOM_TABLE:-$((10000 + id))}"
tuntom_chain="${TUNTOM_CHAIN:-TUNTOM_${id}}"

# Optional hooks. Hook files exist ONLY on the caller/local host.
# For the remote side, the same local file is streamed over SSH to bash -s.
# Missing hook files are silently ignored.
tuntom_pre_hook="${TUNTOM_PRE_HOOK:-/etc/tuntom/tuntom-pre.sh}"
tuntom_post_hook="${TUNTOM_POST_HOOK:-/etc/tuntom/tuntom-post.sh}"

local_bin="/tmp/tuntom_${id}c"
remote_bin="/tmp/tuntom_${id}s"

# Build into separate staging files so a failed or slow compilation never
# touches the binaries used by the currently running tunnel.
local_stage="${local_bin}.new.$$"
remote_stage="${remote_bin}.new.$$"

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
    root_cmd=(sudo -E)
fi

mk_lock_file="${run_dir}/mk_${id}.lock"

acquire_mk_lock() {
    "${root_cmd[@]}" mkdir -p "$run_dir"

    coproc TUNTOM_MK_LOCK {
        "${root_cmd[@]}" bash -c '
            lock_file="$1"

            exec 9>"$lock_file"

            if ! flock -n 9; then
                exit 75
            fi

            printf "LOCKED\n"

            # Keep fd 9 and therefore the flock alive until the parent
            # mk_ script exits and closes this coprocess stdin pipe.
            cat >/dev/null
        ' bash "$mk_lock_file"
    }

    local lock_status=""

    if ! IFS= read -r lock_status <&"${TUNTOM_MK_LOCK[0]}"; then
        local lock_rc=0
        wait "$TUNTOM_MK_LOCK_PID" || lock_rc=$?

        if (( lock_rc == 75 )); then
            echo "Another mk_ process is already operating on tunnel ${id}" >&2
            echo "Lock: ${mk_lock_file}" >&2
        else
            echo "Unable to acquire mk_ lock ${mk_lock_file}" >&2
        fi

        exit 1
    fi

    if [[ "$lock_status" != "LOCKED" ]]; then
        echo "Unable to acquire mk_ lock ${mk_lock_file}" >&2
        exit 1
    fi

    echo "  mk_ lock:   ${mk_lock_file}"
}

acquire_mk_lock

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

stage_active=1

cleanup_staging() {
    if (( stage_active )); then
        rm -f "$local_stage" 2>/dev/null || true
        ssh "$remote" "rm -f '${remote_stage}'" >/dev/null 2>&1 || true
    fi
}

trap cleanup_staging EXIT



run_hook_local() {
    local hook="$1"
    local phase="$2"
    local action="$3"
    local side="$4"
    local tuntom_if="$5"
    local local_ip="$6"
    local peer_ip="$7"

    if [[ ! -f "$hook" ]]; then
        return 0
    fi

    echo "  hook ${phase}/${action}/${side}: ${hook}"

    "${root_cmd[@]}" env \
        TUNTOM_ID="$id" \
        TUNTOM_ACTION="$action" \
        TUNTOM_PHASE="$phase" \
        TUNTOM_SIDE="$side" \
        TUNTOM_IF="$tuntom_if" \
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
        TUNTOM_NAT_CHAIN="${tuntom_chain}_N" \
        TUNTOM_SNAT_CHAIN="${tuntom_chain}_S" \
        TUNTOM_MANGLE_CHAIN="${tuntom_chain}_M" \
        TUNTOM_FORWARD_CHAIN="${tuntom_chain}_F" \
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

    if [[ ! -f "$hook" ]]; then
        return 0
    fi

    echo "  hook ${phase}/${action}/${side}: ${hook} -> ${remote}"

    ssh "$remote" \
        "TUNTOM_ID='${id}' \
         TUNTOM_ACTION='${action}' \
         TUNTOM_PHASE='${phase}' \
         TUNTOM_SIDE='${side}' \
         TUNTOM_IF='${tuntom_if}' \
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
         TUNTOM_NAT_CHAIN='${tuntom_chain}_N' \
         TUNTOM_SNAT_CHAIN='${tuntom_chain}_S' \
         TUNTOM_MANGLE_CHAIN='${tuntom_chain}_M' \
         TUNTOM_FORWARD_CHAIN='${tuntom_chain}_F' \
         bash -s" < "$hook"
}

hook_pre_down_local() {
    run_hook_local "$tuntom_pre_hook" pre down local "$client_if" "$client_ip" "$server_ip" || \
        echo "WARNING: local pre/down hook failed" >&2
}

hook_post_down_local() {
    run_hook_local "$tuntom_post_hook" post down local "$client_if" "$client_ip" "$server_ip" || \
        echo "WARNING: local post/down hook failed" >&2
}

hook_pre_down_remote() {
    run_hook_remote "$tuntom_pre_hook" pre down remote "$server_if" "$server_ip" "$client_ip" || \
        echo "WARNING: remote pre/down hook failed" >&2
}

hook_post_down_remote() {
    run_hook_remote "$tuntom_post_hook" post down remote "$server_if" "$server_ip" "$client_ip" || \
        echo "WARNING: remote post/down hook failed" >&2
}

# Run this function with the same privileges used to launch the tunnel.
# PID files are only bookkeeping: older/orphaned instances may not be listed.
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
        "$local_bin" client "$id" "$client_if" "$local_pid_file"
}

stop_remote_process() {
    ssh "$remote" "bash -s -- '${remote_bin}' server '${id}' '${server_if}' '${remote_pid_file}'" <<< \
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
        " || true
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
    " || true
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

if (( stop_requested )); then
    echo "Stopping tunnel ${id}"
    stop_local_process
    stop_remote_process
    "${root_cmd[@]}" rm -f "$local_stats_file" "$local_control_file" 2>/dev/null || true
    ssh "$remote" "rm -f '${remote_stats_file}' '${remote_control_file}'" >/dev/null 2>&1 || true

    hook_pre_down_local
    hook_pre_down_remote
    net_down_local
    net_down_remote
    hook_post_down_local
    hook_post_down_remote

    "${root_cmd[@]}" ip link del "$client_if" 2>/dev/null || true
    ssh "$remote" "ip link del '${server_if}' 2>/dev/null || true"
    echo "Tunnel ${id} stopped"
    exit 0
fi

echo "Tunnel ${id}"
echo "  remote:     ${remote}"
echo "  client if:  ${client_if} ${client_ip} -> ${server_ip}"
echo "  server if:  ${server_if} ${server_ip} -> ${client_ip}"
echo "  client IPv6: ${client_ipv6} -> ${server_ipv6}"
echo "  server IPv6: ${server_ipv6} -> ${client_ipv6}"
echo "  UDP port:   ${udp_port}"
echo "  TUN MTU:    ${mtu}"
echo "  xport MTU:  ${transport_mtu}"
echo "  SNAT:       ${tuntom_snat}"
echo "  MSS clamp:  ${tuntom_mss_clamp}"
if [[ -n "$client_switch_socket" ]]; then
    echo "  client switch: ${client_switch_socket} port=${client_switch_port_id} label=${client_switch_label} exit=${client_switch_exit_node}"
fi
if [[ -n "$server_switch_socket" ]]; then
    echo "  server switch: ${server_switch_socket} port=${server_switch_port_id} label=${server_switch_label} exit=${server_switch_exit_node}"
fi
echo "  stats:      ${stats_format} -> ${run_dir}/${id}{c,s}.stats"
echo "  pre hook:   ${tuntom_pre_hook} (local file, runs local+remote)"
echo "  post hook:  ${tuntom_post_hook} (local file, runs local+remote)"
echo "  protocol:   v5 / Ascon auth + replay protection + fragmentation"

echo "[1] Compile local staging binary"
rm -f "$local_stage"
g++ -std=c++17 -O3 -march=native -mtune=native -Wall -Wextra -pedantic "$source_dir/main.cpp" -o "$local_stage"
test -x "$local_stage"

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
g++ -std=c++17 -O3 -march=native -mtune=native -Wall -Wextra -pedantic "$build_dir/src/main.cpp" -o "$stage"
test -x "$stage"
REMOTE_BUILD
)
tar -C "$script_dir" -cf - src | \
    ssh -o BatchMode=yes "$remote" "stage='${remote_stage}'; ${remote_build_command}"

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

# Up to this point the currently running tunnel is untouched. Only after all
# preparation succeeds do we perform the short switchover.
echo "[4] Stop previous processes"
stop_local_process
stop_remote_process
"${root_cmd[@]}" rm -f "$local_stats_file" "$local_control_file" 2>/dev/null || true
ssh "$remote" "rm -f '${remote_stats_file}' '${remote_control_file}'" >/dev/null 2>&1 || true

echo "[5] Clean previous networking"
hook_pre_down_local
hook_pre_down_remote
net_down_local
net_down_remote
hook_post_down_local
hook_post_down_remote

"${root_cmd[@]}" ip link del "$client_if" 2>/dev/null || true
ssh "$remote" "ip link del '${server_if}' 2>/dev/null || true"

echo "[6] Install staged binaries"
"${root_cmd[@]}" mv -f "$local_stage" "$local_bin"
ssh "$remote" "mv -f '${remote_stage}' '${remote_bin}'"
stage_active=0

echo "[7] Start remote server"
printf '%s\n' "$TUNTOM_SECRET" | ssh "$remote" "
    read -r TUNTOM_SECRET
    export TUNTOM_SECRET
    nohup '${remote_bin}' server '${id}' '${server_if}' \
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
        ip address add '${server_ip}' peer '${client_ip}' dev '${server_if}' &&
        ip -6 address add '${server_ipv6}' peer '${client_ipv6}' dev '${server_if}' nodad &&
        ip link set dev '${server_if}' mtu '${mtu}' up &&
        ip -6 route replace '${client_ipv6}/128' dev '${server_if}' metric 256
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
    "nohup '${local_bin}' client '${id}' '${client_if}' '${remote#*@}' --mtu '${mtu}' --transport-mtu '${transport_mtu}' --stats-format '${stats_format}' --stats-file '${local_stats_file}' --control-socket '${local_control_file}' ${crypto_option} ${stats_option}${client_switch_options} >'${local_log}' 2>&1 </dev/null & echo \$! > '${local_pid_file}'"

if (( client_has_tun )); then
    for _ in $(seq 1 20); do
        if "${root_cmd[@]}" ip link show "$client_if" >/dev/null 2>&1; then
            break
        fi
        sleep 0.1
    done

    "${root_cmd[@]}" ip address add "$client_ip" peer "$server_ip" dev "$client_if"
    "${root_cmd[@]}" ip -6 address add "$client_ipv6" peer "$server_ipv6" dev "$client_if" nodad
    "${root_cmd[@]}" ip link set dev "$client_if" mtu "$mtu" up
    "${root_cmd[@]}" ip -6 route replace "${server_ipv6}/128" dev "$client_if" metric 256

    echo "[10] Configure local networking"
    run_hook_local "$tuntom_pre_hook" pre up local "$client_if" "$client_ip" "$server_ip"
    net_up_local
    run_hook_local "$tuntom_post_hook" post up local "$client_if" "$client_ip" "$server_ip"
else
    echo "[10] Skip local TUN/networking (switch port)"
fi

echo "[11] Test"
if (( client_has_tun && server_has_tun )) && "${root_cmd[@]}" ping -c 3 "$server_ip"; then
    ipv6_ping=ping
    if command -v ping6 >/dev/null 2>&1; then
        ipv6_ping=ping6
    fi

    if "${root_cmd[@]}" "$ipv6_ping" -c 3 "$server_ipv6"; then
        echo "Tunnel is UP (IPv4 + IPv6)"
    else
        echo "IPv6 ping failed"
        echo "IPv6 address state:"
        ip -6 address show dev "$client_if" || true
        echo "IPv6 route state:"
        ip -6 route get "$server_ipv6" || true
        echo "Local log:  $local_log"
        echo "Remote log: $remote_log"
        exit 2
    fi
elif (( client_has_tun && server_has_tun )); then
    echo "Ping failed"
    echo "Local log:  $local_log"
    echo "Remote log: $remote_log"
    exit 2
else
    sleep 1
    local_pid="$(cat "$local_pid_file" 2>/dev/null || true)"
    if ! [[ "$local_pid" =~ ^[0-9]+$ ]] ||
       ! "${root_cmd[@]}" kill -0 "$local_pid" 2>/dev/null ||
       ! ssh "$remote" "pid=\$(cat '${remote_pid_file}' 2>/dev/null || true); case \"\$pid\" in ''|*[!0-9]*) exit 1;; esac; kill -0 \"\$pid\""; then
        echo "Switch-mode process health check failed"
        echo "Local log:  $local_log"
        echo "Remote log: $remote_log"
        exit 2
    fi
    echo "Tunnel processes are UP (switch mode; ping skipped)"
fi
