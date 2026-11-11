# ALAC and M4A integration

The native ALAC facade decodes 16-bit mono/stereo M4A into signed 16-bit stereo
PCM. Mono is duplicated. One frame is decoded per step, then caller-sized output
blocks drain without re-decoding.

The shared M4A reader also supplies AAC-LC access units. It parses the container;
AAC decoding is owned by its separate facade.

## Supported boundary

- One audio track in nonfragmented, self-contained M4A.
- Bounded atom nesting, sample/chunk tables, packet sizes, and asynchronous reads.
- ALAC mono/stereo, 16-bit output; no separate low-byte planes or unsupported predictors.
- AAC-LC with a supported rate and mono/stereo configuration.
- A supported leading edit and exact packet timing for trim handling.

Encrypted/external media, unsupported edits, extra tracks, and incompatible
sample entries reject explicitly. Parsed sample rates still need hardware clock
support in the player.

## Ownership and seeking

There is no system heap. The caller owns fixed state, but the imported ALAC core
retains static coefficient scratch, so decoder calls must be serialized.
Compile its predictor arithmetic with the build's wrapping semantics.

The consumer must finish using a returned packet before requesting another.
Pending reads keep the same buffer, offset, and length. Cancel and drain the
outer storage operation before resetting or seeking the decoder.

Seeking uses bounded timing/chunk runs and discards only the necessary frame
prefix. Exact limits and APIs are in [alac.h](alac.h) and [alac.c](alac.c).
Player integration remains responsible for media integrity, scheduling, and
physical audio qualification.

See [ALAC provenance](../../../third_party/alac/PROVENANCE.md) and
[AAC integration](../../../third_party/aac/PROVENANCE.md) for imported code and notices.
