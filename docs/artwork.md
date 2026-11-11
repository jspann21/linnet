# Album artwork and Cover Flow

Media Manager prepares artwork with the music library. The host decodes source
images, applies orientation and color-profile normalization, crops and resizes
covers, and converts them to the target's display colors. The device loads
prepared pixels and checksums. Prepare Music again after changing source artwork.

The [music format guide](music-format.md) describes how PODB references artwork.

## Prepared images

| Target | Playback artwork | Cover Flow |
| --- | --- | --- |
| Photo P98 | 64 × 64 RGB565 cover | 96 × 96 cover plus reflection |
| Monochrome Q21 | 64 × 64 cover prepared for four gray levels, stored as RGB565 | Disabled |
| Video M25 | One bundle containing 160 × 160 and 112 × 112 RGB565 renditions | 130 × 130 cover plus reflection |
| Classic N25 | One bundle containing 160 × 160, 112 × 112, and 126 × 126 RGB565 renditions | 130 × 130 cover plus reflection |

Playback files live under `/.LINNET/ART/`. PODB records their path, dimensions,
encoding, byte length, and CRC. Encoding 2 identifies the Video/Classic bundles;
encoding 1 identifies the Photo/Monochrome square covers. Each bundled rendition
is resized from the normalized source rather than from another small rendition.

The 112 px and 126 px renditions match their Now Playing layouts. Classic's
home artwork uses the 160 px source, scaled to fill the pane below the status
bar. In the 160 × 216 pane, the square cover is drawn at 216 × 216 and cropped
equally at the sides. Home layout depends on the selected theme.

Cover Flow uses a separate catalog tied to the music database. Video and Classic
use `LCFN` tiles with a 256-entry RGB565 palette; Photo uses `LCF2`. Album rows
reference deduplicated tiles. The native mapped catalog also records the
representative cover's playback identity and CRC.

## Loading and caching

On Video and Classic, Now Playing can immediately show a matching resident
Cover Flow tile while the full playback bundle loads. Matching checks the
library identity, artwork path, and CRC across published catalog rows. The
requested song does not have to belong to the album currently selected in
Cover Flow. Uncached covers and distinct per-track artwork use the full reader.

The preview owns its pixel buffer, so it can appear while an older read drains.
The full reader keeps two buffers, verifies a complete read before publication,
and waits for pending reads before reusing their storage. A retained full-cover
cache hit can publish immediately when buffer ownership permits it. A track
without artwork does not retain the previous track's cover.

Native Cover Flow starts with six decoded tiles plus a packed tile cache. Cache sizes
come from the target's memory allocation; they are not album-count limits.
Warm resident navigation avoids artwork reads, while uncached navigation still
depends on storage latency and the audio work budget.

Video preparation can append independently compressed LZ4 blocks containing
the exact native tiles to `COVERS.CFL`. The native records remain intact. Firmware
validates the appended header, payload CRC, contiguous block index, and each
decoded tile's CRC. When the complete compressed set fits, every cover remains
resident and navigation decodes from RAM. Seven visible cards are pinned through
each draw. M25 can retain ten decoded RGB565 tiles when the existing spare-memory
lease is available; three use texture workspace freed by sharing raw decode
scratch. Playback and compressed-input buffers remain separate.

Packed preparation seals the block index with its own checksum. M25
can validate that index and show individually checked covers from the completed
prefix before the full set finishes loading. Full payload validation remains
mandatory, and final publication reuses the verified index without rescanning
it.

A small raw-only library on a qualified 64 MiB M25 can retain every decoded
cover in an otherwise unused artwork array. Admission keeps the Files cache and
history loan in their original winning region. Covers expand during existing
bounded preparation steps; revisits reuse their RGB pixels without added reads.

The additional memory comes from unused extended library metadata memory or,
on a 32 MiB solid-state Video, the unused audio read-window area in STAGE. The
AAC decoder, video packet caches, and PCM buffers retain their existing regions.
Library reload drains readers before reclaiming either lease. Firmware upload
drains artwork and inhibits further STAGE use until reboot. Libraries that do
not fit retain the bounded native cache.

## Checking artwork changes

Use the existing host validators, image verification, simulator, and
[Link diagnostics and captures](device-link.md#diagnostics-input-and-screenshots).
For changes to preparation, mapping, or loading, check:

1. Orientation, crop, color conversion, and exact rendition dimensions against
   the normalized source. Compare full playback pixels separately from the
   palette-based Cover Flow preview.
2. Songs opened from a different album than the Cover Flow selection, rapid
   replacement selections, coverless tracks, and preview-to-full promotion.
3. Cold reads and warm navigation during audio, including a rotating drive.
   Observe artwork reads, cache hits, audio underruns, and completed frames.
4. Malformed or truncated catalogs, mismatched identities/CRCs, and cancellation
   while a read still owns its buffer.
5. Physical scrolling, panel appearance, and clipping on the affected target.
   A framebuffer capture verifies pixels, but cannot establish LCD smoothness.

Keep measurements and captures with the exact build in
[local artifacts](local-artifacts.md). Static linker headroom and runtime stack
usage are different measurements; neither should be inferred from an older build.

Implementation: [host artwork preparation](../tools/content/music.py),
[Cover Flow encoder and validator](../tools/content/coverflow.py),
[artwork loader](../services/music/artwork.c),
[Cover Flow cache](../services/music/coverflow.c), and
[renderer](../ui/runtime/ui.c).
