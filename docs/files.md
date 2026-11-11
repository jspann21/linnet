# Files

Home → Files browses the mounted FAT32 volume, including hidden and system
folders. Select opens the selected item; Menu returns to the parent. Folder
selection survives opening media and returning. Lists contain 16 entries per
page, with Previous page and Next page rows when needed.

Left on a file shows its byte size and choices to view the original as text or
bytes. Right in a folder switches to the current disk contents. Text/byte readers
use the wheel to scroll, Left/Right for the previous/next 480-byte page, Select
for file information, and Menu to return to the folder.

## Opening files

| File | Opening behavior |
| --- | --- |
| Supported audio | Opens in Now Playing using the native decoder and current sound settings. Menu returns to Files. The library queue is retained separately. |
| Prepared video source | Opens the existing Media Manager conversion, including Photo's IPVF copy. TV Out keeps its output choice. |
| Native video | Opens through the target's video backend; codec and container limits still apply. |
| Prepared image | Displays orientation-correct, fitted RGB565 pixels prepared by Media Manager. No image decoding or resizing runs on the iPod. |
| Unprepared JPEG | Uses the existing bounded baseline JPEG decoder on color targets: at most 256 KiB and supported decoder dimensions. |
| Prepared text | Shows host-decoded and wrapped ASCII text, including UTF-8 and BOM-marked UTF-16 sources. Wrapping fits the target's supported themes. |
| Other files | Opens a bounded text preview or byte viewer. Programs, archives, and unsupported document formats can be inspected; their contents are not executed or rendered as a native application. |

Image previews show the first frame of supported Pillow image formats. Files
previews are fitted to the screen; the Photos library provides zoom, rotation,
and slideshows. Monochrome Q21 has no image or video playback. Direct audio uses
transport, sound, and sleep controls; library-only queue/bookmark/favorite actions
are omitted for unindexed files. Playback of a directly opened file is not saved
as a library queue entry.

In Now Playing, Right advances to the next audio file in the source folder;
Left restarts after three seconds, or selects the previous audio file near the
beginning. Navigation crosses folder pages and skips directories and other file
types. If there is no neighbor, the current song keeps its playback state.

## Preparation and performance

Every Media Manager preparation builds a snapshot after media publication. The
computer traverses folders, puts directories first, sorts labels, resolves actual
FAT short aliases, classifies files, converts images, and wraps text. Image/text
conversion results are cached by source size, modification time, target geometry,
and text layout recipe. Original files are unchanged.

The host groups previews into immutable packs of at most 4 MiB, reducing file
creation and flushes during sync. It reuses existing packs and puts new or changed
previews in new packs, so a small update does not rewrite unchanged previews.
The device reads only the selected preview's range; it does not unpack the bundle.

The iPod performs no recursive indexing or sorting. It loads up to 4 KiB of
directory records, checks at most four record checksums per service step, and
reuses parent handles. Prepared image reads fill an existing frame buffer in
8 KiB chunks. Text/byte reads are at most 480 bytes. UI label callbacks perform
no storage I/O. Pending reads drain before buffers are reused; new browsing reads
yield when audio needs its playback buffer filled. Files supplies an executive
deadline so ready work need not wait for the normal idle interval.

Without a valid snapshot, Files enumerates one FAT directory sector per request
and retains 16 entries plus recent page positions. This mode uses disk order and
does not recursively scan subfolders. Older previous pages may require replaying
directory sectors. All reads retain storage epoch, bounds, chain, cancellation,
and checksum checks. The existing FAT layer may verify a file's chain on its first
read; measured hardware latency still depends on media, fragmentation, and audio
load.

The folder stack supports 16 levels below the root. The host index supports one
million records. Long display names are clipped to 127 bytes, while live entries
retain the underlying file handle and extension. Unrepresentable aliases fall
back to live browsing. Changed size/type or invalid index records also fall back
to live browsing. Same-size edits and newly added entries require preparation or
Right → Live; the snapshot does not monitor changes in the background.

Text conversion accepts sources up to 16 MiB and produces at most 4 MiB of
prepared lines; larger text remains accessible with bounded native paging. The
generated `.LINNET/FILES` directory is browsed live to avoid indexing successive
copies of the index itself. Previews are immutable and published before the new
index, with the previous index retained by the existing publication mechanism.

## On-disk formats

`/.LINNET/FILES/INDEX.FIL` has a 512-byte header: `FILS`, version 2, record count,
256-byte record stride, and CRC32 at byte 508. Record zero describes the root;
each directory points to its contiguous child records. Each record stores
attributes, source byte size, child range, prepared kind/size, fallback flags,
native media kind, a 128-byte label, a 14-byte short alias, a 76-byte prepared
path, and CRC32 at byte 252. Prepared kinds are image (1), text (2), and an existing
video conversion (3). Records and headers use little-endian integers.
The 16-bit field at byte 174 holds the preview's offset in 512-byte sectors
within its pack. Prepared size bounds that slice. Videos remain separate files
at offset zero.

Image previews use a 512-byte `FIMG` version-one header containing width, height,
payload CRC32, and header CRC32, followed by RGB565 little-endian pixels. Text
previews use padded ASCII rows of 20 columns on Q21 and 24 on color targets;
host font measurements may wrap earlier. Both row sizes divide the native
480-byte reading page.
