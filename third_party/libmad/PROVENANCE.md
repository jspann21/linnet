# libmad integration

Based on [Rockbox](https://github.com/Rockbox/rockbox),
commit `99c18f336c730f533d3b7d2d8951f5dc4a6da802`,
`lib/rbcodec/codecs/libmad`. [provenance.json](provenance.json) records original
and adapted hashes, using CRLF checkout line endings. Original copyright notices and
[GPL-2.0-or-later terms](COPYING) remain with the code.

## Linnet adaptations

- Freestanding memory, byte-order, and bounded division helpers replace global codec APIs.
- Caller-owned fixed state; ARM and host arithmetic paths are selected by the private configuration.
- ARM assembly macros and constraints are adapted to Linnet's ARM-state callers.
- Memory operations preserve alignment and overlap behavior.
- Requantization-cache validity is tracked separately from values; synthesis stays out of line.
- `MAD_OPTION_SCAN` maintains Layer III reservoir state while skipping spectral work during resume.

The [MP3 facade](../../services/audio/mp3/mp3.c) exposes Layer III. Current source
selection and flags live in [build.py](../../tools/studio/build.py).

## Playback contract

The facade uses bounded input and caller-owned state, with at most one frame
decode or source read per step. It reads recognized Xing/Info and encoder trim
fields when plausible; missing metadata does not imply known gapless bounds.

Seek restarts incrementally. The scan path restores ordinary decoding before the
target to warm synthesis; skipped spectral payloads are not Huffman-validated.
Recoverable frame errors are counted. EOF alone is not source-integrity proof.

Player scheduling owns cross-track joins, resampling policy, and media verification.
Fixed-point host and ARM paths can differ in low output bits; host comparison
does not establish real-time hardware behavior.
