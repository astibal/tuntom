#!/usr/bin/env python3
"""Demo credential backend: accept alice / laboratory-secret from a TTA1 request."""
import struct
import sys

wire = sys.stdin.buffer.read()
if len(wire) < 28 or wire[:4] != b"TTA\x01" or wire[4] != 1:
    raise SystemExit(2)
username_len, peer_len = struct.unpack_from("!HH", wire, 16)
challenge_len, response_len = struct.unpack_from("!II", wire, 20)
if len(wire) != 28 + username_len + peer_len + challenge_len + response_len:
    raise SystemExit(2)
at = 28
username = wire[at:at + username_len]
at += username_len + peer_len + challenge_len
response = wire[at:at + response_len]
raise SystemExit(0 if username == b"alice" and response == b"laboratory-secret" else 1)
