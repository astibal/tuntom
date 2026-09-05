#!/usr/bin/env python3
"""Offline dissector checks; no capture privileges, networking or third-party modules."""
import pathlib
import struct
import subprocess
import tempfile


def wire(kind, payload=b"", seq=0, offset=0, original=0, version=4):
    return struct.pack("!IHBBQQII", 0x5554554E, 42, version, kind,
                       seq, 99, offset, original) + bytes(16) + payload


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
    pcap = directory / "packets.pcap"
    pcap.write_bytes(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 101) +
                     b"".join(struct.pack("<IIII", i, 0, len(p), len(p)) + p
                              for i, p in enumerate(records, 1)))
    fields = ["tuntom_test.version", "tuntom_test.type", "tuntom_test.session_hint",
              "tuntom_test.counter", "tuntom_test.dh_length", "tuntom_test.suite",
              "tuntom_test.reassembled_length", "_ws.expert.message"]
    args = ["tshark", "-X", "lua_script:" + str(lua), "-r", str(pcap), "-T", "fields"]
    for field in fields:
        args.extend(["-e", field])
    result = subprocess.run(args, capture_output=True, text=True, check=True)
    assert "Lua" not in result.stderr, result.stderr
    rows = [line.split("\t") for line in result.stdout.splitlines()]
    assert len(rows) == len(records), result.stdout
    assert rows[0][1:2] == ["8"] and rows[0][4:6] == ["0", "0"], rows[0]
    assert rows[1][1] == "9" and rows[1][4:6] == ["0", "0"], rows[1]
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
    assert "Lua Error" not in result.stdout, result.stdout
print("PASS: Wireshark V4 handshake fields, counters, session fragment separation and malformed messages")
