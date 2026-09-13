# Vendored code

## Linux SipHash-2-4

`siphash.hpp` is a portable C++17 extraction from Linux **v6.12**:

- https://github.com/torvalds/linux/blob/v6.12/lib/siphash.c
- https://github.com/torvalds/linux/blob/v6.12/include/linux/siphash.h
- https://github.com/torvalds/linux/blob/v6.12/lib/siphash_kunit.c (test vectors)

`tools/extract_siphash.py` pins SHA-256 checksums for all inputs and regenerates
the header and `tests/siphash_vectors.hpp`. Download the three files above plus
`LICENSES/preferred/BSD-3-Clause` into one directory under their base filenames,
then run `python3 tools/extract_siphash.py DIRECTORY`. Builds need no downloads
or external libraries.

The extraction retains the SipHash-2-4 permutation, constants, generic unaligned
routine and the 8/16-byte specializations. Kernel dependencies are replaced by
fixed-width C++ types, bounded little-endian loads and a rotate helper. It selects
the portable tail-read branch, converts void-pointer arithmetic to byte-pointer
arithmetic, adds inline linkage and a namespace, and scopes/undefines macros.
HalfSipHash, kernel exports and architecture-specific word-access helpers are
excluded. The generated files and `LICENSE.md` retain the BSD-3-Clause option.

`switch_ecmp_test` checks all 64 Linux reference vectors at eight alignments,
the fixed-size helpers, symmetric IPv4/IPv6 keys, fragment/L3 fallback, flow
distribution and rendezvous stability. ECMP's fixed public hash keys provide
deterministic routing; they are not authentication secrets.

## Monocypher X25519

`x25519.hpp` is a dependency-closed extraction from Monocypher **4.0.3**:
https://github.com/LoupVaillant/Monocypher/blob/4.0.3/src/monocypher.c

The original file's SHA-256 is pinned in `tools/extract_x25519.py` and in the
output header. The script selects exact upstream line ranges and changes only
namespace-scope functions/constants to C++17 `inline` linkage, adds C++ headers/namespace, and
undefines helper macros. Arithmetic and compiler timing mitigations are retained
verbatim. Upstream's unused algorithms, constants and helpers are excluded.

Reproduce from the original downloaded file (not needed for building tuntom):

```sh
python3 tools/extract_x25519.py /path/to/monocypher.c
```

The header retains the upstream BSD-2-Clause OR CC0-1.0 license. This project
uses the BSD-2-Clause option; binary redistributions must include its notice
and disclaimer (available in the header and `LICENSE.md` at repository root).
The upstream file labels itself `__git__`; its origin is the pinned 4.0.3 tag.

Validation: RFC 7748 key exchange and 1/1000-iteration vectors, scalar clamping,
public high-bit masking, noncanonical coordinates and zero/low-order inputs.
During extraction the result was also compared against the complete unmodified
4.0.3 implementation on 10,000 deterministically generated input pairs. These
checks establish regression confidence, not a new audit of this extraction.
