#!/usr/bin/env python3
"""Offline dissector checks; no capture privileges, networking or third-party modules."""
import pathlib
import struct
import subprocess
import tempfile


def wire(kind, payload=b"", seq=0, offset=0, original=0, version=5):
    if version != 5:
        return struct.pack("!IHBBQQII", 0x5554554E, 42, version, kind,
                           seq, 99, offset, original) + bytes(16) + payload
    base_kind = kind & 15
    extension = b""
    if base_kind in (3,12) and (offset or original != len(payload)):
        kind |= 0x40
        extension = struct.pack("!QII" if base_kind == 12 else "!QHH", 99, offset, original)
    elif base_kind in (8, 9):
        extension = struct.pack("!BQ", 5, 99)
    elif base_kind in (6, 7):
        extension = struct.pack("!QH", 99, original)
    elif base_kind in (4, 5, 10, 11):
        extension = struct.pack("!Q", 99)
    return struct.pack("!BQ", kind, seq) + extension + bytes(16) + payload


def frame(payload, reverse=False):
    src, dst = (40042, 45000) if reverse else (45000, 40042)
    udp = struct.pack("!HHHH", src, dst, len(payload) + 8, 0) + payload
    a, b = bytes([192, 0, 2, 1]), bytes([192, 0, 2, 2])
    if reverse:
        a, b = b, a
    return struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 1, 0,
                       64, 17, 0, a, b) + udp


