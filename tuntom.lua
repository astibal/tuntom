-- tuntom.lua
--
-- Wireshark dissector for the tuntom TUN-over-UDP protocol.
--
-- Protocol v1:
--   0..3   magic      "UTUN"
--   4..5   tunnel_id  uint16 BE
--   6      version    1
--   7      type
--   8..    payload
--
-- Protocol v2:
--   0..3   magic      "UTUN"
--   4..5   tunnel_id  uint16 BE
--   6      version    2
--   7      type
--   8..15  sequence   uint64 BE
--   16..31 auth_tag   16 bytes
--   32..   payload
--
-- Packet types:
--   1 = HELLO
--   2 = KEEPALIVE
--   3 = DATA
--
-- This dissector does not verify the Ascon authentication tag.
-- For DATA packets it hands the payload to Wireshark's IPv4/IPv6 dissector.
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
}

local f_magic = ProtoField.string("tuntom.magic", "Magic")
local f_tunnel_id = ProtoField.uint16("tuntom.tunnel_id", "Tunnel ID", base.DEC)
local f_version = ProtoField.uint8("tuntom.version", "Version", base.DEC)
local f_type = ProtoField.uint8("tuntom.type", "Type", base.DEC, packet_type_names)
local f_sequence = ProtoField.uint64("tuntom.sequence", "Sequence", base.DEC)
local f_auth_tag = ProtoField.bytes("tuntom.auth_tag", "Auth Tag")
local f_payload = ProtoField.bytes("tuntom.payload", "Payload")

tuntom.fields = {
    f_magic,
    f_tunnel_id,
    f_version,
    f_type,
    f_sequence,
    f_auth_tag,
    f_payload,
}

local ip_dissector = Dissector.get("ip")
local ipv6_dissector = Dissector.get("ipv6")

local function dissect_inner_ip(payload, pinfo, tree)
    if payload:len() == 0 then
        return
    end

    local first_byte = payload(0, 1):uint()
    local ip_version = bit32.rshift(first_byte, 4)

    if ip_version == 4 and ip_dissector ~= nil then
        ip_dissector:call(payload:tvb(), pinfo, tree)
    elseif ip_version == 6 and ipv6_dissector ~= nil then
        ipv6_dissector:call(payload:tvb(), pinfo, tree)
    else
        tree:add(f_payload, payload)
    end
end

function tuntom.dissector(buffer, pinfo, tree)
    if buffer:len() < 8 then
        return 0
    end

    if buffer(0, 4):string() ~= "UTUN" then
        return 0
    end

    local version = buffer(6, 1):uint()
    local packet_type = buffer(7, 1):uint()

    local header_size

    if version == 1 then
        header_size = 8
    elseif version == 2 then
        header_size = 32
        if buffer:len() < header_size then
            return 0
        end
    else
        return 0
    end

    pinfo.cols.protocol = "TUNTOM"

    local tunnel_id = buffer(4, 2):uint()
    local type_name = packet_type_names[packet_type] or ("UNKNOWN(" .. packet_type .. ")")

    pinfo.cols.info = string.format(
        "Tunnel %u, v%u, %s",
        tunnel_id,
        version,
        type_name
    )

    local subtree = tree:add(
        tuntom,
        buffer(),
        string.format("tuntom, Tunnel %u, v%u, %s", tunnel_id, version, type_name)
    )

    subtree:add(f_magic, buffer(0, 4))
    subtree:add(f_tunnel_id, buffer(4, 2))
    subtree:add(f_version, buffer(6, 1))
    subtree:add(f_type, buffer(7, 1))

    if version == 2 then
        subtree:add(f_sequence, buffer(8, 8))
        subtree:add(f_auth_tag, buffer(16, 16))
    end

    if buffer:len() > header_size then
        local payload = buffer(header_size)

        if packet_type == 3 then
            local payload_tree = subtree:add(
                tuntom,
                payload,
                string.format("Encapsulated IP packet (%u bytes)", payload:len())
            )

            dissect_inner_ip(payload, pinfo, payload_tree)
        else
            subtree:add(f_payload, payload)
        end
    end

    return buffer:len()
end

--
-- Register the deterministic tuntom UDP port range.
--
local udp_port_table = DissectorTable.get("udp.port")

for port = 40001, 40255 do
    udp_port_table:add(port, tuntom)
end
