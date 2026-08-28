#!/usr/bin/env bash
set -euo pipefail

if (( $# < 2 )); then
    echo "Usage: $0 <id 1..255> <host|user@host> [--snat|--no-snat] [--mss-clamp|--no-mss-clamp]" >&2
    exit 1
fi

id="$1"
remote="$2"
shift 2

tuntom_snat=0
tuntom_mss_clamp=1

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
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 <id 1..255> <host|user@host> [--snat|--no-snat] [--mss-clamp|--no-mss-clamp]" >&2
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

if [[ -z "${TUNTOM_SECRET:-}" ]]; then
    echo "TUNTOM_SECRET is not set" >&2
    echo "Use a 128-bit key encoded as exactly 32 hex characters." >&2
    exit 1
fi

if ! [[ "$TUNTOM_SECRET" =~ ^[0-9A-Fa-f]{32}$ ]]; then
    echo "TUNTOM_SECRET must contain exactly 32 hex characters" >&2
    exit 1
fi

export TUNTOM_SECRET

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source_file="${script_dir}/udp_tun.cpp"
net_file="${script_dir}/tuntom-net.sh"

client_if="ut${id}c"
server_if="ut${id}s"
client_ip="10.254.${id}.1"
server_ip="10.254.${id}.2"
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

local_bin="/tmp/udp_tun-${id}-client"
remote_bin="/tmp/udp_tun-${id}-server"
remote_net_file="/tmp/tuntom-net-${id}.sh"

local_log="/tmp/udp_tun-${id}-client.log"
remote_log="/tmp/udp_tun-${id}-server.log"
local_pid_file="/tmp/udp_tun-${id}-client.pid"
remote_pid_file="/tmp/udp_tun-${id}-server.pid"

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

    if [[ "$pid" =~ ^[0-9]+$ ]] && [[ -e "/proc/${pid}/exe" ]]; then
        local exe
        exe="$(readlink -f "/proc/${pid}/exe" 2>/dev/null || true)"
        if [[ "$exe" == "$local_bin" ]]; then
            "${root_cmd[@]}" kill "$pid" 2>/dev/null || true
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
                    if [ -e \"/proc/\$pid/exe\" ]; then
                        exe=\$(readlink -f \"/proc/\$pid/exe\" 2>/dev/null || true)
                        if [ \"\$exe\" = '${remote_bin}' ]; then
                            kill \"\$pid\" 2>/dev/null || true
                        fi
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

echo "Tunnel ${id}"
echo "  remote:     ${remote}"
echo "  client if:  ${client_if} ${client_ip} -> ${server_ip}"
echo "  server if:  ${server_if} ${server_ip} -> ${client_ip}"
echo "  UDP port:   ${udp_port}"
echo "  TUN MTU:    ${mtu}"
echo "  xport MTU:  ${transport_mtu}"
echo "  SNAT:       ${tuntom_snat}"
echo "  MSS clamp:  ${tuntom_mss_clamp}"
echo "  pre hook:   ${tuntom_pre_hook} (local file, runs local+remote)"
echo "  post hook:  ${tuntom_post_hook} (local file, runs local+remote)"
echo "  protocol:   v2 / Ascon auth + sequence replay protection"

echo "[1] Compile local"
g++ -std=c++17 -O2 -Wall -Wextra -pedantic "$source_file" -o "$local_bin"

echo "[2] Compile remote"
ssh -o BatchMode=yes "$remote" \
    "g++ -x c++ -std=c++17 -O2 -Wall -Wextra -pedantic -o '${remote_bin}' -" \
    < "$source_file"

echo "[3] Deploy network helper"
ssh "$remote" "cat > '${remote_net_file}' && chmod 700 '${remote_net_file}'" < "$net_file"

echo "[4] Stop previous processes"
stop_local_process
stop_remote_process

echo "[5] Clean previous networking"
hook_pre_down_local
hook_pre_down_remote
net_down_local
net_down_remote
hook_post_down_local
hook_post_down_remote

"${root_cmd[@]}" ip link del "$client_if" 2>/dev/null || true
ssh "$remote" "ip link del '${server_if}' 2>/dev/null || true"

echo "[6] Start remote server"
printf '%s\n' "$TUNTOM_SECRET" | ssh "$remote" "
    read -r TUNTOM_SECRET
    export TUNTOM_SECRET
    nohup '${remote_bin}' server '${id}' '${server_if}' \
        --mtu '${mtu}' \
        --transport-mtu '${transport_mtu}' \
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
    ip link set dev '${server_if}' mtu '${mtu}' up
"

echo "[7] Configure remote networking"
run_hook_remote "$tuntom_pre_hook" pre up remote "$server_if" "$server_ip" "$client_ip"
net_up_remote
run_hook_remote "$tuntom_post_hook" post up remote "$server_if" "$server_ip" "$client_ip"

echo "[8] Start local client"
"${root_cmd[@]}" sh -c \
    "nohup '${local_bin}' client '${id}' '${client_if}' '${remote#*@}' --mtu '${mtu}' --transport-mtu '${transport_mtu}' >'${local_log}' 2>&1 </dev/null & echo \$! > '${local_pid_file}'"

for _ in $(seq 1 20); do
    if "${root_cmd[@]}" ip link show "$client_if" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

"${root_cmd[@]}" ip address add "$client_ip" peer "$server_ip" dev "$client_if"
"${root_cmd[@]}" ip link set dev "$client_if" mtu "$mtu" up

echo "[9] Configure local networking"
run_hook_local "$tuntom_pre_hook" pre up local "$client_if" "$client_ip" "$server_ip"
net_up_local
run_hook_local "$tuntom_post_hook" post up local "$client_if" "$client_ip" "$server_ip"

echo "[10] Test"
if "${root_cmd[@]}" ping -I "$client_if" -c 3 -W 2 "$server_ip"; then
    echo "Tunnel is UP"
else
    echo "Ping failed"
    echo "Local log:  $local_log"
    echo "Remote log: $remote_log"
    exit 2
fi
