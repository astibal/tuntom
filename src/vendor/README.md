# Vendored X25519

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