root = pathlib.Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="tuntom-dissector-") as directory:
    directory = pathlib.Path(directory)
    # The host may already load an installed tuntom plugin. Register the
    # checkout under a unique name and override only this test process's ports.
    lua = directory / "test.lua"
    lua.write_text((root / "tuntom.lua").read_text().replace("tuntom", "tuntom_test"))
    hint = 0xABCD << 48
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 40, 1, 0, 64, 17, 0,
                     bytes([10, 0, 0, 1]), bytes([10, 0, 0, 2])) + bytes(20)
    records = [
        frame(wire(8, bytes(40) + bytes(4))),
        frame(wire(9, bytes(64) + bytes(4)), True),
        frame(wire(10, seq=hint)),
        frame(wire(11, seq=hint), True),
        frame(wire(3, ip[:20], hint | 2, 0, 40)),
        # Same message ID, different session hint: must not complete frame 5.
        frame(wire(3, ip[20:], (0x1234 << 48) | 1, 20, 40)),
        frame(wire(3, ip[20:], hint | 1, 20, 40)),
        frame(wire(8, bytes(12))),  # truncated
        frame(wire(8, bytes(40) + b"\x00\x00\x00\x01\x00")),  # nonempty DH
        frame(wire(8, bytes(40) + b"\x00\x02\x00\x00")),  # missing suite-2 DH
        frame(wire(3, ip, 100, 0, 40, 3)),  # historical v3 still dissects
        frame(wire(8, bytes(40) + b"\x00\x01\x00\x00")),
        frame(wire(0x83, ip, hint | 3, 0, 40)),  # ciphertext resembling IP
        frame(wire(8, bytes(40) + b"\x00\x02\x00\x20" + bytes(32))),
        frame(wire(9, bytes(64) + b"\x00\x02\x00\x20" + bytes(32)), True),
        frame(wire(8, bytes(40) + b"\x00\x02\x00\x1f" + bytes(31))),
    ]
    labels = [17,42,int.from_bytes(b'hTXVIA\x01\x05','big'), (7<<32)|(2<<8)|2,123,17,42]
    ipc = struct.pack('!BBBBI',1,2,0,len(labels),8+8*len(labels)+len(ip)) + b''.join(struct.pack('!Q',v) for v in labels) + ip
    def relay(kind, channel, payload=b''):
        return b'TTR\x01' + struct.pack('!B3xIIQQ',kind,channel,32+len(payload),1234,0) + payload
    data = relay(4,9,ipc)
    snapshot = relay(2,3,struct.pack('!HIQB',1,9,22,7)+b'proxy-0')
    records += [frame(wire(12,data,hint|20,0,len(data))),
                frame(wire(12,data[:50],hint|21,0,len(data))),
                frame(wire(12,data[50:],hint|22,50,len(data))),
                frame(wire(12,snapshot,hint|23,0,len(snapshot))),
                frame(wire(0x8c,data,hint|24,0,len(data))),
                frame(wire(12,b'TTR\x01',hint|25,0,4))]
    info_start = len(records)
    info_valid = b'access=10.0.0.1,10.0.0.2\ncustom=  Praha, centrum=x \t\nempty='
    info_bad = [b'', b'\n', b'access=x\n\n', b'access=x\nmissing',
                b'a=1\na=2', b'Bad=x', b'a =x', b'a-b=x', b'=x',
                b'a=x\r\n', b'a=\x00', b'a=\x7f', b'a=\xc4\x8d',
                b'\xef\xbb\xbfa=x', b'a=' + b'x' * 4095]
    records += [frame(wire(13, info_valid, hint | 30)),
                frame(wire(13, b'access=\n', hint | 31)),
                frame(wire(0x8d, info_valid, hint | 32)),
                frame(wire(13, b'a=' + b'x' * 4094, hint | 33))]
    records += [frame(wire(13, payload, hint | (40 + i))) for i, payload in enumerate(info_bad)]
    control_start = len(records)
    request_id = bytes(range(1, 17))
    def control(kind=1, state=1, offset=0, total=0, command=b'', data=b''):
        return struct.pack('!BBBB16sIIHH', 1, kind, state, 0, request_id,
                           offset, total, len(command), 0) + command + data
    put = control(total=8, command=b'classifier load 8', data=b'format 1')
    control_good = [put,
                   control(kind=2, offset=512),
                   control(kind=3, offset=8),  # upload ACK, total deliberately zero
                   control(kind=3, state=4, total=1024, data=b's' * 512),
                   control(kind=3, state=4, offset=512, total=1024, data=b's' * 512),
                   control(kind=4, state=4, total=1024),
                   control(kind=5, state=4, total=1024)]
    control_good += [control(kind=3, state=state) for state in (2, 3, 5, 6, 7, 8)]
    records += [frame(wire(14, payload, hint | (80 + i))) for i, payload in enumerate(control_good)]
    records += [frame(wire(0x8e, put, hint | 100))]  # encrypted bytes resembling CONTROL
    control_bad = [b'', put[:31], bytes([2]) + put[1:],
                   put[:1] + b'\x06' + put[2:], put[:2] + b'\x09' + put[3:],
                   put[:3] + b'\x01' + put[4:], put[:4] + bytes(16) + put[20:],
                   put[:30] + b'\x00\x01' + put[32:],
                   control(command=b'x' * 257), put[:35],
                   control(kind=2, command=b'x'), control(kind=4, data=b'x'),
                   control(offset=2, total=1), control(total=0, data=b'x'),
                   control(kind=3, state=4, total=1048577)]
    records += [frame(wire(14, payload, hint | (110 + i))) for i, payload in enumerate(control_bad)]
    v2_start = len(records)
    def control_v2(kind=1, state=1, destination=b'\x01\x00', reply=b'', command=b'show stats', data=b''):
        return bytes([2,kind,state,16])+request_id+bytes(range(16,32))+struct.pack('!IIHHHH',0,len(data),len(command),len(destination),len(reply),0)+destination+reply+command+data
    v2_good = [control_v2(), control_v2(kind=6,state=0,command=b''),
               control_v2(kind=7,state=0,command=b'port:adapter')]
    records += [frame(wire(14,payload,hint|(140+i))) for i,payload in enumerate(v2_good)]
    v2_bad = [control_v2()[:51], control_v2(destination=b'\x03\x10short'),
              control_v2(destination=b'\x01\x01x'), control_v2(kind=6,state=1,command=b'')]
    records += [frame(wire(14,payload,hint|(150+i))) for i,payload in enumerate(v2_bad)]
    pcap = directory / "packets.pcap"
    pcap.write_bytes(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 101) +
                     b"".join(struct.pack("<IIII", i, 0, len(p), len(p)) + p
                              for i, p in enumerate(records, 1)))
    fields = ["tuntom_test.version", "tuntom_test.type", "tuntom_test.session_hint",
              "tuntom_test.counter", "tuntom_test.dh_length", "tuntom_test.suite",
              "tuntom_test.reassembled_length", "_ws.expert.message",
              "tuntom_test.ipc.channel", "tuntom_test.ipc.label", "tuntom_test.ipc.via.chain",
              "tuntom_test.ipc.via.action", "tuntom_test.ipc.name",
              "tuntom_test.info.entry", "tuntom_test.info.key", "tuntom_test.info.value",
              "tuntom_test.control.version", "tuntom_test.control.kind", "tuntom_test.control.state",
              "tuntom_test.control.request_id", "tuntom_test.control.offset", "tuntom_test.control.total",
              "tuntom_test.control.command_length", "tuntom_test.control.command", "tuntom_test.control.data"]
    args = ["tshark", "-X", "lua_script:" + str(lua), "-r", str(pcap), "-T", "fields"]
    for field in fields:
        args.extend(["-e", field])
    result = subprocess.run(args, capture_output=True, text=True, check=True)
    assert "Lua" not in result.stderr, result.stderr
    rows = [line.split("\t") for line in result.stdout.splitlines()]
    assert len(rows) == len(records), result.stdout
    assert rows[0][1:2] == ["8"] and rows[0][4:6] == ["0", "0"], rows[0]
    assert rows[1][1] == "9" and rows[1][4:6] == ["0", "0"], rows[1]
    assert rows[0][0] == "5" and rows[1][0] == "5", rows[:2]
    assert not rows[2][0] and not rows[4][0], "Version should only occur in handshake"
    assert rows[2][2:4] == ["0xabcd", "0"], rows[2]
    assert rows[3][1:4] == ["11", "0xabcd", "0"], rows[3]
    assert rows[4][3] == "2" and not rows[4][6], rows[4]
    assert not rows[5][6], "Fragments from different session hints were mixed"
    assert rows[6][6] == "40", rows[6]
    assert "Truncated handshake" in rows[7][7], rows[7]
    assert "Expected suite 0" in rows[8][7], rows[8]
    assert "Expected suite 0" in rows[9][7], rows[9]
    assert rows[10][0] == "3", rows[10]
    assert rows[11][5] == "1" and not rows[11][7], rows[11]
    assert rows[12][1] == "3" and not rows[12][6], rows[12]
    assert rows[13][4:6] == ["32", "2"] and not rows[13][7], rows[13]
    assert rows[14][4:6] == ["32", "2"] and not rows[14][7], rows[14]
    assert "Expected suite 0" in rows[15][7], rows[15]
    assert rows[16][8] == '9' and rows[16][10:12] == ['7','1'], rows[16]
    assert len(rows[16][9].split(',')) == 7, rows[16]
    assert rows[18][6] == str(len(data)) and rows[18][10:12] == ['7','1'], rows[18]
    assert rows[19][12] == 'proxy-0', rows[19]
    assert not rows[20][8], 'encrypted IPC was decoded as plaintext'
    assert 'Truncated relay' in rows[21][7], rows[21]
    assert "Lua Error" not in result.stdout, result.stdout
    info_rows = rows[info_start:control_start]
    assert info_rows[0][1] == '13' and not info_rows[0][7], info_rows[0]
    assert info_rows[0][13] == 'access=10.0.0.1,10.0.0.2,custom=Praha, centrum=x,empty=', info_rows[0]
    assert info_rows[0][14] == 'access,custom,empty', info_rows[0]
    assert info_rows[1][13:16] == ['access=', 'access', ''], info_rows[1]
    assert not info_rows[2][7] and not any(info_rows[2][13:16]), 'encrypted INFO parsed as text'
    assert info_rows[3][14] == 'a' and len(info_rows[3][15]) == 4094, info_rows[3]
    for row in info_rows[4:]:
        assert row[7] and not any(row[13:16]), 'malformed INFO published partial fields: ' + repr(row)
    control_rows = rows[control_start:v2_start]
    for index, row in enumerate(control_rows[:len(control_good)]):
        assert row[1] == '14' and not row[7], row
        assert row[16] == '1' and row[19].replace(':', '') == request_id.hex(), row
    assert control_rows[0][20:24] == ['0', '8', '17', 'classifier load 8'], control_rows[0]
    assert control_rows[0][24].replace(':', '') == b'format 1'.hex(), control_rows[0]
    assert control_rows[1][17] == '2' and control_rows[1][20] == '512', control_rows[1]
    assert control_rows[2][20:22] == ['8', '0'], control_rows[2]
    assert control_rows[4][20:22] == ['512', '1024'], control_rows[4]
    assert control_rows[6][17:19] == ['5', '4'], control_rows[6]
    encrypted_control = control_rows[len(control_good)]
    assert encrypted_control[1] == '14' and not encrypted_control[7] and not any(encrypted_control[16:]), encrypted_control
    for row in control_rows[len(control_good)+1:]:
        assert row[7] and not any(row[16:]), 'malformed CONTROL published fields: ' + repr(row)
    for row in rows[v2_start:v2_start+len(v2_good)]:
        assert not row[7] and row[16]=='2', row
    for row in rows[v2_start+len(v2_good):]:
        assert row[7], row
    raw_capture = directory / 'ipc.pcap'
    raw_capture.write_bytes(struct.pack('<IHHIIII',0xA1B2C3D4,2,4,0,0,70000,147) +
        b''.join(struct.pack('<IIII',i,0,len(p),len(p))+p for i,p in enumerate((ipc,data,snapshot,v2_good[0]),1)))
    raw_args = args.copy()
    raw_args[raw_args.index('-r')+1] = str(raw_capture)
    raw_result = subprocess.run(raw_args,capture_output=True,text=True,check=True)
    assert 'Lua' not in raw_result.stderr and 'Lua Error' not in raw_result.stdout, raw_result
    raw_rows = [line.split('\t') for line in raw_result.stdout.splitlines()]
    assert raw_rows[0][10:12] == ['7','1'], raw_rows
    assert raw_rows[1][8] == '9' and raw_rows[2][12] == 'proxy-0', raw_rows
    assert raw_rows[3][16]=='2' and not raw_rows[3][7], raw_rows[3]

print("PASS: Wireshark V5 handshakes, DATA/IPC reassembly, relay channels, VIA fields, ASCII INFO, CONTROL blocks/states and malformed messages")
