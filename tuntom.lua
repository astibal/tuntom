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
-- Protocol v3:
--   0..3   magic            "UTUN"
--   4..5   tunnel_id        uint16 BE
--   6      version          3
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

tuntom.fields = {
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
    message_id
)
    return string.format(
        "%s|tun=%u|msg=%s",
        direction_key(pinfo),
        tunnel_id,
        tostring(message_id)
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
    payload
)
    if original_length == 0 or
       original_length > max_original_length then
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
       max_fragments_per_message then
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

function tuntom.dissector(buffer, pinfo, tree)
    if buffer:len() < 8 then
        return 0
    end

    if buffer(0, 4):string() ~= "UTUN" then
        return 0
    end

    local version =
        buffer(6, 1):uint()

    local packet_type =
        buffer(7, 1):uint()

    local header_size

    if version == 1 then
        header_size = 8
    elseif version == 2 then
        header_size = 32
    elseif version == 3 then
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

    if version == 3 and
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

    if version == 3 and
       (packet_type == 4 or packet_type == 5) then

        local probe_id =
            buffer(16, 8):uint64()

        info = info ..
            ", probe=" ..
            tostring(probe_id)
    elseif version == 3 and
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
        buffer(7, 1)
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
    elseif version == 3 then
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
    if version ~= 3 then
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

    if version == 3 and packet_type == 6 then
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

    if version == 3 and packet_type == 7 then
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
            message_id
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
