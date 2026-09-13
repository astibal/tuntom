#!/usr/bin/env bash
# Sourced by mk_tunnel.sh. Configuration snapshots contain no shared secret.

group_config_names=(member_count no_address tuntom_snat tuntom_mss_clamp mtu
    transport_mtu stats_format crypto_option stats_option tuntom_prefix16
    client_switch_socket client_switch_port_id client_switch_label client_switch_exit_node
    server_switch_socket server_switch_port_id server_switch_label server_switch_exit_node
    client_has_tun server_has_tun switch_enabled mark_override mask_override
    table_override chain_base remote group_owner)

write_group_config() {
    local name
    for name in "${group_config_names[@]}"; do
        printf '%s=%q\n' "$name" "${!name}"
    done
    printf 'client_ipc_args=(%s)\n' "$(shell_join "${client_ipc_args[@]}")"
    printf 'server_ipc_args=(%s)\n' "$(shell_join "${server_ipc_args[@]}")"
}

select_member() {
    member_index="$1"
    instance="$id"
    local suffix="" host4 host6
    if (( member_index )); then suffix="_${member_index}"; instance+="$suffix"; fi
    instance_key=$((id + 256 * member_index))
    client_if="ut${instance}c"
    server_if="ut${instance}s"
    client_ip="" server_ip="" client_ipv6="" server_ipv6=""
    if (( ! no_address )); then
        host4=$((4 * member_index + 1))
        client_ip="${tuntom_prefix16}.${id}.${host4}"
        server_ip="${tuntom_prefix16}.${id}.$((host4 + 1))"
        printf -v host6 '%x' "$host4"
        client_ipv6="fd42::${tuntom_prefix16//./:}:${id}:${host6}"
        printf -v host6 '%x' "$((host4 + 1))"
        server_ipv6="fd42::${tuntom_prefix16//./:}:${id}:${host6}"
    fi
    udp_port=$((40000 + instance_key))
    tuntom_mark_mask="${mask_override:-0xffff0000}"
    tuntom_mark="${mark_override:-$((instance_key << 16))}"
    tuntom_table="${table_override:-$((10000 + instance_key))}"
    tuntom_chain="${chain_base}${suffix}"
    local_bin="/tmp/tuntom_${instance}c"
    remote_bin="/tmp/tuntom_${instance}s"
    local_log="/tmp/tuntom_${instance}c.log"
    remote_log="/tmp/tuntom_${instance}s.log"
    local_pid_file="${run_dir}/${instance}c.pid"
    remote_pid_file="${run_dir}/${instance}s.pid"
    local_stats_file="${run_dir}/${instance}c.stats"
    remote_stats_file="${run_dir}/${instance}s.stats"
    local_control_file="${run_dir}/${instance}c.control"
    remote_control_file="${run_dir}/${instance}s.control"
    client_switch_args=("${client_ipc_args[@]}")
    server_switch_args=("${server_ipc_args[@]}")
    member_client_port="" member_server_port=""
    if [[ -n "$client_switch_socket" ]]; then
        member_client_port="${client_switch_port_id}${suffix}"
        client_switch_args+=(--switch-socket "$client_switch_socket"
            --switch-port-id "$member_client_port" --switch-label "$client_switch_label")
        if (( client_switch_exit_node )); then client_switch_args+=(--switch-exit-node); fi
    fi
    if [[ -n "$server_switch_socket" ]]; then
        member_server_port="${server_switch_port_id}${suffix}"
        server_switch_args+=(--switch-socket "$server_switch_socket"
            --switch-port-id "$member_server_port" --switch-label "$server_switch_label")
        if (( server_switch_exit_node )); then server_switch_args+=(--switch-exit-node); fi
    fi
    client_switch_options="$(shell_join "${client_switch_args[@]}")"
    server_switch_options="$(shell_join "${server_switch_args[@]}")"
}

set_group_members() {
    local index
    group_members="$id"
    for ((index=1; index<member_count; ++index)); do group_members+=" ${id}_${index}"; done
}

write_group_manifest() {
    printf 'instance\tindex\tkey\tudp_port\tclient_if\tserver_if\tclient_ipv4\tserver_ipv4\tclient_ipv6\tserver_ipv6\tmark\tmask\ttable\tchain\tclient_port\tserver_port\tclient_label\tserver_label\tclient_socket\tserver_socket\tclient_has_tun\tserver_has_tun\tno_address\n'
    local index
    for ((index=0; index<member_count; ++index)); do
        select_member "$index"
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$instance" "$member_index" "$instance_key" "$udp_port" "$client_if" "$server_if" \
            "$client_ip" "$server_ip" "$client_ipv6" "$server_ipv6" "$tuntom_mark" \
            "$tuntom_mark_mask" "$tuntom_table" "$tuntom_chain" "$member_client_port" \
            "$member_server_port" "$client_switch_label" "$server_switch_label" \
            "$client_switch_socket" "$server_switch_socket" "$client_has_tun" "$server_has_tun" "$no_address"
    done
}

