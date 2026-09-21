-- tuntom.lua
--
-- Wireshark dissector for the tuntom TUN-over-UDP protocol.
--
-- Protocol v1:
--   0..3   magic            "UTUN"
--   4..5   tunnel_id        uint16 BE
--   6      version          1
--   7      type
--   8..    payload
--
-- Protocol v2:
--   0..3   magic            "UTUN"
--   4..5   tunnel_id        uint16 BE
--   6      version          2
--   7      type
--   8..15  sequence         uint64 BE
--   16..31 auth_tag         16 bytes
--   32..   payload
--
-- Protocol v3/v4 (same layout; v4 uses directional keys):
--   0..3   magic            "UTUN"
--   4..5   tunnel_id        uint16 BE
--   6      version          3 or 4
--   7      type
--   8..15  sequence         uint64 BE
--   16..23 message_id       uint64 BE
--   24..27 fragment_offset  uint32 BE
--   28..31 original_length  uint32 BE
--   32..47 auth_tag         16 bytes
--   48..   payload
--
-- Packet types:
--   1 = HELLO
--   2 = KEEPALIVE
--   3 = DATA
--   4 = PING
--   5 = PONG
--   6 = MTU_PROBE
--   7 = MTU_REPLY
--   14 = CONTROL (V5; version/kind/state, request ID, offset/total, command, body block)
--
-- V3 PING/PONG use message_id as the probe_id. fragment_offset and
-- original_length remain zero and there is no payload.
--
-- V3 MTU_PROBE/MTU_REPLY use message_id as the PMTUD probe id and
-- original_length as the target/observed outer MTU. MTU_PROBE carries
-- padding so that the complete outer IP packet reaches original_length;
-- MTU_REPLY has no payload.
--
-- V3 DATA fragments are reassembled in Lua. The reassembly key includes
-- tunnel id, message id and packet direction. Once all byte ranges are
-- available, a synthetic Tvb is created from the original IP packet and
-- handed to Wireshark's IPv4/IPv6 dissector.
--
-- Reassembly happens during Wireshark's first sequential dissection pass.
-- On later passes/clicks, cached completion metadata and reassembled bytes
-- are reused. proto.init() clears all state when the capture is reloaded.
--
-- V4 handshake: INIT(8), RESPONSE(9), CONFIRM(10), CONFIRM_ACK(11).
-- SEQ splits into a 16-bit session hint and a 48-bit counter.
-- Suites 0/1 have absent DH; suite 2 uses X25519 + AKDF + Ascon-AEAD128.
-- This dissector does not verify the Ascon authentication tag.
--
-- By default it registers for UDP ports 40001..40255, matching:
--
--     udp_port = 40000 + tunnel_id
--

local tuntom = Proto("tuntom", "tuntom TUN-over-UDP")

local packet_type_names = {
    [1] = "HELLO",
    [2] = "KEEPALIVE",
    [3] = "DATA",
    [4] = "PING",
    [5] = "PONG",
    [6] = "MTU_PROBE",
    [7] = "MTU_REPLY",
    [8] = "INIT",
    [9] = "RESPONSE",
    [10] = "CONFIRM",
    [11] = "CONFIRM_ACK",
    [12] = "IPC",
    [13] = "INFO",
    [14] = "CONTROL",
}

local f_magic = ProtoField.string(
    "tuntom.magic",
    "Magic"
)

local f_tunnel_id = ProtoField.uint16(
    "tuntom.tunnel_id",
    "Tunnel ID",
    base.DEC
)

local f_version = ProtoField.uint8(
    "tuntom.version",
    "Version",
    base.DEC
)

local f_type = ProtoField.uint8(
    "tuntom.type",
    "Type",
    base.DEC,
    packet_type_names
)

local f_sequence = ProtoField.uint64(
    "tuntom.sequence",
    "Sequence",
    base.DEC
)

local f_message_id = ProtoField.uint64(
    "tuntom.message_id",
    "Message ID",
    base.DEC
)

local f_probe_id = ProtoField.uint64(
    "tuntom.probe_id",
    "Probe ID",
    base.DEC
)

local f_mtu_probe_id = ProtoField.uint64(
    "tuntom.mtu_probe_id",
    "PMTUD Probe ID",
    base.DEC
)

local f_mtu = ProtoField.uint32(
    "tuntom.mtu",
    "Outer MTU",
    base.DEC
)

local f_mtu_padding = ProtoField.bytes(
    "tuntom.mtu_padding",
    "PMTUD Padding"
)

local f_fragment_offset = ProtoField.uint32(
    "tuntom.fragment_offset",
    "Fragment Offset",
    base.DEC
)

local f_original_length = ProtoField.uint32(
    "tuntom.original_length",
    "Original Length",
    base.DEC
)

local f_fragment_length = ProtoField.uint32(
    "tuntom.fragment_length",
    "Fragment Length",
    base.DEC
)

local f_fragment_end = ProtoField.uint32(
    "tuntom.fragment_end",
    "Fragment End",
    base.DEC
)

local f_fragmented = ProtoField.bool(
    "tuntom.fragmented",
    "Fragmented"
)

local f_reassembled = ProtoField.bool(
    "tuntom.reassembled",
    "Reassembled"
)

local f_reassembled_length = ProtoField.uint32(
    "tuntom.reassembled_length",
    "Reassembled Length",
    base.DEC
)

local f_reassembled_in = ProtoField.framenum(
    "tuntom.reassembled_in",
    "Reassembled In",
    base.NONE
)

local f_fragment_of = ProtoField.framenum(
    "tuntom.fragment_of",
    "Fragment Of",
    base.NONE
)

local f_auth_tag = ProtoField.bytes(
    "tuntom.auth_tag",
    "Auth Tag"
)

local f_payload = ProtoField.bytes(
    "tuntom.payload",
    "Payload"
)

local f_info_entry = ProtoField.string("tuntom.info.entry", "INFO Field")
local f_info_key = ProtoField.string("tuntom.info.key", "INFO Key")
local f_info_value = ProtoField.string("tuntom.info.value", "INFO Value")
local e_info = ProtoExpert.new("tuntom.info.malformed", "Malformed INFO",
    expert.group.MALFORMED, expert.severity.ERROR)

-- CONTROL v1 envelope; this is separate from DATA fragmentation.
local control_kinds = {[1]="PUT", [2]="STATUS", [3]="REPLY", [4]="FINISH", [5]="CONFIRMED", [6]="DISCOVER", [7]="FOUND", [8]="ALT_PATH", [9]="ROUTE_ERROR"}
local control_states = {[1]="RECEIVING", [2]="READY", [3]="RUNNING", [4]="SUCCEEDED",
    [5]="FAILED", [6]="REJECTED", [7]="EXPIRED", [8]="NOT_FOUND"}
