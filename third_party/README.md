# Vendor source index

These directories contain imported decoder code. Their own notices and
provenance files remain authoritative for license terms and exact changes.
Native integration flags and source selection live in
[`tools/studio/build.py`](../tools/studio/build.py); the build report records
each selected source and its extra compiler flags. The common OS build uses
`-fwrapv` and freestanding `-fno-builtin` flags.

Playback uses the Linnet [decoder interface](../services/music/decoder.h).
Vendor types and fixed playback arenas are contained in its implementation and
the codec facades; runtime callers do not include vendor decoder headers.

| Dependency | Origin and license | Local adaptation and integration |
| --- | --- | --- |
| [iPod model catalogue](ipod_models/PROVENANCE.md) | libgpod model/serial tables; [LGPL-2.1-or-later](ipod_models/COPYING). | Desktop identification metadata with regional model normalization and ambiguity preserved. Hardware admission remains separate. |
| [AAC](aac/PROVENANCE.md) | FAAD and shared Rockbox codec helpers; GPL, LGPL, and Xiph BSD components have their respective notices under `aac/`. | Fixed-point AAC-LC and bounded decoder state; [`SOURCES`](aac/SOURCES) selects vendor files. ARM4 codec flags, include paths, and `-O3` are grouped in the build rule. |
| [libmad MP3](libmad/PROVENANCE.md) | libmad from Rockbox; [GPL-2.0-or-later](libmad/COPYING). | Freestanding memory shim and ARM fixed-point path. The build keeps the ordered libmad source set and its `-O3`, ARM, and include flags. |
| [Tremor Vorbis](vorbis/README.linnet.md) | Xiph Tremor from Rockbox; [Xiph BSD](vorbis/COPYING). | Caller-owned pool and bounded Ogg handling. The build selects service and vendor sources with Tremor and shared AAC FFT include paths; it does not enable the AAC ARM define for Tremor. |
| [FLAC](flac/README.linnet.md) | FFmpeg FLAC decoder from Rockbox; [LGPL-2.0-or-later](flac/COPYING). | Bounded fixed-point decoder and Linnet asynchronous wrapper; `decoder.c` is selected with the service wrapper under the common freestanding and wrapping flags. |
| [ALAC](alac/PROVENANCE.md) | libalac from Rockbox; [MIT](alac/LICENSE). | Exact-length bit reader and bounded mono/stereo decoding. The build links the vendor decoder with Linnet's ALAC wrapper and M4A demux. |
| [h264bsd](h264bsd/LICENSE.md) | Android Open Source Project H.264 decoder, as stated in its [Apache-2.0 notice](h264bsd/LICENSE.md); no exact upstream revision is recorded here. | Linnet's native decoder wrapper and compatibility header constrain the feature set. The build selects H.264 C sources with `-O3`, compatibility include, and narrow warning exceptions on native MP4 targets. |
| [PacketVideo MPEG-4](pvmp4/NOTICE) | AOSP OpenCORE, with exact revision and security backports in [UPSTREAM](pvmp4/UPSTREAM); [Apache-2.0 license](pvmp4/LICENSE) and [notice](pvmp4/NOTICE). | Freestanding C++ compatibility and bounded decoder integration; [SOURCES](pvmp4/SOURCES) selects vendor files. |

The source lists and per-backend flags are part of build identity. Keep notices
with imported files, update the corresponding provenance record when changing
vendor code, and verify the affected target's build and media behavior.
