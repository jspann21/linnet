# Prepared photos (LPIC)

LPIC is the tiled photo library used by Linnet. The computer applies orientation,
creates thumbnails and zoom levels, converts pixels to RGB565, and compresses
tiles. The iPod loads only the catalog pages and visible tiles it needs.

For normal use, prepare Photos in [Media Manager](media-manager.md). The original
images are preserved. Monochrome Q21 does not support photo playback.

## Prepare and inspect

Run from a configured source checkout. Replace the source directory as needed:

```console
python linnet photos compile --source path/to/photos --output out/photos.pimg --target photo4g-p98
python linnet photos inspect out/photos.pimg
```

The default maximum zoom dimension is 640 pixels. Add `--max-dimension 2048`
for more detail and larger output. The target supplies the initial viewport.
Compilation creates and validates a local package; publication is a separate
operation handled by Media Manager.

## Package structure

Host files use `.pimg`; the device reads `/.LINNET/PHOTOS/LIBRARY.PIM`.
The header starts with `LPIC`. Version 0 stores compressed tiles in the library
on the Photo 4G. Version 1 stores them in content-addressed packs under
`/.LINNET/PHOTOS/PACKS/` on the Video 5G and Classic 6G. Both versions use
little-endian integers and RGB565 words.

| Section | Purpose |
| --- | --- |
| 512-byte header | Counts, offsets, dimensions, file size, and checksums |
| Album and photo tables | Folder hierarchy, titles, image dimensions, and references |
| Resolution levels and tile index | Locate compressed prepared pixels without loading a whole image |
| UTF-8 string pool | Deduplicated titles and optional fallback paths |
| Payloads | 64 × 64 thumbnails followed by compressed 32 × 32 RGB565 tiles on Photo 4G; Video 5G and Classic 6G keep tiles in separate packs |

`/.LINNET/PHOTOS/GRID.PGR` contains host-rendered thumbnails at each target's
grid size and binds to the exact LPIC header. The iPod can display a prepared
grid page without scaling 64 × 64 thumbnails while browsing.

Limits include 4,096 albums, 65,535 photos, six levels per photo, one million
tiles, and a file smaller than 2 GiB. The compiler checks all size and offset
arithmetic. Optional baseline JPEG fallback references are bounded and explicitly
supplied; compilation does not copy those originals automatically.

## Validation and publication

Header, metadata, record, and pixel checksums protect against corruption.
Relationships, string boundaries, exact lengths, and canonical offsets must
validate before use. The device checks each thumbnail or tile before displaying
it. CRCs detect corruption; they are not authentication.

Publication verifies the host package and mounted device identity, retains the
previous library, writes and reads back a temporary package, then replaces the
active file. FAT replacement is not a power-loss journal; keep recovery receipts
when a write is interrupted.

The exact fields and validation rules live with their implementations:

- [Compiler and host validator](../tools/content/photos.py)
- [Firmware reader](../services/photos/pimg.c) and [record definitions](../services/photos/pimg.h)
- [Publisher](../tools/content/publish_photos.py)
- [Viewer and cache](../services/photos/photos.c)
