# AKDF v1 specification

AKDF v1 is an extract/expand key derivation construction based on [AMAC
v1](AMAC_V1.md). It is not HKDF or a standardized Ascon KDF. The domain
constants below are part of the algorithm; no additional version byte is
inserted.

## Interface and notation

```text
Extract(K, id, Z) -> PRK
Expand(PRK, id, L, C) -> output

K       exactly 16 bytes of secret extraction key
id      unsigned 16-bit domain identifier (0..65535)
Z       exactly 32 bytes of input keying material
PRK     exactly 16 bytes of intermediate key
L       ASCII label bytes, excluding any NUL terminator or embedded NUL
C       arbitrary context byte string, including the empty string
output  exactly 16 bytes
```

`||` means byte concatenation, `0x00` and `0x01` each denote one byte, and
`BE64(n)` encodes an unsigned 64-bit integer in big-endian order. `len(C)`
counts bytes and must fit in 64 bits. The context may contain any byte values.
`AMAC(K, id, M)` returns the full 16-byte tag defined in [AMAC v1](AMAC_V1.md).
Use the same domain identifier for extraction and expansion.

## Extraction

The fixed extraction domain `D` is the following 22-byte constant, expressed in
hexadecimal. Decode it to bytes; do not use the hexadecimal text as input.

```text
D = 54554e544f4d2d414b44462d76312d45585452414354
PRK = AMAC(K, id, D || 0x00 || Z)
```

The domain including its delimiter occupies 23 bytes; the complete AMAC message
is 55 bytes. All 32 bytes of `Z` are used in their supplied order, without
encoding, integer conversion, or truncation. Validation of the source keying
material belongs to the caller; extraction does not reject any particular value
of `Z`.

## Expansion

```text
output = AMAC(PRK, id, L || 0x00 || BE64(len(C)) || C || 0x01)
```

Exactly one NUL follows the label. Exactly eight length bytes precede the
context and exactly one `0x01` byte follows it. No previous output block is
prepended. Each invocation returns one full AMAC block; no variable-length or
multi-block expansion is defined. Different output purposes use different
labels. Labels are supplied without their terminating NUL.

The caller defines the context and its encoding. If it contains multiple fields,
their encoding must be unambiguous, for example through fixed field sizes or
explicit length prefixes. AKDF treats the context as opaque bytes and does not
define message formats, roles, or a key exchange protocol.

## Regression vectors

Inputs below are hexadecimal except the decimal domain identifier. Label
constants are also hexadecimal encodings of bytes, not literal hex strings.

```text
K = 000102030405060708090a0b0c0d0e0f
id = 42
Z = 202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f
C = 000102ff
BE64(len(C)) = 0000000000000004
L1 = 54554e544f4d2d414b44462d76312d433253
L2 = 54554e544f4d2d414b44462d76312d533243
L3 = 54554e544f4d2d414b44462d76312d48494e54

PRK = Extract(K, id, Z)
O1 = Expand(PRK, id, L1, C)
O2 = Expand(PRK, id, L2, C)
O3 = Expand(PRK, id, L3, C)
```

| Output | Value (hexadecimal) |
|---|---|
| PRK | `9918fbc76d8a36087c9d4adb33d80ed9` |
| O1 | `d665fb496abc0d32716b716afd690388` |
| O2 | `f2edcdf874d30a54816c9daec844f7ae` |
| O3 | `47d51288b8455b2f72bd1a2375ade0b2` |


These labels are test inputs, not a required application-specific output set.
Every expansion returns all 16 bytes.

## Security assumptions

The extract/expand structure is HKDF-like, but its security does not follow from
HKDF's analysis. The 16-byte PRK and outputs cap claimed strength at 128 bits.
Test vectors verify reproducibility, not cryptographic security.

Using this construction for forward secrecy relies on the unproven assumption
that AMAC extraction hides high-entropy input keying material even if the
extraction key later becomes known. It also depends on the security of the
source key exchange, authentication, and erasure of ephemeral secrets. AKDF
alone does not provide forward secrecy or authenticate a peer.

Input keying material, extraction keys, PRKs, and derived keys must be handled
as secrets. Their lifetime, erasure, and use are the responsibilities of the
caller.

## Implementation references

- [C++ implementation](../src/akdf.hpp)
- [Framing, domain separation, and input-binding tests](../tests/akdf_test.cpp)
- [Independent test-only vector generator](../tests/akdf_vectors.py)