validate_group() {
    local socket
    for socket in "$client_switch_socket" "$server_switch_socket"; do
        if [[ "$socket" == *$'\t'* || "$socket" == *$'\n'* || "$socket" == *$'\r'* ]]; then
            echo "Switch socket paths must not contain tabs or newlines" >&2; return 1
        fi
    done
    if (( member_count > 1 )) && [[ -n "$mark_override$mask_override$table_override" ]]; then
        echo "--count > 1 requires automatic TUNTOM_MARK, TUNTOM_MARK_MASK and TUNTOM_TABLE" >&2
        return 1
    fi
    if ! [[ "$chain_base" =~ ^[A-Za-z0-9_]{1,20}$ ]]; then
        echo "TUNTOM_CHAIN must contain 1..20 letters, digits or underscores" >&2
        return 1
    fi
    local index
    for ((index=0; index<member_count; ++index)); do
        select_member "$index"
        if (( ${#member_client_port} > 63 || ${#member_server_port} > 63 )); then
            echo "Switch port ID with member suffix exceeds 63 characters: $instance" >&2
            return 1
        fi
    done
    select_member 0
}

load_group_state() {
    local config
    config="$("${root_cmd[@]}" cat "$local_state/active/config")"
    # This generated file lives under a root-owned 0700 directory. Never load
    # a remote config as shell code on the caller.
    source /dev/stdin <<< "$config"
    tuntom_pre_hook="$local_state/active/pre.sh"
    tuntom_post_hook="$local_state/active/post.sh"
    group_pre_hook="$local_state/active/group-pre.sh"
    group_post_hook="$local_state/active/group-post.sh"
    local_manifest="$local_state/active/manifest.tsv"
    remote_manifest="$remote_state/active/manifest.tsv"
    set_group_members
    select_member 0
}

run_group_hooks() {
    local phase="$1" action="$2" hook="$group_pre_hook" rc=0
    if [[ "$phase" == post ]]; then hook="$group_post_hook"; fi
    local hook_scope=group instance="" instance_key="" member_index=""
    local client_ip="" server_ip="" client_ipv6="" server_ipv6="" udp_port=""
    local tuntom_mark="" tuntom_mark_mask="" tuntom_table="" tuntom_chain=""
    if [[ "$action" == down ]]; then
        run_hook_local "$hook" "$phase" "$action" local "" "" "" || rc=1
        run_hook_remote "$hook" "$phase" "$action" remote "" "" "" || rc=1
    else
        run_hook_local "$hook" "$phase" "$action" local "" "" ""
        run_hook_remote "$hook" "$phase" "$action" remote "" "" ""
    fi
    return "$rc"
}

stop_group() {
    local limit="${1:-$member_count}" index rc=0
    run_group_hooks pre down || rc=1
    for ((index=limit-1; index>=0; --index)); do
        select_member "$index"
        stop_member || rc=1
    done
    run_group_hooks post down || rc=1
    return "$rc"
}

stop_legacy_member() {
    member_count=1
    mask_override="${TUNTOM_MARK_MASK:-0x00ff0000}"
    set_group_members
    select_member 0
    # An old TUN may need cleanup even when the new endpoint is a switch port.
    if "${root_cmd[@]}" ip link show "$client_if" >/dev/null 2>&1; then client_has_tun=1; fi
    local remote_tun
    remote_tun="$(ssh "$remote" "if ip link show '$server_if' >/dev/null 2>&1; then printf yes; fi")"
    if [[ "$remote_tun" == yes ]]; then server_has_tun=1; fi
    stop_member
    # A crashed legacy process can leave policy rules after its TUN disappears.
    if (( ! client_has_tun )); then net_down_local; fi
    if (( ! server_has_tun )); then net_down_remote; fi
}

prepare_group_snapshot() {
    mkdir -p "$group_work/new"
    write_group_config > "$group_work/new/config"
    write_group_manifest > "$group_work/new/manifest.tsv"
    printf '%s\n' "$group_owner" > "$group_work/new/owner"
    printf '%s\n' "$remote" > "$group_work/new/remote"
    printf '%s\n' "$member_count" > "$group_work/new/count"
    local source target
    for target in pre post group-pre group-post; do
        case "$target" in
            pre) source="$tuntom_pre_hook";; post) source="$tuntom_post_hook";;
            group-pre) source="$group_pre_hook";; group-post) source="$group_post_hook";;
        esac
        if "${root_cmd[@]}" test -f "$source"; then
            "${root_cmd[@]}" cat "$source" > "$group_work/new/$target.sh"
        fi
    done
    select_member 0
}

