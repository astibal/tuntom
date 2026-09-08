#!/usr/bin/env bash
# Shared local lifecycle for mk_switch.sh and mk_adapter.sh. Linux /proc required.

local_die() { echo "ERROR: $*" >&2; exit 1; }

local_value() {
    (( $# >= 2 )) && [[ -n "$2" && "$2" != --* ]] ||
        local_die "$1 requires a value"
}

local_number() {
    [[ "$2" =~ ^[0-9]{1,9}$ ]] && (( 10#$2 >= $3 && 10#$2 <= $4 )) ||
        local_die "$1 must be in range $3..$4"
}

local_init() {
    kind="$1"; shift
    if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then usage; exit 0; fi
    (( $# )) || { usage >&2; exit 1; }
    name="$1"
    [[ "$name" =~ ^[a-zA-Z0-9][a-zA-Z0-9_.-]{0,62}$ ]] ||
        local_die "Invalid instance name"
    if [[ "$kind" == adapter && ${#name} -gt 15 ]]; then
        local_die "Interface name must have at most 15 bytes"
    fi
    original_args=("$@")
    run_dir="${TUNTOM_RUN_DIR:-/run/tuntom}"
    state_root="${TUNTOM_STATE_DIR:-/run/tuntom-mk}"
    bin_root="${TUNTOM_BIN_DIR:-/var/lib/tuntom-mk}"
    switch_socket=""; port_id=""; mtu="${TUNTOM_MTU:-1500}"
    if [[ "$kind" == switch ]]; then switch_socket="${run_dir}/${name}.sock"; fi
    control_socket="${run_dir}/${name}.control"
    socket_owner="${TUNTOM_SOCKET_OWNER:-tuntom:tuntom}"
    if [[ "$kind" == switch ]]; then
        pre_hook="${TUNTOM_SWITCH_PRE_HOOK:-/etc/tuntom/switch-pre.sh}"
        post_hook="${TUNTOM_SWITCH_POST_HOOK:-/etc/tuntom/switch-post.sh}"
    else
        pre_hook="${TUNTOM_ADAPTER_PRE_HOOK:-/etc/tuntom/adapter-pre.sh}"
        post_hook="${TUNTOM_ADAPTER_POST_HOOK:-/etc/tuntom/adapter-post.sh}"
    fi
    stop_requested=0; service_args=(); stage=""; starting=0; probe_pid=""
    rules_source=""; rules_file=""
}

local_option() {
    option_shift=0
    case "$1" in
        --control-socket|--pre-hook|--post-hook|--socket-owner)
            local_value "$@"; option_shift=1
            case "$1" in
                --control-socket) control_socket="$2" ;;
                --pre-hook) pre_hook="$2"; [[ -f "$2" ]] || local_die "Missing hook: $2" ;;
                --post-hook) post_hook="$2"; [[ -f "$2" ]] || local_die "Missing hook: $2" ;;
                --socket-owner) socket_owner="$2" ;;
            esac ;;
        --stop) stop_requested=1 ;;
        --help|-h) usage; exit 0 ;;
        *) local_die "Unknown option: $1" ;;
    esac
}

local_socket_path() {
    local LC_ALL=C
    [[ "$1" == /* && ${#1} -le 107 && "$1" != *$'\n'* ]] ||
        local_die "Socket path must be absolute and at most 107 bytes: $1"
}

local_validate() {
    [[ "$run_dir" == /* && "$state_root" == /* && "$bin_root" == /* &&
       "$run_dir" != / && "$state_root" != / && "$bin_root" != / ]] ||
        local_die "Runtime, state and binary directories must be absolute, non-root paths"
    local_socket_path "$control_socket"
    if [[ -n "$switch_socket" ]]; then local_socket_path "$switch_socket"; fi
    [[ "$switch_socket" != "$control_socket" ]] ||
        local_die "Data and control sockets must have different paths"
    [[ "$pre_hook" == /* ]] || pre_hook="$PWD/$pre_hook"
    [[ "$post_hook" == /* ]] || post_hook="$PWD/$post_hook"
}

local_require_root() {
    if (( EUID != 0 )); then
        exec sudo -E bash "${script_dir}/mk_${kind}.sh" "${original_args[@]}"
    fi
}

local_runtime_account() {
    if ! getent group tuntom >/dev/null; then groupadd --system tuntom; fi
    if ! id -u tuntom >/dev/null 2>&1; then
        useradd --system --gid tuntom --no-create-home --home-dir /nonexistent \
            --shell /usr/sbin/nologin tuntom
    fi
    [[ "$(id -g tuntom)" == "$(getent group tuntom | cut -d: -f3)" ]] ||
        local_die "Runtime user tuntom does not use group tuntom"
    mkdir -p -- "$run_dir"
    chown root:tuntom "$run_dir"
    chmod 2770 "$run_dir"
}

local_private_dir() {
    mkdir -p -- "$1"
    [[ "$(stat -Lc %u "$1")" == "$EUID" ]] || local_die "Directory has a different owner: $1"
    chmod 700 "$1"
}

# Start time identifies a process even if a PID is later reused. Zombies have
# released their sockets and TUN; they need no further signals.
local_identity() {
    local stat rest
    local -a fields
    { IFS= read -r stat < "/proc/$1/stat"; } 2>/dev/null || return 1
    rest="${stat##*) }"
    read -r -a fields <<< "$rest"
    [[ "${fields[0]}" != Z && "${fields[0]}" != X ]] || return 1
    printf '%s\n' "${fields[19]}"
}

local_find_processes() {
    local proc pid identity exe
    local -a argv
    targets=(); identities=()
    for proc in /proc/[0-9]*; do
        pid="${proc##*/}"
        argv=()
        { mapfile -d '' -t argv < "$proc/cmdline"; } 2>/dev/null || continue
        # Also recognize instances started before binaries moved out of /run.
        [[ "${argv[0]:-}" == "$binary" || "${argv[0]:-}" == "${instance_dir}/main" ]] || continue
        exe="$(readlink -- "$proc/exe" 2>/dev/null)" || continue
        [[ "$exe" == "${argv[0]}" || "$exe" == "${argv[0]} (deleted)" ]] || continue
        identity="$(local_identity "$pid")" || continue
        targets+=("$pid"); identities+=("$identity")
    done
}

local_stop_processes() {
    local index pid identity attempt current
    local_find_processes
    for index in "${!targets[@]}"; do
        pid="${targets[index]}"; identity="${identities[index]}"
        for ((attempt=0; attempt<60; ++attempt)); do
            current="$(local_identity "$pid")" || break
            [[ "$current" == "$identity" ]] || break
            if (( attempt == 0 )); then
                echo "Stopping $kind $name PID $pid"
                kill -TERM "$pid" 2>/dev/null || true
            elif (( attempt == 40 )); then
                kill -KILL "$pid" 2>/dev/null || true
            fi
            sleep 0.05
        done
        (( attempt < 60 )) || { echo "ERROR: PID $pid did not stop" >&2; return 1; }
        wait "$pid" 2>/dev/null || true
    done
    rm -f -- "$pid_file"
}

local_socket_bound() {
    local num refs protocol flags type state inode path
    while read -r num refs protocol flags type state inode path; do
        # -ef also catches alternative spellings and hard links to a live socket.
        if [[ "$path" == "$1" || ( -n "$path" && "$path" -ef "$1" ) ]]; then return 0; fi
    done < /proc/net/unix
    return 1
}

local_remove_socket() {
    local path="$1"
    [[ -n "$path" ]] || return 0
    if local_socket_bound "$path"; then
        echo "ERROR: Socket is still in use: $path" >&2; return 1
    fi
    if [[ -L "$path" || ( -e "$path" && ! -S "$path" ) ]]; then
        echo "ERROR: Refusing to remove non-socket: $path" >&2; return 1
    fi
    if [[ -S "$path" ]]; then rm -- "$path"; fi
}

local_hook() {
    local phase="$1" action="$2" hook="$pre_hook" interface=""
    local hook_binary="$binary" hook_ctl="$control_binary"
    [[ "$phase" != post ]] || hook="$post_hook"
    [[ -f "$hook" ]] || return 0
    [[ "$kind" != adapter ]] || interface="$name"
    if [[ "$action" == down && ! -e "$binary" && -e "${instance_dir}/main" ]]; then
        hook_binary="${instance_dir}/main"; hook_ctl="${instance_dir}/tuntomctl"
    fi
    echo "  hook $phase/$action/local: $hook"
    env TUNTOM_COMPONENT="$kind" TUNTOM_ID="$name" TUNTOM_SIDE=local \
        TUNTOM_PHASE="$phase" TUNTOM_ACTION="$action" TUNTOM_IF="$interface" \
        TUNTOM_SWITCH_SOCKET="$switch_socket" TUNTOM_CONTROL_SOCKET="$control_socket" \
        TUNTOM_SWITCH_PORT_ID="$port_id" TUNTOM_MTU="$mtu" \
        TUNTOM_SOCKET_OWNER="$socket_owner" TUNTOM_RULES_FILE="$rules_file" \
        TUNTOM_BIN="$hook_binary" TUNTOM_CTL="$hook_ctl" \
        TUNTOM_PID_FILE="$pid_file" TUNTOM_LOG_FILE="$log_file" bash "$hook"
}

local_down() (
    # Hooks and cleanup must use the previous endpoints, including --stop with
    # only the instance name, or a restart that changes socket paths/port/MTU.
    local -a saved
    local_find_processes
    if [[ ! -f "$state_file" && ${#targets[@]} -eq 0 ]]; then
        rm -f -- "$pid_file"
        return 0
    fi
    if [[ -f "$state_file" ]]; then
        mapfile -d '' -t saved < "$state_file"
        (( ${#saved[@]} == 7 )) || { echo "ERROR: Invalid saved state" >&2; return 1; }
        switch_socket="${saved[0]}"; control_socket="${saved[1]}"
        port_id="${saved[2]}"; mtu="${saved[3]}"; socket_owner="${saved[4]}"
        pre_hook="${saved[5]}"; post_hook="${saved[6]}"
    fi
    rules_file="${instance_dir}/rules"
    local_hook pre down || echo "WARNING: pre/down hook failed" >&2
    local_stop_processes || return 1
    local_remove_socket "$control_socket" || return 1
    if [[ "$kind" == switch ]]; then local_remove_socket "$switch_socket" || return 1; fi
    local_hook post down || echo "WARNING: post/down hook failed" >&2
    rm -f -- "$state_file"
)

local_wait_ready() {
    local pid="$1" identity="$2" ctl="$3" socket="$4" component="$5"
    local deadline=$((SECONDS + 10)) current stats
    while (( SECONDS < deadline )); do
        current="$(local_identity "$pid")" || return 1
        [[ "$current" == "$identity" ]] || return 1
        if [[ -S "$socket" ]] && stats="$(timeout 1 "$ctl" "$socket" show stats 2>/dev/null)" &&
           [[ $'\n'"$stats"$'\n' == *$'\n'"component=$component"$'\n'* ]]; then
            return 0
        fi
        sleep 0.05
    done
    return 1
}

local_read_rules() {
    local directive value extra line=0
    while read -r directive value extra || [[ -n "$directive$value$extra" ]]; do
        line=$((line + 1))
        [[ -n "$directive" && "$directive" != \#* ]] || continue
        [[ -n "$value" && -z "$extra" ]] || local_die "Invalid rules line $line"
        case "$directive" in
            route|exit-port) service_args+=("--$directive" "$value") ;;
            default-back)
                [[ "$value" == on || "$value" == off ]] || local_die "Invalid default-back on line $line"
                service_args+=("--default-back=$value") ;;
            *) local_die "Unknown rules directive on line $line: $directive" ;;
        esac
    done < "$rules_file"
}

local_check_endpoints() {
    local path old_data="" old_control=""
    local -a saved paths=("$control_socket")
    [[ "$kind" != switch ]] || paths+=("$switch_socket")
    local_find_processes
    if [[ -f "$state_file" && ${#targets[@]} -gt 0 ]]; then
        mapfile -d '' -t saved < "$state_file"
        old_data="${saved[0]:-}"; old_control="${saved[1]:-}"
    fi
    for path in "${paths[@]}"; do
        [[ ! -L "$path" && ( ! -e "$path" || -S "$path" ) ]] ||
            local_die "Socket path is a symlink or non-socket: $path"
        if local_socket_bound "$path" && [[ "$path" != "$old_control" &&
             ( "$kind" != switch || "$path" != "$old_data" ) ]]; then
            local_die "Socket is used by another instance: $path"
        fi
    done
    if [[ "$kind" == adapter && ${#targets[@]} -eq 0 ]] && ip link show dev "$name" >/dev/null 2>&1; then
        local_die "Interface $name already exists; refusing to take it over"
    fi
}

local_check_switch() {
    # Exercise the real parser on isolated sockets before stopping the old switch.
    # This catches malformed labels, duplicate routes and startup errors without
    # duplicating the binary's option parser in shell.
    "${stage}/main" --socket "${stage}/probe.sock" \
        --control-socket "${stage}/probe.control" "${service_args[@]}" \
        >"${stage}/probe.log" 2>&1 </dev/null 9>&- &
    probe_pid=$!
    local identity
    identity="$(local_identity "$probe_pid")" || identity=""
    if ! local_wait_ready "$probe_pid" "$identity" "${stage}/ctl" "${stage}/probe.control" switch; then
        cat "${stage}/probe.log" >&2
        local_die "Switch configuration check failed; current instance was kept running"
    fi
    kill -TERM "$probe_pid"
    wait "$probe_pid"
    probe_pid=""
}

local_cleanup() {
    local rc=$?
    trap - EXIT
    if [[ -n "$probe_pid" ]]; then
        kill -KILL "$probe_pid" 2>/dev/null || true
        wait "$probe_pid" 2>/dev/null || true
    fi
    if (( starting )); then
        echo "ERROR: $kind $name failed to start; cleaning up. Log: $log_file" >&2
        tail -n 20 "$log_file" >&2 || true
        local_down || echo "ERROR: Cleanup failed; retry --stop" >&2
    fi
    if [[ -n "$stage" ]]; then rm -rf -- "$stage"; fi
    exit "$rc"
}

local_run() {
    local_require_root
    local dependency
    for dependency in flock timeout readlink; do
        command -v "$dependency" >/dev/null || local_die "Missing command: $dependency"
    done
    # Keep locks/metadata outside group-writable sockets. /run may be noexec:
    # executables and their staging files belong on a separate executable mount.
    local_private_dir "$state_root"
    state_root="$(cd -- "$state_root" && pwd -P)"
    bin_root="$(readlink -m -- "$bin_root")"
    instance_dir="${state_root}/${kind}-${name}"
    bin_dir="${bin_root}/${kind}-${name}"
    [[ ! -L "$instance_dir" ]] || local_die "Instance directory is a symlink"
    mkdir -p -- "$instance_dir"
    chmod 700 "$instance_dir"
    exec 9>"${instance_dir}/lock"
    flock -n 9 || local_die "Another mk_ process is operating on $kind $name"
    binary="${bin_dir}/main"; control_binary="${bin_dir}/tuntomctl"
    pid_file="${instance_dir}/pid"; state_file="${instance_dir}/endpoints"
    log_file="${instance_dir}/log"; rules_file="${instance_dir}/rules"
    trap local_cleanup EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    if (( stop_requested )); then
        local_down
        echo "$kind $name stopped"
        return
    fi

    local_private_dir "$bin_root"
    [[ ! -L "$bin_dir" ]] || local_die "Binary directory is a symlink"
    local_private_dir "$bin_dir"
    command -v "${CXX:-g++}" >/dev/null || local_die "Missing compiler: ${CXX:-g++}"
    if [[ "$kind" == adapter ]]; then
        command -v ip >/dev/null || local_die "Missing command: ip"
    fi
    local_runtime_account
    local path
    for path in "$control_socket" "$switch_socket"; do
        [[ -d "$(dirname -- "$path")" ]] || local_die "Missing socket directory: $path"
    done
    stage="$(mktemp -d "${bin_root}/stage.XXXXXXXX")"
    printf '#!/bin/sh\nexit 0\n' > "${stage}/exec-check"
    chmod 700 "${stage}/exec-check"
    if ! "${stage}/exec-check" >/dev/null 2>&1; then
        local_die "Cannot execute files in $bin_root (check noexec/permissions); set TUNTOM_BIN_DIR to a directory on an executable filesystem"
    fi
    touch "${stage}/owner-check"
    chown "$socket_owner" "${stage}/owner-check"
    local_check_endpoints
    echo "[1] Compile staged $kind and tuntomctl"
    "${CXX:-g++}" -std=c++17 -pthread -O3 -march=native -mtune=native -Wall -Wextra -pedantic \
        "${script_dir}/src/${kind}/main.cpp" -o "${stage}/main"
    "${CXX:-g++}" -std=c++17 -pthread -O3 -march=native -mtune=native -Wall -Wextra -pedantic \
        "${script_dir}/src/control/main.cpp" -o "${stage}/ctl"
    for path in "${stage}/main" "${stage}/ctl"; do
        [[ -f "$path" && -s "$path" ]] || local_die "Compiler did not produce a binary: $path"
        [[ -x "$path" ]] || local_die "Compiled binary is not executable: $path (check permissions and noexec on TUNTOM_BIN_DIR)"
    done
    if [[ "$kind" == switch ]]; then
        rules_file="${stage}/rules"
        if [[ -n "$rules_source" ]]; then cp -- "$rules_source" "$rules_file"; else : > "$rules_file"; fi
        local_hook pre up
        local_read_rules
        local_check_switch
    fi

    echo "[2] Stop previous instance"
    local_down
    # Never unlink a live listener, a symlink, or an unrelated regular file.
    local_remove_socket "$control_socket"
    if [[ "$kind" == switch ]]; then
        local_remove_socket "$switch_socket"
    elif ip link show dev "$name" >/dev/null 2>&1; then
        local_die "Interface $name already exists; refusing to take it over"
    fi
    echo "[3] Install staged binaries"
    mv -f -- "${stage}/main" "$binary"
    mv -f -- "${stage}/ctl" "$control_binary"
    if [[ "$kind" == switch ]]; then mv -f -- "$rules_file" "${instance_dir}/rules"; fi
    rules_file="${instance_dir}/rules"
    # Metadata stays on the runtime filesystem; replace it atomically there.
    printf '%s\0' "$switch_socket" "$control_socket" "$port_id" "$mtu" "$socket_owner" \
        "$pre_hook" "$post_hook" > "${state_file}.new"
    mv -f -- "${state_file}.new" "$state_file"
    starting=1
    local -a command=("$binary")
    if [[ "$kind" == switch ]]; then
        command+=(--socket "$switch_socket")
    else
        command+=("$name" --switch-socket "$switch_socket" --switch-port-id "$port_id" --mtu "$mtu")
    fi
    command+=(--control-socket "$control_socket" "${service_args[@]}")
    echo "[4] Start and check $kind"
    nohup "${command[@]}" > "$log_file" 2>&1 </dev/null 9>&- &
    local pid=$! identity
    printf '%s\n' "$pid" > "$pid_file"
    identity="$(local_identity "$pid")" || local_die "Process exited during startup"
    local_wait_ready "$pid" "$identity" "$control_binary" "$control_socket" "$kind" ||
        local_die "No ready control socket"
    chown "$socket_owner" "$control_socket"
    chmod 0660 "$control_socket"
    if [[ "$kind" == switch ]]; then
        chown "$socket_owner" "$switch_socket"
        chmod 0660 "$switch_socket"
    else
        ip link show dev "$name" >/dev/null || local_die "TUN interface was not created"
        local_hook pre up
    fi
    local_hook post up
    [[ "$(local_identity "$pid")" == "$identity" ]] || local_die "Process exited during hooks"
    starting=0
    echo "$kind $name ready (PID $pid)"
    echo "  log: $log_file"
    echo "  stats: $control_binary $control_socket show stats"
}