local f_control_version = ProtoField.uint8("tuntom.control.version", "CONTROL Version", base.DEC)
local f_control_kind = ProtoField.uint8("tuntom.control.kind", "CONTROL Kind", base.DEC, control_kinds)
local f_control_state = ProtoField.uint8("tuntom.control.state", "Request State", base.DEC, control_states)
local f_control_origin = ProtoField.bytes("tuntom.control.origin", "Origin Instance ID")
local f_control_hops = ProtoField.uint8("tuntom.control.hops", "Remaining Hops", base.DEC)
local f_control_destination = ProtoField.bytes("tuntom.control.destination", "Destination Stack")
local f_control_reply_path = ProtoField.bytes("tuntom.control.reply_path", "Reply Stack")
local f_control_hop_type = ProtoField.uint8("tuntom.control.hop.type", "Hop Type", base.DEC, {[1]="PEER",[2]="PORT",[3]="LINK"})
local f_control_hop_value = ProtoField.bytes("tuntom.control.hop.value", "Hop Value")
local f_control_id = ProtoField.bytes("tuntom.control.request_id", "Request ID")
local f_control_offset = ProtoField.uint32("tuntom.control.offset", "Block / Acknowledged Offset", base.DEC)
local f_control_total = ProtoField.uint32("tuntom.control.total", "Total Body Length", base.DEC)
local f_control_command_length = ProtoField.uint16("tuntom.control.command_length", "Command Length", base.DEC)
local f_control_command = ProtoField.string("tuntom.control.command", "Command")
local f_control_data = ProtoField.bytes("tuntom.control.data", "Body Block")
local e_control = ProtoExpert.new("tuntom.control.malformed", "Malformed CONTROL",
    expert.group.MALFORMED, expert.severity.ERROR)

local f_session_hint = ProtoField.uint16("tuntom.session_hint", "Session Hint", base.HEX)
local f_counter = ProtoField.uint64("tuntom.counter", "Session Packet Counter", base.DEC)
local f_init_timestamp = ProtoField.uint64("tuntom.init_timestamp", "INIT Unix Timestamp (seconds)", base.DEC)
local f_nonce = ProtoField.bytes("tuntom.nonce", "Handshake Nonce")
local f_init_hash = ProtoField.bytes("tuntom.init_hash", "INIT Binding (keyed AMAC commitment)")
local f_suite = ProtoField.uint16("tuntom.suite", "Suite", base.DEC, {[0] = "AMAC, no encryption", [1] = "Ascon-AEAD128", [2] = "X25519 + AKDF + Ascon-AEAD128"})
local f_dh_length = ProtoField.uint16("tuntom.dh_length", "DH Public Key Length", base.DEC)
local f_dh = ProtoField.bytes("tuntom.dh", "DH Public Key")
local e_handshake = ProtoExpert.new("tuntom.handshake_error", "Invalid/unsupported handshake",
    expert.group.MALFORMED, expert.severity.ERROR)
tuntom.experts = {e_handshake, e_info, e_control}

tuntom.fields = {
    f_control_origin, f_control_hops, f_control_destination, f_control_reply_path, f_control_hop_type, f_control_hop_value,
    f_control_version, f_control_kind, f_control_state, f_control_id,
    f_control_offset, f_control_total, f_control_command_length, f_control_command, f_control_data,
    f_info_entry, f_info_key, f_info_value,
    f_session_hint, f_counter, f_init_timestamp, f_nonce, f_init_hash, f_suite, f_dh_length, f_dh,
    f_magic,
    f_tunnel_id,
    f_version,
    f_type,
    f_sequence,
    f_message_id,
    f_probe_id,
    f_mtu_probe_id,
    f_mtu,
    f_mtu_padding,
    f_fragment_offset,
    f_original_length,
    f_fragment_length,
    f_fragment_end,
    f_fragmented,
    f_reassembled,
    f_reassembled_length,
    f_reassembled_in,
    f_fragment_of,
    f_auth_tag,
    f_payload,
}

local ip_dissector = Dissector.get("ip")
local ipv6_dissector = Dissector.get("ipv6")

--
-- Reassembly state.
--
-- We intentionally store raw Lua strings rather than Tvb/TvbRange objects:
-- Tvb objects are valid only during the current dissector invocation.
--
local reassembly = {}
local frame_info = {}
local reassembly_entry_count = 0

local max_reassembly_entries = 512
local max_original_length = 65535
local max_fragments_per_message = 64

function tuntom.init()
    reassembly = {}
    frame_info = {}
    reassembly_entry_count = 0
end

local function dissect_inner_ip(tvb, pinfo, tree)
    if tvb:len() == 0 then
        return
    end

    local first_byte = tvb(0, 1):uint()
    local ip_version = math.floor(first_byte / 16)

    if ip_version == 4 and ip_dissector ~= nil then
        ip_dissector:call(tvb, pinfo, tree)
    elseif ip_version == 6 and ipv6_dissector ~= nil then
        ipv6_dissector:call(tvb, pinfo, tree)
    else
        tree:add(f_payload, tvb())
    end
end

local function dissect_inner_ip_with_tuntom_info(
    tvb,
    pinfo,
    tree,
    info_text
)
    dissect_inner_ip(tvb, pinfo, tree)

    local inner_info =
        tostring(pinfo.cols.info)

    -- Keep the native IP/ICMP/TCP/UDP packet-list presentation and append
    -- tuntom encapsulation metadata without replacing the useful inner title.
    if inner_info ~= "" then
        pinfo.cols.info = string.format(
            "%s [TUNTOM: %s]",
            inner_info,
            info_text
        )
    else
        pinfo.cols.info = string.format(
            "[TUNTOM: %s]",
            info_text
        )
    end
end

local function direction_key(pinfo)
    return string.format(
        "%s:%s>%s:%s",
        tostring(pinfo.src),
        tostring(pinfo.src_port),
        tostring(pinfo.dst),
        tostring(pinfo.dst_port)
    )
end

local function message_key(
    pinfo,
    tunnel_id,
    message_id,
    version,
    hint
)
    -- Hints are not unique/authenticated here: collisions cannot be resolved
    -- without session keys. This is best-effort capture reassembly only.
    return string.format(
        "%s|tun=%u|msg=%s|v=%u|hint=%u",
        direction_key(pinfo),
        tunnel_id,
        tostring(message_id), version, hint
    )
