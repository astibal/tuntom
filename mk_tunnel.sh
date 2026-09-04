#!/usr/bin/env bash
set -euo pipefail
umask 0077

if (( $# < 2 )); then
    echo "Usage: $0 <id 1..255> <host|user@host> [--snat|--no-snat] [--mss-clamp|--no-mss-clamp] [--stop]" >&2
    exit 1
fi

id="$1"
remote="$2"
shift 2

tuntom_snat=0
tuntom_mss_clamp=1
stop_requested=0

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
        --stop)
            stop_requested=1
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 <id 1..255> <host|user@host> [--snat|--no-snat] [--mss-clamp|--no-mss-clamp] [--stop]" >&2
            exit 1
            ;;
    esac

    shift
done

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
source_file="${script_dir}/udp_tun.cpp"
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
stats_format="${TUNTOM_STATS_FORMAT:-txt}"

runtime_user="tuntom"
runtime_group="tuntom"

if [[ ! -f "$source_file" ]]; then
    echo "Missing source file: $source_file" >&2
    exit 1
fi

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

stop_local_process() {
    if [[ ! -f "$local_pid_file" ]]; then
        return
    fi

    local pid
    pid="$(cat "$local_pid_file" 2>/dev/null || true)"

    if [[ "$pid" =~ ^[0-9]+$ ]]; then
        local argv0
        argv0="$("${root_cmd[@]}" sh -c "tr '\\0' '\\n' < '/proc/${pid}/cmdline' 2>/dev/null | head -n 1" || true)"

        if [[ "$argv0" == "$local_bin" ]]; then
            "${root_cmd[@]}" kill "$pid" 2>/dev/null || true
            for _ in $(seq 1 20); do
                if ! "${root_cmd[@]}" kill -0 "$pid" 2>/dev/null; then
                    break
                fi
                sleep 0.05
            done
            if "${root_cmd[@]}" kill -0 "$pid" 2>/dev/null; then
                "${root_cmd[@]}" kill -KILL "$pid" 2>/dev/null || true
            fi
        else
            echo "WARNING: stale local PID file ${local_pid_file}: PID ${pid} is not ${local_bin}" >&2
        fi
    fi

    "${root_cmd[@]}" rm -f "$local_pid_file"
}

stop_remote_process() {
    ssh "$remote" "
        if [ -f '${remote_pid_file}' ]; then
            pid=\$(cat '${remote_pid_file}' 2>/dev/null || true)
            case \"\$pid\" in
                ''|*[!0-9]*) ;;
                *)
                    argv0=\$(tr '\\000' '\\n' < \"/proc/\$pid/cmdline\" 2>/dev/null | head -n 1 || true)
                    if [ \"\$argv0\" = '${remote_bin}' ]; then
                        kill \"\$pid\" 2>/dev/null || true
                        n=0
                        while kill -0 \"\$pid\" 2>/dev/null && [ \"\$n\" -lt 20 ]; do
                            sleep 0.05
                            n=\$((n + 1))
                        done
                        if kill -0 \"\$pid\" 2>/dev/null; then
                            kill -KILL \"\$pid\" 2>/dev/null || true
                        fi
                    else
                        echo \"WARNING: stale remote PID file ${remote_pid_file}: PID \$pid is not ${remote_bin}\" >&2
                    fi
                    ;;
            esac
            rm -f '${remote_pid_file}'
        fi
    "
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
    "${root_cmd[@]}" rm -f "$local_stats_file" 2>/dev/null || true
    ssh "$remote" "rm -f '${remote_stats_file}'" >/dev/null 2>&1 || true

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
echo "  stats:      ${stats_format} -> ${run_dir}/${id}{c,s}.stats"
echo "  pre hook:   ${tuntom_pre_hook} (local file, runs local+remote)"
echo "  post hook:  ${tuntom_post_hook} (local file, runs local+remote)"
echo "  protocol:   v3 / Ascon auth + replay protection + fragmentation"

echo "[1] Compile local staging binary"
rm -f "$local_stage"
g++ -std=c++17 -O2 -Wall -Wextra -pedantic "$source_file" -o "$local_stage"
test -x "$local_stage"

echo "[2] Compile remote staging binary"
ssh -o BatchMode=yes "$remote" "rm -f '${remote_stage}'"
ssh -o BatchMode=yes "$remote" \
    "g++ -x c++ -std=c++17 -O2 -Wall -Wextra -pedantic -o '${remote_stage}' - && test -x '${remote_stage}'" \
    < "$source_file"

echo "[3] Deploy network helper"
ssh "$remote" "cat > '${remote_net_file}' && chmod 700 '${remote_net_file}'" < "$net_file"

ensure_runtime_account_local
ensure_runtime_account_remote

"${root_cmd[@]}" mkdir -p "$run_dir"
"${root_cmd[@]}" chown root:"$runtime_group" "$run_dir"
"${root_cmd[@]}" chmod 0770 "$run_dir"

ssh "$remote" "
    mkdir -p '${run_dir}'
    chown root:'${runtime_group}' '${run_dir}'
    chmod 0770 '${run_dir}'
"

# Up to this point the currently running tunnel is untouched. Only after all
# preparation succeeds do we perform the short switchover.
echo "[4] Stop previous processes"
stop_local_process
stop_remote_process
"${root_cmd[@]}" rm -f "$local_stats_file" 2>/dev/null || true
ssh "$remote" "rm -f '${remote_stats_file}'" >/dev/null 2>&1 || true

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
        --stats-file '${remote_stats_file}' \
        >'${remote_log}' 2>&1 </dev/null &
    echo \$! > '${remote_pid_file}'
"

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

echo "[9] Start local client"
"${root_cmd[@]}" sh -c \
    "nohup '${local_bin}' client '${id}' '${client_if}' '${remote#*@}' --mtu '${mtu}' --transport-mtu '${transport_mtu}' --stats-format '${stats_format}' --stats-file '${local_stats_file}' >'${local_log}' 2>&1 </dev/null & echo \$! > '${local_pid_file}'"

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

echo "[11] Test"
if "${root_cmd[@]}" ping -c 3 "$server_ip"; then
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
else
    echo "Ping failed"
    echo "Local log:  $local_log"
    echo "Remote log: $remote_log"
    exit 2
fi
