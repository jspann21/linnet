# ALAC decoder provenance

Based on [Rockbox](https://github.com/Rockbox/rockbox),
commit `99c18f336c730f533d3b7d2d8951f5dc4a6da802`,
`lib/rbcodec/codecs/libalac`. The original MIT notice remains in `alac.c`
and [LICENSE](LICENSE).

Source SHA-256 values (CRLF checkout line endings):

| File | SHA-256 |
| --- | --- |
| `alac.c` | `e33facf8b5015f283c1f8548cdc233bd216f0c9c122a45e195f8dcb4c80611cd` |
| `decomp.h` | `f28f873a30433966b32c3c62e22cab2d55d517d99b1051404c1cd522ba223b75` |
| `README.rockbox` | `ca809ccd5e135f1c1427893f9bc6e6bd4cf96a8e71b1e6a17370c595f86ddffc` |

Linnet adds freestanding declarations, an exact-length bit reader, bounded frame,
sample, predictor, and Rice handling, and explicit rejection of unsupported
prediction and low-byte planes. Output is restricted to 16-bit mono/stereo.

The four-tap predictor uses local coefficient/sample state and unrolled bounded
adaptation while retaining upstream wrapping and stopping semantics. Static
coefficient scratch still requires serialized decoder calls.

The M4A demux and asynchronous facade are Linnet code; see
[their contract](../../services/audio/alac/README.md). Emulator instruction
comparisons are not hardware throughput evidence.