end

local function count_fragments(entry)
    local count = 0
    for _ in pairs(entry.fragments) do
        count = count + 1
    end
    return count
end

local function ranges_overlap(
    begin_a,
    end_a,
    begin_b,
    end_b
)
    return begin_a < end_b and end_a > begin_b
end

local function remove_reassembly_entry(key)
    if reassembly[key] ~= nil then
        reassembly[key] = nil
        reassembly_entry_count =
            math.max(0, reassembly_entry_count - 1)
    end
end

local function make_entry(
    key,
    original_length,
    first_frame
)
    if reassembly_entry_count >= max_reassembly_entries then
        return nil
    end

    local entry = {
        key = key,
        original_length = original_length,
        fragments = {},
        frames = {},
        first_frame = first_frame,
        completion_frame = nil,
        complete_raw = nil,
    }

    reassembly[key] = entry
    reassembly_entry_count =
        reassembly_entry_count + 1

    return entry
end

local function fragment_is_complete_packet(
    fragment_offset,
    payload_length,
    original_length
)
    return
        fragment_offset == 0 and
        payload_length == original_length
end

local function try_complete_entry(entry)
    local offsets = {}

    for offset, _ in pairs(entry.fragments) do
        table.insert(offsets, offset)
    end

    table.sort(offsets)

    local expected_offset = 0
    local parts = {}

    for _, offset in ipairs(offsets) do
        local fragment = entry.fragments[offset]

        if offset ~= expected_offset then
            return nil
        end

        table.insert(parts, fragment.raw)
        expected_offset =
            expected_offset + fragment.length
    end

    if expected_offset ~= entry.original_length then
        return nil
    end

    return table.concat(parts)
end

local function remember_completion(
    entry,
    completion_frame,
    raw
)
    entry.completion_frame = completion_frame
    entry.complete_raw = raw

    for _, frame_number in ipairs(entry.frames) do
        frame_info[frame_number] =
            frame_info[frame_number] or {}

        frame_info[frame_number].reassembled_in =
            completion_frame

        frame_info[frame_number].first_frame =
            entry.first_frame
    end

    frame_info[completion_frame] =
        frame_info[completion_frame] or {}

    frame_info[completion_frame].reassembled_in =
        completion_frame

    frame_info[completion_frame].first_frame =
        entry.first_frame

    frame_info[completion_frame].complete_raw =
        raw

    frame_info[completion_frame].original_length =
        entry.original_length
end

local function accept_fragment_first_pass(
    pinfo,
    key,
    fragment_offset,
    original_length,
    payload,
    limit
)
    if original_length == 0 or
       original_length > (limit or max_original_length) then
        return nil, "invalid original length"
    end

    local payload_length = payload:len()

    if payload_length == 0 then
        return nil, "empty data fragment"
    end

    local fragment_end =
        fragment_offset + payload_length

    if fragment_end > original_length then
        return nil, "fragment exceeds original length"
    end

    local entry = reassembly[key]

    if entry == nil then
        entry = make_entry(
            key,
            original_length,
            pinfo.number
        )

        if entry == nil then
            return nil, "reassembly table full"
        end
    elseif entry.original_length ~= original_length then
        remove_reassembly_entry(key)
        return nil, "original length changed"
    end

    if count_fragments(entry) >=
       (limit == 65639 and 256 or max_fragments_per_message) then
        remove_reassembly_entry(key)
        return nil, "too many fragments"
    end

    for existing_offset, existing in
        pairs(entry.fragments) do

        local existing_end =
            existing_offset + existing.length

        if ranges_overlap(
            fragment_offset,
            fragment_end,
            existing_offset,
            existing_end
        ) then
            --
            -- Exact duplicate: quietly ignore it for reassembly.
            -- A partial overlap is malformed for tuntom V3.
            --
            if fragment_offset == existing_offset and
               payload_length == existing.length and
               payload:raw() == existing.raw then
                return nil, "duplicate fragment"
            end

            return nil, "overlapping fragment"
        end
    end

    entry.fragments[fragment_offset] = {
        raw = payload:raw(),
        length = payload_length,
        frame = pinfo.number,
    }

    table.insert(
        entry.frames,
        pinfo.number
    )

    frame_info[pinfo.number] =
        frame_info[pinfo.number] or {}

    frame_info[pinfo.number].first_frame =
        entry.first_frame

    local complete_raw =
        try_complete_entry(entry)

    if complete_raw ~= nil then
        remember_completion(
            entry,
            pinfo.number,
            complete_raw
        )

        -- Redissection uses frame_info, which now holds the completed
        -- packet and fragment links. Keep this table for incomplete
        -- messages only so completed traffic cannot exhaust its limit.
        remove_reassembly_entry(key)

        return complete_raw, nil
    end

    return nil, nil
end

local function add_generated_field(
    tree,
    field,
    value
)
    local item = tree:add(field, value)
    item:set_generated()
    return item
end

local function add_reassembly_links(
    subtree,
    pinfo
)
    local info = frame_info[pinfo.number]

    if info == nil then
        return
    end

    if info.first_frame ~= nil and
       info.first_frame ~= pinfo.number then

        add_generated_field(
            subtree,
            f_fragment_of,
            info.first_frame
        )
    end

    if info.reassembled_in ~= nil then
        add_generated_field(
            subtree,
            f_reassembled_in,
            info.reassembled_in
        )
    end
end

local function add_v3_fragment_fields(
    subtree,
    buffer,
    payload_length
)
    local fragment_offset =
        buffer(24, 4):uint()

    local original_length =
        buffer(28, 4):uint()

    local fragment_end =
        fragment_offset + payload_length

    local fragmented =
        fragment_offset ~= 0 or
        payload_length ~= original_length

    subtree:add(
        f_message_id,
        buffer(16, 8)
    )

    subtree:add(
        f_fragment_offset,
        buffer(24, 4)
    )

    subtree:add(
        f_original_length,
        buffer(28, 4)
    )

    add_generated_field(
        subtree,
        f_fragment_length,
        payload_length
    )

    add_generated_field(
        subtree,
        f_fragment_end,
        fragment_end
    )

    add_generated_field(
        subtree,
        f_fragmented,
        fragmented
    )

    return
        fragment_offset,
        original_length,
        fragment_end,
        fragmented
end

