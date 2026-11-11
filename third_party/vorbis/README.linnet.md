# Tremor integration

Sources originate from `lib/rbcodec/codecs/libtremor/` in
[Rockbox](https://github.com/Rockbox/rockbox), revision
`99c18f336c730f533d3b7d2d8951f5dc4a6da802`. Original notices and the [Xiph BSD license](COPYING) remain.
Shared FFT/MDCT sources retain their notices under [AAC](../aac/PROVENANCE.md).

Linnet omits the upstream file/Ogg allocation layer. Its facade owns asynchronous
reads, Ogg CRC/sequence checks, packet assembly, and source positions. PCM scratch
state is context-owned. Shared FFT code is linked once; current flags and source
selection live in [build.py](../../tools/studio/build.py).

Each context uses a fixed caller-owned pool and bounded page/packet buffers.
Exhaustion unwinds the codec call and resets the pool rather than passing an
unchecked allocation failure into the decoder. No allocation uses the system heap.

Supported scope is one unmultiplexed, unchained Ogg Vorbis stream, mono/stereo,
within bounded rates, block sizes, codebooks, packets, and pool capacity.
Sequential seeking restarts and discards in bounded steps; there is no fast
seek index. Close/restart resets the owned state.

See [the facade](../../services/audio/vorbis/vorbis.c) for exact admission and
read limits. Hardware timing and listening checks are separate from host
reference-decoder comparisons.