stage_group_snapshot() {
    local command='set -e; umask 077; rm -rf -- "$1/pending"; mkdir "$1/pending"; tar --no-same-owner -xf - -C "$1/pending"'
    tar -C "$group_work/new" -cf - . | "${root_cmd[@]}" bash -c "$command" -- "$local_state"
    tar -C "$group_work/new" -cf - . | ssh "$remote" \
        "bash -c $(shell_join "$command") -- '$remote_state'"
}

activate_group_snapshot() {
    local command='set -e; rm -rf -- "$1/previous"; if [ -d "$1/active" ]; then mv "$1/active" "$1/previous"; fi; mv "$1/pending" "$1/active"'
    "${root_cmd[@]}" bash -c "$command" -- "$local_state"
    ssh "$remote" "bash -c $(shell_join "$command") -- '$remote_state'"
    load_group_state
}

clear_group_state() {
    # Retain owner/config until teardown succeeded on BOTH hosts.
    ssh "$remote" "rm -rf -- '$remote_state/active' '$remote_state/previous' '$remote_state/pending'"
    "${root_cmd[@]}" rm -rf -- "$local_state/active" "$local_state/previous" "$local_state/pending"
}

prepare_group_directories() {
    local command='set -e; umask 077; for p in /var/lib/tuntom-mk /var/lib/tuntom-mk/"$1"; do
        if [ -L "$p" ]; then echo "Unsafe state directory: $p" >&2; exit 1; fi
        mkdir -p "$p"; chown root:root "$p"; chmod 700 "$p"
    done
    if [ -L "$2" ]; then echo "Unsafe group directory" >&2; exit 1; fi
    mkdir -p "$2"; chown root:root "$2"; chmod 700 "$2"'
    "${root_cmd[@]}" bash -c "$command" -- client "$local_state"
    ssh "$remote" "bash -c $(shell_join "$command") -- server '$remote_state'"
}

check_group_ownership() {
    old_count=1
    have_old_state=0
    local saved_remote="" remote_owner=""
    if "${root_cmd[@]}" test -f "$local_state/active/owner"; then
        have_old_state=1
        group_owner="$("${root_cmd[@]}" cat "$local_state/active/owner")"
        saved_remote="$("${root_cmd[@]}" cat "$local_state/active/remote")"
        old_count="$("${root_cmd[@]}" cat "$local_state/active/count")"
        if [[ "$saved_remote" != "$remote" ]]; then
            echo "Group $id belongs to $saved_remote; stop it there before changing the remote" >&2
            return 1
        fi
    else
        IFS= read -r group_owner < /proc/sys/kernel/random/uuid
    fi
    remote_owner="$(ssh "$remote" "if [ -f '$remote_state/active/owner' ]; then cat '$remote_state/active/owner'; fi")"
    if [[ -n "$remote_owner" && "$remote_owner" != "$group_owner" ]]; then
        echo "Remote group $id belongs to another caller; refusing to replace it" >&2
        return 1
    fi
    if ! [[ "$old_count" =~ ^([1-9]|[1-5][0-9]|6[0-4])$ ]]; then
        echo "Invalid saved group member count" >&2; return 1
    fi
    if (( ! count_requested )); then member_count="$old_count"; fi
}