local function dissect_reassembled_raw(
    raw,
    pinfo,
    subtree,
    original_length,
    info_text
)
    local bytes =
        ByteArray.new(raw, true)

    local reassembled_tvb =
        bytes:tvb(
            string.format(
                "tuntom reassembled IP packet (%u bytes)",
                original_length
            )
        )

    local reassembled_tree =
        subtree:add(
            tuntom,
            reassembled_tvb(),
            string.format(
                "Reassembled IP packet (%u bytes)",
                original_length
            )
        )

    add_generated_field(
        reassembled_tree,
        f_reassembled,
        true
    )

    add_generated_field(
        reassembled_tree,
        f_reassembled_length,
        original_length
    )

    dissect_inner_ip_with_tuntom_info(
        reassembled_tvb,
        pinfo,
        reassembled_tree,
        info_text
    )
end

-- Canonical IPC relay records. No session keys are exported by tuntom; encrypted
-- tunnel payloads stay opaque. USER0 captures can contain decoded TTR records or
-- ordinary switch IPC frames directly.
local ipc_proto = Proto("tuntom_ipc", "tuntom IPC")
local ipc_fields = {
    type = ProtoField.uint8("tuntom.ipc.type", "Relay type", base.DEC, {[1]="RESET",[2]="SNAPSHOT",[3]="ACK",[4]="DATA"}),
    channel = ProtoField.uint32("tuntom.ipc.channel", "Channel / snapshot revision", base.DEC),
    epoch = ProtoField.uint64("tuntom.ipc.epoch", "Relay epoch", base.HEX),
    owner = ProtoField.uint64("tuntom.ipc.owner", "Owner", base.HEX),
    name = ProtoField.string("tuntom.ipc.name", "Port identity"),
    count = ProtoField.uint16("tuntom.ipc.channels", "Channel count", base.DEC),
    opcode = ProtoField.uint8("tuntom.ipc.opcode", "Opcode", base.DEC, {[1]="SWITCH",[2]="EXIT"}),
    labels = ProtoField.uint8("tuntom.ipc.label_count", "Label count", base.DEC),
    label = ProtoField.uint64("tuntom.ipc.label", "Label", base.HEX),
    length = ProtoField.uint32("tuntom.ipc.length", "Record length", base.DEC),
    cookie = ProtoField.string("tuntom.ipc.via.cookie", "VIA cookie"),
    chain = ProtoField.uint32("tuntom.ipc.via.chain", "Chain ID", base.DEC),
    step = ProtoField.uint16("tuntom.ipc.via.step", "Step", base.DEC),
    saved = ProtoField.uint8("tuntom.ipc.via.saved", "Saved label count", base.DEC),
    action = ProtoField.uint8("tuntom.ipc.via.action", "Action", base.DEC, {[0]="OFFER",[1]="CONTINUE",[2]="BYPASS",[3]="COMPLETE"}),
    reverse = ProtoField.bool("tuntom.ipc.via.reverse", "Reverse direction"),
    origin = ProtoField.uint64("tuntom.ipc.via.origin", "Origin port ID", base.DEC),
    payload = ProtoField.bytes("tuntom.ipc.payload", "Payload"),
}
local field_list = {}
for _, f in pairs(ipc_fields) do field_list[#field_list+1] = f end
ipc_proto.fields = field_list
local ipc_error = ProtoExpert.new("tuntom.ipc.malformed", "Malformed IPC", expert.group.MALFORMED, expert.severity.ERROR)
ipc_proto.experts = {ipc_error}
local dissect_control
local function dissect_ipc(buffer, pinfo, tree)
    if buffer:len()>0 and buffer(0,1):uint()==2 then
        dissect_control(buffer,0,pinfo,tree:add(tuntom,buffer()))
        return buffer:len()
    end
    local t = tree:add(ipc_proto, buffer())
    local function bad(message) t:add_proto_expert_info(ipc_error, message); return buffer:len() end
    local at, n = 0, buffer:len()
    if n >= 4 and buffer(0,4):string() == "TTR\001" then
        if n < 32 then return bad("Truncated relay header") end
        local kind = buffer(4,1):uint()
        t:add(ipc_fields.type,buffer(4,1)); t:add(ipc_fields.channel,buffer(8,4))
        t:add(ipc_fields.length,buffer(12,4)); t:add(ipc_fields.epoch,buffer(16,8))
        if n > 65639 or buffer(12,4):uint() ~= n or buffer(5,3):uint() ~= 0 or
            buffer(24,8):uint64() ~= UInt64(0,0) or kind < 1 or kind > 4 then return bad("Invalid relay header") end
        if kind == 1 then
            if n ~= 32 or buffer(8,4):uint() ~= 0 or buffer(16,8):uint64() ~= UInt64(0,0) then return bad("Invalid reset") end
            return n
        end
        if buffer(8,4):uint() == 0 or buffer(16,8):uint64() == UInt64(0,0) then return bad("Zero channel/revision or epoch") end
        if kind == 3 then if n ~= 32 then return bad("Invalid ACK length") end; return n end
        if kind == 2 then
            if n < 34 then return bad("Truncated snapshot") end
            local count = buffer(32,2):uint(); t:add(ipc_fields.count,buffer(32,2)); at = 34
            if count > 128 then return bad("Too many channels") end
            local ids, names = {}, {}
            for i=1,count do
                if at+13 > n then return bad("Truncated channel") end
                local length = buffer(at+12,1):uint()
                if length == 0 or length > 63 or at+13+length > n then return bad("Invalid channel name length") end
                local id, name = buffer(at,4):uint(), buffer(at+13,length):string()
                if id == 0 or ids[id] or names[name] or buffer(at+4,8):uint64() == UInt64(0,0) then return bad("Invalid or duplicate channel identity") end
                ids[id], names[name] = true, true
                local c = t:add(buffer(at,13+length), "Channel " .. id .. ": " .. name)
                c:add(ipc_fields.channel,buffer(at,4)); c:add(ipc_fields.owner,buffer(at+4,8)); c:add(ipc_fields.name,buffer(at+13,length))
                at = at+13+length
            end
            if at ~= n then return bad("Trailing snapshot bytes") end
            return n
        end
        at = 32
    end
    if n-at < 8 then return bad("Truncated switch frame") end
    local count, opcode = buffer(at+3,1):uint(), buffer(at+1,1):uint()
    t:add(ipc_fields.opcode,buffer(at+1,1)); t:add(ipc_fields.labels,buffer(at+3,1)); t:add(ipc_fields.length,buffer(at+4,4))
    if buffer(at,1):uint() ~= 1 or (opcode ~= 1 and opcode ~= 2) or buffer(at+2,1):uint() ~= 0 or count < 1 or count > 8 or
        buffer(at+4,4):uint() ~= n-at or n-at <= 8+count*8 or n-at-8-count*8 > 65535 then return bad("Invalid switch frame") end
    at = at+8
    for i=0,count-1 do
        local offset = at+i*8
        local label = t:add(ipc_fields.label,buffer(offset,8))
        if i+2 < count and buffer(offset+3,3):string() == "VIA" and buffer(offset+6,1):uint() == 1 then
            local length = buffer(offset+7,1):uint()
            local saved = buffer(offset+14,1):uint()
            if length == 3+saved and i+length == count then
                local flags = buffer(offset+15,1):uint()
                label:add(ipc_fields.cookie,buffer(offset,3))
                label:add(ipc_fields.chain,buffer(offset+8,4)); label:add(ipc_fields.step,buffer(offset+12,2))
                label:add(ipc_fields.saved,buffer(offset+14,1)); label:add(ipc_fields.action,buffer(offset+15,1),math.floor(flags/2))
                label:add(ipc_fields.reverse,buffer(offset+15,1),flags%2==1); label:add(ipc_fields.origin,buffer(offset+16,8))
            end
        end
    end
    local payload = buffer(at+count*8)
    t:add(ipc_fields.payload,payload)
    if payload:len() >= 20 and (math.floor(payload(0,1):uint()/16) == 4 or math.floor(payload(0,1):uint()/16) == 6) then
        dissect_inner_ip_with_tuntom_info(payload:tvb(),pinfo,t,"tuntom IPC")
    end
    return n
end
ipc_proto.dissector = dissect_ipc
DissectorTable.get("wtap_encap"):add(wtap.USER0, ipc_proto)

-- Validate the entire snapshot before exposing any fields. Authentication is
-- not verified by this dissector; encrypted payloads never enter this parser.
local function dissect_info(buffer, header, tree)
    local length = buffer:len() - header
    local function malformed(reason)
        tree:add_proto_expert_info(e_info, reason)
    end
    if length == 0 or length > 4096 then
        malformed("INFO payload must contain 1..4096 bytes")
        return
    end
    local payload = buffer(header, length)
    tree:add(f_payload, payload)
    local text = payload:raw() -- Preserve NUL bytes for strict validation.
    for i = 1, #text do
        local byte = text:byte(i)
        if (byte < 32 or byte > 126) and byte ~= 9 and byte ~= 10 then
            malformed("INFO contains a forbidden byte (only printable ASCII, TAB and LF allowed)")
            return
        end
    end
    local entries, seen, at = {}, {}, 1
    while at <= #text do
        local newline = text:find("\n", at, true)
        local last = newline and newline - 1 or #text
        local line = text:sub(at, last)
        local equal = line:find("=", 1, true)
        if not equal then malformed("INFO line is empty or missing '='"); return end
        local key = line:sub(1, equal - 1)
        if not key:match("^[a-z][a-z0-9_]*$") then malformed("Invalid INFO key"); return end
        if seen[key] then malformed("Duplicate INFO key: " .. key); return end
        seen[key] = true
        local value = line:sub(equal + 1):gsub("^[ \t]+", ""):gsub("[ \t]+$", "")
        entries[#entries + 1] = {offset = header + at - 1, size = #line, key = key, value = value}
        at = newline and newline + 1 or #text + 1
    end
    for _, entry in ipairs(entries) do
        local item = tree:add(f_info_entry, buffer(entry.offset, entry.size), entry.key .. "=" .. entry.value)
        item:add(f_info_key, buffer(entry.offset, #entry.key), entry.key)
        item:add(f_info_value, buffer(entry.offset + #entry.key + 1, entry.size - #entry.key - 1), entry.value)
    end
end

-- V5: no magic or tunnel ID on wire. Version occurs only in INIT/RESPONSE.
-- Base = type/flags(1), sequence(8), type extension, tag(16), payload.
-- Extensions: fragments(12), ping/confirm(8), PMTUD(10), handshake(9).
dissect_control = function(buffer, header, pinfo, tree)
    local length = buffer:len() - header
    local function bad(reason) tree:add_proto_expert_info(e_control, reason) end
    if length < 32 then bad("Truncated CONTROL header (expected 32 bytes)"); return end
    local payload = buffer(header):tvb()
    local version, kind, state = payload(0,1):uint(), payload(1,1):uint(), payload(2,1):uint()
    if version == 2 then
        if length < 52 then bad("Truncated CONTROL v2 header"); return end
        local hops = payload(3,1):uint()
        local offset, total, command_length = payload(36,4):uint(), payload(40,4):uint(), payload(44,2):uint()
        local destination_length, reply_length = payload(46,2):uint(), payload(48,2):uint()
        local start = 52 + destination_length + reply_length
        local data_length = length - start - command_length
        if not control_kinds[kind] or hops < 1 or hops > 16 or payload(50,2):uint() ~= 0 or
           payload(4,16):raw() == string.rep("\0",16) or payload(20,16):raw() == string.rep("\0",16) or
           command_length > 256 or destination_length > 1024 or reply_length > 1024 or data_length < 0 then
            bad("Invalid CONTROL v2 metadata or lengths"); return
        end
        if (kind <= 5 and (not control_states[state] or (kind ~= 1 and command_length ~= 0))) or
           (kind >= 6 and (state ~= 0 or offset ~= 0 or total ~= 0)) or
           ((kind == 2 or kind == 4 or kind == 5 or kind == 6) and data_length ~= 0) then
            bad("Invalid CONTROL v2 kind/state/body combination"); return
        end
        if (kind == 1 or (kind == 3 and state >= 4 and state <= 7)) and
           (total > 1048576 or offset > total or data_length > total-offset) then
            bad("CONTROL v2 block exceeds declared length"); return
        end
        local function path(at, size, field)
            if size == 0 then return true end
            local subtree, finish, count = tree:add(field,payload(at,size)), at+size, 0
            while at < finish do
                count = count + 1
                if finish-at < 2 or count > 16 then return false end
                local tag, n = payload(at,1):uint(), payload(at+1,1):uint()
                if at+2+n > finish or not ((tag==1 and n==0) or (tag==2 and n>0 and n<=63) or (tag==3 and n==16)) then return false end
                if tag==2 and payload(at+2,n):string():find("[^!-~]") then return false end
                if tag==3 and payload(at+2,n):raw()==string.rep("\0",16) then return false end
                subtree:add(f_control_hop_type,payload(at,1))
                if n>0 then subtree:add(f_control_hop_value,payload(at+2,n)) end
                at=at+2+n
            end
            return true
        end
        if not path(52,destination_length,f_control_destination) or
           not path(52+destination_length,reply_length,f_control_reply_path) then bad("Invalid CONTROL routing stack"); return end
        tree:add(f_control_version,payload(0,1)); tree:add(f_control_kind,payload(1,1))
        tree:add(f_control_state,payload(2,1)); tree:add(f_control_hops,payload(3,1))
        tree:add(f_control_id,payload(4,16)); tree:add(f_control_origin,payload(20,16))
        tree:add(f_control_offset,payload(36,4)); tree:add(f_control_total,payload(40,4))
        tree:add(f_control_command_length,payload(44,2))
        if command_length>0 then tree:add(f_control_command,payload(start,command_length)) end
        if data_length>0 then tree:add(f_control_data,payload(start+command_length,data_length)) end
        pinfo.cols.info:append(string.format(", v2 %s, hops=%d, offset=%d, total=%d",control_kinds[kind],hops,offset,total))
        return
    end
    local offset, total, command_length = payload(20,4):uint(), payload(24,4):uint(), payload(28,2):uint()
    if version ~= 1 or kind > 5 or not control_kinds[kind] or not control_states[state] then
        bad("Unsupported CONTROL version, kind or state"); return
    end
    if payload(3,1):uint() ~= 0 or payload(30,2):uint() ~= 0 then
        bad("Nonzero CONTROL reserved bytes"); return
    end
    if payload(4,16):raw() == string.rep("\0",16) then bad("Zero CONTROL request ID"); return end
    if command_length > 256 or length < 32 + command_length then
        bad("Invalid or truncated CONTROL command"); return
    end
    local data_length = length - 32 - command_length
    if (kind ~= 1 and command_length ~= 0) or
       ((kind == 2 or kind == 4 or kind == 5) and data_length ~= 0) then
        bad("Unexpected CONTROL command or body"); return
    end
    -- STATUS offsets refer to an independently stored response, and RECEIVING
    -- replies acknowledge upload bytes with total=0; neither is a body block.
    if (kind == 1 or (kind == 3 and state >= 4 and state <= 7)) and
       (total > 1048576 or offset > total or data_length > total - offset) then
        bad("CONTROL body block exceeds declared length or 1 MiB limit"); return
    end
    tree:add(f_control_version, payload(0,1))
    tree:add(f_control_kind, payload(1,1))
    tree:add(f_control_state, payload(2,1))
    tree:add(f_control_id, payload(4,16))
    tree:add(f_control_offset, payload(20,4))
    tree:add(f_control_total, payload(24,4))
    tree:add(f_control_command_length, payload(28,2))
    if command_length > 0 then tree:add(f_control_command, payload(32,command_length)) end
    if data_length > 0 then tree:add(f_control_data, payload(32+command_length,data_length)) end
    local id = tostring(payload(4,16):bytes()):gsub(":", ""):lower()
    pinfo.cols.info:append(string.format(", %s, id=%s, offset=%d, total=%d",
        control_kinds[kind], id, offset, total))
    if kind == 3 or kind == 5 then pinfo.cols.info:append(", " .. control_states[state]) end
    if command_length > 0 then pinfo.cols.info:append(", " .. payload(32,command_length):string()) end
end

local function dissect_v5(buffer, pinfo, tree)
    if buffer:len() < 25 then return 0 end
    local flags = buffer(0, 1):uint()
    local kind = flags % 16
    local encrypted = flags >= 128
    local fragment = math.floor(flags / 64) % 2 == 1
    if math.floor(flags / 16) % 4 ~= 0 or not packet_type_names[kind] or
       (fragment and kind ~= 3 and kind ~= 12) then return 0 end
    local handshake = kind == 8 or kind == 9
    local meta = 9
    if fragment then meta = kind == 12 and 25 or 21
    elseif handshake then meta = 18
    elseif kind == 6 or kind == 7 then meta = 19
    elseif kind >= 4 and kind <= 11 then meta = 17 end
    local header = meta + 16
    if buffer:len() < header then return 0 end
    local length = buffer:len() - header
    local info = "v5, " .. packet_type_names[kind]
    pinfo.cols.protocol = "TUNTOM"
    pinfo.cols.info = info
    local subtree = tree:add(tuntom, buffer(), "tuntom " .. info)
    subtree:add(f_type, buffer(0, 1), kind)
    subtree:add(f_sequence, buffer(1, 8))
    subtree:add(f_session_hint, buffer(1, 2))
    subtree:add(f_counter, buffer(3, 6))
    subtree:add(f_auth_tag, buffer(meta, 16))
    if handshake then subtree:add(f_version, buffer(9, 1)) end
    if meta > 9 then
        local field = (kind == 4 or kind == 5) and f_probe_id or
            ((kind == 6 or kind == 7) and f_mtu_probe_id or f_message_id)
        subtree:add(field, buffer(handshake and 10 or 9, 8))
    end
    if kind == 6 or kind == 7 then subtree:add(f_mtu, buffer(17, kind == 12 and 4 or 2)) end
    if fragment then
        subtree:add(f_fragment_offset, buffer(17, kind == 12 and 4 or 2))
        subtree:add(f_original_length, buffer(kind == 12 and 21 or 19, kind == 12 and 4 or 2))
    end
    if kind == 3 or kind == 12 then add_generated_field(subtree, f_fragmented, fragment) end
    if handshake then
        local fields_end = header + (kind == 8 and 44 or 68)
        if length < (kind == 8 and 44 or 68) then
            subtree:add_proto_expert_info(e_handshake, "Truncated handshake payload")
            return buffer:len()
        end
        local nonce_offset = header + (kind == 8 and 0 or 32)
        if kind == 9 then subtree:add(f_init_hash, buffer(header, 32)) end
        subtree:add(f_nonce, buffer(nonce_offset, 32))
        if kind == 8 then subtree:add(f_init_timestamp, buffer(header + 32, 8)) end
        local suite = buffer(fields_end - 4, 2):uint()
        local dh = buffer(fields_end - 2, 2):uint()
        subtree:add(f_suite, buffer(fields_end - 4, 2))
        subtree:add(f_dh_length, buffer(fields_end - 2, 2))
        if buffer:len() > fields_end then subtree:add(f_dh, buffer(fields_end)) end
        if buffer(9, 1):uint() ~= 5 or encrypted or
           buffer(1, 8):uint64() ~= UInt64(0, 0) or
           buffer(10, 8):uint64() == UInt64(0, 0) or suite > 2 or
           dh ~= (suite == 2 and 32 or 0) or buffer:len() ~= fields_end + dh then
            subtree:add_proto_expert_info(e_handshake,
                "Expected suite 0/1 without DH or suite 2 with 32-byte DH, version 5 and exact message length")
        end
        return buffer:len()
    end
    if encrypted then
        pinfo.cols.info:append(", Ascon-AEAD128 encrypted")
        if length > 0 then subtree:add(f_payload, buffer(header)) end
        return buffer:len()
    end
    if kind == 10 or kind == 11 then
        if length ~= 0 or buffer(3, 6):uint64() ~= UInt64(0, 0) or
           buffer(9, 8):uint64() == UInt64(0, 0) then
            subtree:add_proto_expert_info(e_handshake, "Invalid confirmation")
        end
        return buffer:len()
    end
    if kind == 14 then
        dissect_control(buffer, header, pinfo, subtree)
        return buffer:len()
    end
    if kind == 13 then
        dissect_info(buffer, header, subtree)
        return buffer:len()
    end
    if length == 0 then return buffer:len() end
    local payload = buffer(header)
    if kind ~= 3 and kind ~= 12 then
        subtree:add(kind == 6 and f_mtu_padding or f_payload, payload)
        return buffer:len()
    end
    if not fragment then
        if kind == 12 then dissect_ipc(payload:tvb(), pinfo, subtree)
        else dissect_inner_ip_with_tuntom_info(payload:tvb(), pinfo, subtree, info) end
        return buffer:len()
    end
    local offset = buffer(17, kind == 12 and 4 or 2):uint()
    local original = buffer(kind == 12 and 21 or 19, kind == 12 and 4 or 2):uint()
    subtree:add(f_payload, payload)
    add_generated_field(subtree, f_fragment_length, length)
    add_generated_field(subtree, f_fragment_end, offset + length)
    -- UDP endpoints identify the tunnel; no tunnel ID is inferred from payload.
    local key = message_key(pinfo, 0, buffer(9, 8):uint64(), 5, buffer(1, 2):uint()) .. "|type=" .. kind
    local raw, err
    if not pinfo.visited then
        raw, err = accept_fragment_first_pass(pinfo, key, offset, original, payload, kind == 12 and 65639 or 65535)
    elseif frame_info[pinfo.number] then
        raw = frame_info[pinfo.number].complete_raw
    end
    if err then subtree:add("Reassembly: " .. err) end
    add_reassembly_links(subtree, pinfo)
    if raw then
        if kind == 12 then
            add_generated_field(subtree, f_reassembled_length, original)
            dissect_ipc(ByteArray.new(raw,true):tvb("Reassembled IPC"),pinfo,subtree)
        else dissect_reassembled_raw(raw, pinfo, subtree, original, info .. ", reassembled") end
        add_generated_field(subtree, f_reassembled_in, pinfo.number)
    end
    return buffer:len()
end

function tuntom.dissector(buffer, pinfo, tree)
    if buffer:len() < 8 then
        return 0
    end

    if buffer(0, 4):string() ~= "UTUN" then
        return dissect_v5(buffer, pinfo, tree)
    end

    local version =
        buffer(6, 1):uint()

    local packet_type =
        buffer(7, 1):uint()

    local encrypted = version == 4 and packet_type >= 128
    if encrypted then packet_type = packet_type - 128 end

    local header_size

    if version == 1 then
        header_size = 8
    elseif version == 2 then
        header_size = 32
    elseif (version == 3 or version == 4) then
        header_size = 48
    else
        return 0
    end

    if buffer:len() < header_size then
        return 0
    end

    pinfo.cols.protocol = "TUNTOM"

    local tunnel_id =
        buffer(4, 2):uint()

    local type_name =
        packet_type_names[packet_type] or
        ("UNKNOWN(" .. packet_type .. ")")

    local payload_length =
        buffer:len() - header_size

    local info = string.format(
        "Tunnel %u, v%u, %s",
        tunnel_id,
        version,
        type_name
    )

    if (version == 3 or version == 4) and
       packet_type == 3 then

        local fragment_offset =
            buffer(24, 4):uint()

        local original_length =
            buffer(28, 4):uint()

        local fragment_end =
            fragment_offset + payload_length

        local fragmented =
            not fragment_is_complete_packet(
                fragment_offset,
                payload_length,
                original_length
            )

        if fragmented then
            info = info .. string.format(
                ", fragment %u..%u/%u (%u bytes)",
                fragment_offset,
                fragment_end,
                original_length,
                payload_length
            )
        else
            info = info .. string.format(
                ", complete %u bytes",
                payload_length
            )
        end

        local cached =
            frame_info[pinfo.number]

        if cached ~= nil and
           cached.reassembled_in ~= nil then

            if cached.reassembled_in ==
               pinfo.number then

                info = info .. ", reassembled"
            else
                info = info .. string.format(
                    ", reassembled in #%u",
                    cached.reassembled_in
                )
            end
        end
    end

    if (version == 3 or version == 4) and
       (packet_type == 4 or packet_type == 5) then

        local probe_id =
            buffer(16, 8):uint64()

        info = info ..
            ", probe=" ..
            tostring(probe_id)
    elseif (version == 3 or version == 4) and
           (packet_type == 6 or packet_type == 7) then

        local probe_id =
            buffer(16, 8):uint64()

        local mtu =
            buffer(28, 4):uint()

        info = info ..
            ", pmtud=" ..
            tostring(probe_id) ..
            ", mtu=" ..
            tostring(mtu)
    end

    pinfo.cols.info = info

    local subtree =
        tree:add(
            tuntom,
            buffer(),
            string.format(
                "tuntom, Tunnel %u, v%u, %s",
                tunnel_id,
                version,
                type_name
            )
        )

    subtree:add(
        f_magic,
        buffer(0, 4)
    )

    subtree:add(
        f_tunnel_id,
        buffer(4, 2)
    )

    subtree:add(
        f_version,
        buffer(6, 1)
    )

    subtree:add(
        f_type,
        buffer(7, 1), packet_type
    )

    if version == 2 then
        subtree:add(
            f_sequence,
            buffer(8, 8)
        )

        subtree:add(
            f_auth_tag,
            buffer(16, 16)
        )
    elseif (version == 3 or version == 4) then
        subtree:add(
            f_sequence,
            buffer(8, 8)
        )

        if packet_type == 3 then
            add_v3_fragment_fields(
                subtree,
                buffer,
                payload_length
            )
        else
            if packet_type == 4 or packet_type == 5 then
                subtree:add(
                    f_probe_id,
                    buffer(16, 8)
                )
            elseif packet_type == 6 or packet_type == 7 then
                subtree:add(
                    f_mtu_probe_id,
                    buffer(16, 8)
                )
            else
                subtree:add(
                    f_message_id,
                    buffer(16, 8)
                )
            end

            subtree:add(
                f_fragment_offset,
                buffer(24, 4)
            )

            if packet_type == 6 or packet_type == 7 then
                subtree:add(
                    f_mtu,
                    buffer(28, 4)
                )
            else
                subtree:add(
                    f_original_length,
                    buffer(28, 4)
                )
            end
        end

        subtree:add(
            f_auth_tag,
            buffer(32, 16)
        )
    end

    if encrypted then
        pinfo.cols.info:append(", Ascon-AEAD128 encrypted")
        subtree:add(f_session_hint, buffer(8, 2))
        subtree:add(f_counter, buffer(10, 6))
        if payload_length > 0 then subtree:add(f_payload, buffer(header_size)) end
        return buffer:len()
    end

    if version == 4 then
        subtree:add(f_session_hint, buffer(8, 2))
        subtree:add(f_counter, buffer(10, 6))
        if packet_type >= 8 and packet_type <= 11 then
            local bad = buffer(16, 8):uint64() == UInt64(0, 0) or
                buffer(24, 4):uint() ~= 0 or buffer(28, 4):uint() ~= 0
            if packet_type == 8 or packet_type == 9 then
                local nonce_offset = packet_type == 8 and 48 or 80
                local suite_offset = nonce_offset + (packet_type == 8 and 40 or 32)
                local fields_end = suite_offset + 4
                if buffer:len() < fields_end then
                    subtree:add_proto_expert_info(e_handshake, "Truncated handshake payload")
                    return buffer:len()
                end
                if packet_type == 9 then subtree:add(f_init_hash, buffer(48, 32)) end
                subtree:add(f_nonce, buffer(nonce_offset, 32))
                if packet_type == 8 then subtree:add(f_init_timestamp, buffer(80, 8)) end
                subtree:add(f_suite, buffer(suite_offset, 2))
                subtree:add(f_dh_length, buffer(suite_offset + 2, 2))
                local dh_length = buffer(suite_offset + 2, 2):uint()
                local remaining = buffer:len() - fields_end
                if remaining > 0 then subtree:add(f_dh, buffer(fields_end, remaining)) end
                local suite = buffer(suite_offset, 2):uint()
                local expected_dh = suite == 2 and 32 or 0
                bad = bad or buffer(8, 8):uint64() ~= UInt64(0, 0) or
                    suite > 2 or
                    dh_length ~= expected_dh or remaining ~= dh_length
            else
                bad = bad or payload_length ~= 0 or buffer(10, 6):uint64() ~= UInt64(0, 0)
            end
            if bad then
                subtree:add_proto_expert_info(e_handshake,
                    "Expected suite 0/1 without DH or suite 2 with 32-byte DH, zero control metadata and exact message length")
            end
            return buffer:len()
        end
    end

    add_reassembly_links(
        subtree,
        pinfo
    )

    if payload_length <= 0 then
        return buffer:len()
    end

    local payload =
        buffer(header_size)

    if packet_type ~= 3 then
        subtree:add(
            f_payload,
            payload
        )

        return buffer:len()
    end

    --
    -- V1/V2 DATA is always a complete inner IP packet.
    --
    if (version ~= 3 and version ~= 4) then
        local payload_tree =
            subtree:add(
                tuntom,
                payload,
                string.format(
                    "Encapsulated IP packet (%u bytes)",
                    payload:len()
                )
            )

        dissect_inner_ip_with_tuntom_info(
            payload:tvb(),
            pinfo,
            payload_tree,
            info
        )

        return buffer:len()
    end

    if (version == 3 or version == 4) and packet_type == 6 then
        if payload_length > 0 then
            local padding =
                buffer(
                    protocol_header_v3_size,
                    payload_length
                )

            subtree:add(
                f_mtu_padding,
                padding
            )
        end

        return buffer:len()
    end

    if (version == 3 or version == 4) and packet_type == 7 then
        return buffer:len()
    end

    --
    -- V3 DATA.
    --
    local message_id =
        buffer(16, 8):uint64()

    local fragment_offset =
        buffer(24, 4):uint()

    local original_length =
        buffer(28, 4):uint()

    local complete_in_one =
        fragment_is_complete_packet(
            fragment_offset,
            payload_length,
            original_length
        )

    if complete_in_one then
        local payload_tree =
            subtree:add(
                tuntom,
                payload,
                string.format(
                    "Encapsulated IP packet (%u bytes)",
                    payload:len()
                )
            )

        dissect_inner_ip_with_tuntom_info(
            payload:tvb(),
            pinfo,
            payload_tree,
            info
        )

        return buffer:len()
    end

    local fragment_tree =
        subtree:add(
            tuntom,
            payload,
            string.format(
                "tuntom fragment (%u bytes, offset %u, original %u)",
                payload_length,
                fragment_offset,
                original_length
            )
        )

    fragment_tree:add(
        f_payload,
        payload
    )

    local key =
        message_key(
            pinfo,
            tunnel_id,
            message_id,
            version,
            version == 4 and buffer(8, 2):uint() or 0
        )

    local complete_raw = nil
    local reassembly_error = nil

    if not pinfo.visited then
        complete_raw, reassembly_error =
            accept_fragment_first_pass(
                pinfo,
                key,
                fragment_offset,
                original_length,
                payload
            )
    else
        local cached =
            frame_info[pinfo.number]

        if cached ~= nil and
           cached.complete_raw ~= nil then

            complete_raw =
                cached.complete_raw
        end
    end

    if reassembly_error ~= nil then
        fragment_tree:add(
            string.format(
                "Reassembly: %s",
                reassembly_error
            )
        )
    end

    --
    -- On the completion frame, expose the synthetic packet and run the
    -- ordinary IP dissector over the exact reconstructed bytes.
    --
    if complete_raw ~= nil then
        local completion_info = info

        if string.find(
            completion_info,
            ", reassembled",
            1,
            true
        ) == nil then
            completion_info =
                completion_info .. ", reassembled"
        end

        dissect_reassembled_raw(
            complete_raw,
            pinfo,
            subtree,
            original_length,
            completion_info
        )

        add_generated_field(
            subtree,
            f_reassembled_in,
            pinfo.number
        )
    end

    return buffer:len()
end

--
-- Register the deterministic tuntom UDP port range.
--
local udp_port_table =
    DissectorTable.get("udp.port")

for port = 40001, 40255 do
    udp_port_table:add(
        port,
        tuntom
    )
end
