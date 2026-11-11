# Music library (PODB)

PODB is Linnet's immutable music index at `/.LINNET/LIBRARY.PDB`. The host
prepares metadata, browse groups, search keys, and artwork; the device reads
bounded records instead of scanning media files.

Use [Media Manager](media-manager.md) for everyday imports. The compiler indexes
audio and can stage unchanged copies; it does not transcode audio. A codec ID
describes the source encoding, not a guarantee that a target can play it.

## Playback compatibility

The firmware provides decoders for integer PCM in WAV/AIFF, MP3, AAC in M4A or
ADTS, ALAC, FLAC, and Vorbis. Channel count, sample rate, profile, and frame
limits still apply. Host metadata readers also recognize formats such as Opus
and WavPack that have no native decoder.

Media Manager checks compatibility for the selected target and reports skipped
files with a reason. It can prepare PCM copies of incompatible unprotected
audio or selected music while retaining originals and metadata. This conversion
is separate from PODB compilation. Exact limits live in the
[host compatibility check](../tools/content/music.py) and
[native decoder](../services/music/decoder.c).

Media Manager also prepares AAC-LC seek checkpoints and FLAC seek metadata on
the iPod's copies. This preparation is separate from the PODB index; its formats
and checks are owned by the [AAC preparation tool](../tools/content/aac_index.py)
and [FLAC preparation tool](../tools/content/flac_index.py).

## Structure

The database contains a header followed by track, group, membership, search,
artwork, and string sections. Integers are little-endian. Current sizes and
field offsets are defined in the [host compiler](../tools/content/music.py)
and [firmware reader](../services/music/database.c); keep those in agreement
when changing the format.

| Section | Stores |
| --- | --- |
| Tracks | Stable identity, media path and size, encoding, sample counts, tags, artwork reference, and gain information |
| Groups and members | Artists, albums, genres, composers, years, folders, and playlists |
| Search | Sorted keys for tracks and artist/album groups |
| Artwork | Pixel dimensions, prepared-file reference, length, and checksum |
| Strings | Bounded, deduplicated, NUL-terminated UTF-8 |

Libraries are limited to 100,000 tracks and 128 MiB. The header CRC and payload
SHA-256 cover the database; readers also check section bounds, references, string
starts, and reserved fields. These checks detect corruption, not authenticity.
Rebuild an incompatible catalog with Media Manager.

## Identity and paths

Track IDs derive from source-relative paths. Editing tags preserves identity;
moving a file changes it. Albums use album-artist ownership while track artist
credits remain intact. Playlists preserve their supplied order and repeats.

In-place imports reference actual FAT short aliases. Unsupported aliases may
require verified compatibility copies; originals stay intact. Fast verification
uses file size and modification time. Full verification additionally hashes media.
Neither a valid database nor fast verification proves the source audio is intact.

Host-decoded sample counts supply duration where container headers do not.
Native decoder timing takes precedence during playback.

## Artwork

PODB references target-specific RGB565 covers under `/.LINNET/ART/`.
Video and Classic bundle exact Now Playing renditions with the larger cover;
Photo and monochrome 4G use 64 × 64 covers. Cover Flow has a separate catalog
bound to the music database and, on Video/Classic, mapped playback identities.

See [album artwork and Cover Flow](artwork.md) for sizes, encodings, preparation,
resident previews, and validation. Keep the artwork files and Cover Flow catalog
consistent with the published PODB generation.

## Inspect and publish

```console
python linnet music inspect path/to/LIBRARY.PDB
```

The [publisher](../tools/content/publish.py) verifies staged objects and device
identity before replacing the index. Keep an open player and its library on
the same generation across a swap. Mutable playback history and queue state
are separate from PODB; see [binary contracts](formats.md).

Implementation entry points: [compiler](../tools/content/music.py),
[database API](../services/music/database.h), [search](../services/music/search.c),
and [library owner](../services/music/library.c).