preflight_new_members() {
    local index routes probe_rc
    # The explicitly named base tunnel can be adopted from the legacy script.
    # Additional members must be free unless they belong to our saved group.
    for ((index=old_count; index<member_count; ++index)); do
        select_member "$index"
        if "${root_cmd[@]}" ip link show "$client_if" >/dev/null 2>&1 ||
           "${root_cmd[@]}" test -e "$local_pid_file" ||
           "${root_cmd[@]}" bash -c "$(declare -f member_process_exists); member_process_exists \"\$@\"" -- \
               "$local_bin" client "$instance" "$client_if"; then
            echo "Local member $instance is already in use" >&2; return 1
        fi
        probe_rc=0
        ssh "$remote" "bash -s -- '$remote_bin' server '$instance' '$server_if'" <<< \
            "$(declare -f member_process_exists); member_process_exists \"\$@\"" || probe_rc=$?
        if (( probe_rc == 0 )); then
            echo "Remote member $instance already has a running process" >&2; return 1
        fi
        if (( probe_rc != 1 )); then
            echo "Cannot inspect remote member $instance" >&2; return 1
        fi
        routes="$("${root_cmd[@]}" ip route show table "$tuntom_table" 2>/dev/null || true)"
        if [[ -n "$routes" ]] || "${root_cmd[@]}" iptables -w 5 -t mangle -nL "${tuntom_chain}_M" >/dev/null 2>&1; then
            echo "Local network resources for $instance are already in use" >&2; return 1
        fi
        ssh "$remote" "
            if ip link show '$server_if' >/dev/null 2>&1 || [ -e '$remote_pid_file' ]; then
                echo 'Remote member $instance is already in use' >&2; exit 1
            fi
            listeners=\$(ss -H -lun 'sport = :$udp_port') || exit 1
            if [ -n \"\$listeners\" ]; then echo 'UDP port $udp_port is already in use' >&2; exit 1; fi
            routes=\$(ip route show table '$tuntom_table' 2>/dev/null || true)
            if [ -n \"\$routes\" ] || iptables -w 5 -t mangle -nL '${tuntom_chain}_M' >/dev/null 2>&1; then
                echo 'Remote network resources for $instance are already in use' >&2; exit 1
            fi
        "
    done
    select_member 0
}

install_member_binary() {
    local command='set -e; staged=$(mktemp "${2}.install.XXXXXXXX"); trap '\''rm -f -- "$staged"'\'' EXIT; cp -- "$1" "$staged"; chmod 700 "$staged"; mv -f -- "$staged" "$2"'
    "${root_cmd[@]}" bash -c "$command" -- "$local_stage" "$local_bin"
    ssh "$remote" "bash -c $(shell_join "$command") -- '$remote_stage' '$remote_bin'"
}

group_exit() {
    local rc=$?
    trap - EXIT
    if (( rollback_active )); then
        echo "Group $id failed; cleaning up $started_count attempted members" >&2
        if ! stop_group "$started_count"; then
            echo "Cleanup incomplete; saved group state retained for --stop" >&2
        fi
        # Keep the manifest even after successful rollback, for an idempotent
        # retry/stop with the same owner and hook context.
    fi
    cleanup_staging
    rm -rf -- "$group_work"
    exit "$rc"
}

group_main() {
    mark_override="${TUNTOM_MARK:-}" mask_override="${TUNTOM_MARK_MASK:-}"
    table_override="${TUNTOM_TABLE:-}" chain_base="${TUNTOM_CHAIN:-TUNTOM_${id}}"
    group_pre_hook="${TUNTOM_GROUP_PRE_HOOK:-}"
    group_post_hook="${TUNTOM_GROUP_POST_HOOK:-}"
    local_state="/var/lib/tuntom-mk/client/$id"
    remote_state="/var/lib/tuntom-mk/server/$id"
    group_work="$(mktemp -d /tmp/tuntom-group.XXXXXXXX)"
    local_stage="$group_work/tuntom"
    local_switch_stage="$group_work/switch"
    local_adapter_stage="$group_work/adapter"
    local_control_stage="$group_work/control"
    remote_stage="$remote_state/tuntom.new"
    remote_switch_stage="$remote_state/switch.new"
    remote_adapter_stage="$remote_state/adapter.new"
    remote_control_stage="$remote_state/control.new"
    rollback_active=0 started_count=0
    trap group_exit EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    trap 'exit 129' HUP
    local remote_lock_command="set -e; umask 077; mkdir -p /run/tuntom-mk; exec 9>/run/tuntom-mk/server_${id}.lock; flock -n 9 || exit 75; printf 'LOCKED\\n'; cat >/dev/null"
    acquire_mk_lock "$remote_lock_command"
    prepare_group_directories
    check_group_ownership

    if (( stop_requested )); then
        if (( have_old_state )); then
            load_group_state
            stop_group
        else
            stop_legacy_member
        fi
        clear_group_state
        echo "Tunnel group $id stopped"
        return
    fi

    validate_group
    set_group_members
    preflight_new_members
    prepare_group_snapshot
    echo "Tunnel group $id ($member_count members): $group_members"
    build_staging
    prepare_runtime
    stage_group_snapshot

    # Teardown uses its original snapshot, even when count/options/hooks change.
    (
        if (( have_old_state )); then
            load_group_state
            stop_group
        else
            stop_legacy_member
        fi
    )
    activate_group_snapshot
    rollback_active=1
    install_companion_tools
    run_group_hooks pre up
    local index
    for ((index=0; index<member_count; ++index)); do
        select_member "$index"
        started_count=$((index + 1))
        show_member
        install_member_binary
        start_member
        test_member
    done
    for ((index=0; index<member_count; ++index)); do
        select_member "$index"
        check_member_processes
    done
    run_group_hooks post up
    rollback_active=0
    echo "Tunnel group $id is UP ($member_count members)"
}
