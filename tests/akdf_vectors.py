#!/usr/bin/env python3
"""Independent, slow lookup-S-box AMAC/AKDF regression vector generator.
Not constant time; never used by the application. No third-party modules.
"""
SBOX = [4, 11, 31, 20, 26, 21, 9, 2, 27, 5, 8, 18, 29, 3, 6, 28,
        30, 19, 7, 14, 0, 13, 17, 24, 16, 12, 1, 25, 22, 10, 15, 23]
MASK = (1 << 64) - 1
ROTATIONS = [(19, 28), (61, 39), (1, 6), (10, 17), (7, 41)]


def permute(s, rounds):
    for r in range(12 - rounds, 12):
        s[2] ^= ((15-r) << 4) | r
        t = [0]*5
        for bit in range(64):
            x = sum(((s[j] >> bit) & 1) << (4-j) for j in range(5))
            for j in range(5):
                t[j] |= ((SBOX[x] >> (4-j)) & 1) << bit
        s = [x ^ ((x >> a) | ((x << (64-a)) & MASK)) ^
             ((x >> b) | ((x << (64-b)) & MASK))
             for x, (a, b) in zip(t, ROTATIONS)]
    return s


def mac(key, tunnel, message):
    k0, k1 = (int.from_bytes(key[i:i+8], 'big') for i in (0, 8))
    s = permute([0x54554e544f4d4d41, k0, k1, tunnel, MASK ^ tunnel], 12)
    s[3] ^= k0
    s[4] ^= k1
    full = len(message) // 8 * 8
    for i in range(0, full, 8):
        s[0] ^= int.from_bytes(message[i:i+8], 'big')
        s = permute(s, 8)
    s[0] ^= int.from_bytes((message[full:] + b'\x80').ljust(8, b'\0'), 'big')
    s[4] ^= 1
    s[1] ^= k0
    s[2] ^= k1
    s = permute(s, 12)
    return (s[3] ^ k0).to_bytes(8, 'big') + (s[4] ^ k1).to_bytes(8, 'big')


if __name__ == '__main__':
    psk = bytes(range(16))
    assert mac(psk, 42, b'').hex() == '2aad841a9aa2adc884208ea8f62a8a87'
    prk = mac(psk, 42, b'TUNTOM-AKDF-v1-EXTRACT\0' + bytes(range(32, 64)))
    print('PRK', prk.hex())
    context = bytes([0, 1, 2, 255])
    for label in ('C2S', 'S2C', 'HINT'):
        info = ('TUNTOM-AKDF-v1-' + label).encode() + b'\0'
        print(label, mac(prk, 42, info + len(context).to_bytes(8, 'big') + context + b'\x01').hex())
