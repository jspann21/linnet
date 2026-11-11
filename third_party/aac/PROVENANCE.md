# AAC-LC integration

Based on [Rockbox](https://github.com/Rockbox/rockbox),
commit `99c18f336c730f533d3b7d2d8951f5dc4a6da802`, from its `libfaad` and shared
codec `lib` directories. [provenance.json](provenance.json) records original
and adapted file hashes, using CRLF checkout line endings.

## Licenses

Original notices are retained. FAAD and selected lookup files are GPL-2.0-or-later;
see [faad/COPYING](faad/COPYING). Shared FFT/IMDCT files carry their respective
LGPL notices, with [LGPL text](licenses/LGPL-2.1.txt). Shared Tremor math helpers
use the [Xiph BSD license](licenses/Tremor-BSD.txt). Consult each source file's notice.

## Linnet adaptations

- Fixed-point AAC-LC configuration with freestanding memory operations and bounded integer arithmetic.
- Caller-owned decoder workspace and random state; calls must run serially, never concurrently or in interrupt handlers.
- Bounded bit reader and access units; no allocation on the supported LC path.
- ARM FFT/MDCT and multiply helpers selected through the build rules.
- Explicit first-frame output and container-owned priming/tail trimming.
- Bounded asynchronous read windows for M4A table and media access.

The facade supports mono/stereo AAC-LC in unprotected single-block ADTS and the
bounded M4A reader. HE-AAC/SBR, PS, PCE, ADIF, unsupported AAC profiles, and
960-sample frames are outside its accepted profile. Seeking uses bounded steps
and the facade's available container and checkpoint state; consult the APIs for
the current paths and required read ownership.

[SOURCES](SOURCES) selects vendor code. Current flags and includes are owned by
[build.py](../../tools/studio/build.py), not copied into this document. Shared
FFT/MDCT symbols must be linked once when another codec uses them.
The build report identifies FAAD sources separately from the shared AAC/Vorbis
transforms, preserving the same compiler flags and one linked implementation.

APIs: [raw/ADTS AAC](../../services/audio/aac/aac.h),
[M4A AAC](../../services/audio/aac/m4a.h), and
[shared M4A demux](../../services/audio/alac/README.md).
Host/emulator checks do not establish hardware timing or listening quality.
