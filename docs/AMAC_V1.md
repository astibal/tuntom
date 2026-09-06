# AMAC v1 specification

AMAC is a keyed message authentication code based on the Ascon permutation. It
is not HMAC, a standardized Ascon-MAC profile, or Ascon-AEAD128. No version byte
is prepended to the message or tag. A related extract/expand construction is
specified in [AKDF v1](AKDF_V1.md).

## Interface and notation

```text
AMAC(K, id, M) -> T

K   exactly 16 key bytes (128 bits)
id  unsigned 16-bit domain identifier (0..65535)
M   arbitrary byte string, including the empty string
T   exactly 16 tag bytes (128 bits), without truncation
```

The domain identifier is a separate input chosen by the application. AMAC has no
nonce input and does not itself provide replay protection or confidentiality.
Message framing and domain labels belong to its callers.

The state consists of five unsigned 64-bit words `s[0]..s[4]`. All complements,
shifts and bitwise operations below operate on 64-bit words; `~x` complements
all 64 bits. `^`, `&` and `||` denote XOR, bitwise AND and byte concatenation.
`BE64(bytes)` loads exactly eight bytes most-significant byte first; `ENC64(x)`
encodes a word in the same order. `ROTR(x, n)` rotates a word right by `n` bits.
Slices use an exclusive upper bound.

## Permutation

`P12` uses all twelve round constants below in order. `P8` uses only the last
eight, starting with `b4`. Constants are hexadecimal, zero-extended to 64 bits.

```text
f0 e1 d2 c3 b4 a5 96 87 78 69 5a 4b
```

For each selected constant `c`, apply this round in the written order:

```text
s[2] ^= c

s[0] ^= s[4]
s[4] ^= s[3]
s[2] ^= s[1]

# Compute all five temporaries before changing any state word.
t[0] = ~s[0] & s[1]
t[1] = ~s[1] & s[2]
t[2] = ~s[2] & s[3]
t[3] = ~s[3] & s[4]
t[4] = ~s[4] & s[0]

s[0] ^= t[1]
s[1] ^= t[2]
s[2] ^= t[3]
s[3] ^= t[4]
s[4] ^= t[0]

s[1] ^= s[0]
s[0] ^= s[4]
s[3] ^= s[2]
s[2] = ~s[2]

# Each assignment uses the previous value of that word on its entire RHS.
s[0] = s[0] ^ ROTR(s[0], 19) ^ ROTR(s[0], 28)
s[1] = s[1] ^ ROTR(s[1], 61) ^ ROTR(s[1], 39)
s[2] = s[2] ^ ROTR(s[2],  1) ^ ROTR(s[2],  6)
s[3] = s[3] ^ ROTR(s[3], 10) ^ ROTR(s[3], 17)
s[4] = s[4] ^ ROTR(s[4],  7) ^ ROTR(s[4], 41)
```

## Initialization, absorption and finalization

The absorption rate is eight bytes. The fixed initial domain word is
`0x54554e544f4d4d41`. This constant is part of the algorithm.

```text
k0 = BE64(K[0:8])
k1 = BE64(K[8:16])
u  = zero_extend_to_64_bits(id)
s  = [0x54554e544f4d4d41, k0, k1, u, ~u]
s  = P12(s)
s[3] ^= k0
s[4] ^= k1

# Absorb every complete block, including a final complete message block.
while length(M) >= 8:
    s[0] ^= BE64(M[0:8])
    s = P8(s)
    M = M[8:]

# M now contains 0..7 bytes. Always append a padded block.
B = M || 0x80 || zero_bytes(7 - length(M))
s[0] ^= BE64(B)
s[4] ^= 1

# No P8 occurs between the padded block and finalization.
s[1] ^= k0
s[2] ^= k1
s = P12(s)
s[3] ^= k0
s[4] ^= k1
T = ENC64(s[3]) || ENC64(s[4])
```

Empty messages and messages whose lengths are multiples of eight both receive a
separate `80 00 00 00 00 00 00 00` padding block. No message length, implicit
NUL byte or other prefix/suffix is added. The ID complement is taken **after**
zero-extension: for ID 42, `u = 0x000000000000002a` and
`~u = 0xffffffffffffffd5`.

Verification recomputes the complete tag with the same key, ID and message and
compares all 16 bytes using a constant-time comparison. A mismatch is rejected.

## Regression vectors

All values below are hexadecimal except the decimal message length. For every
row, `K = 000102030405060708090a0b0c0d0e0f`, `id = 42`, and message byte
`M[i] = i` for `0 <= i < length` (thus length 0 means an empty message).

| Message length (bytes) | Tag |
|---:|---|
| 0 | `2aad841a9aa2adc884208ea8f62a8a87` |
| 1 | `683e70577abb233f972101110d599890` |
| 7 | `9560aa1bbe356a3a4b6fb983023354e3` |
| 8 | `bf5b71e039cf24a2bce494d490da4235` |
| 9 | `635187af2fce91c92095af5548a6e9d1` |
| 15 | `69b4260c66d4a7281f320018a3e63c67` |
| 16 | `8893da72cf4c75d71999fd6c816eb9e2` |
| 17 | `8f3090050ecbc089bdc5d0c3661981e6` |
| 64 | `67b1cfc1b8b6d600e5190b5b787eec34` |


These vectors cover empty input and block/padding boundaries. They are not
standardized Ascon-Mac known-answer tests and do not establish the security of
this custom mode.

## Implementation references

- [C++ implementation](../src/ascon.hpp)
- [Permutation comparisons and MAC regression tests](../tests/mac_test.cpp)
- [Independent test-only lookup-S-box implementation](../tests/akdf_vectors.py)
