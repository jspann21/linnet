# Apple OS library updates

Linnet uses its own PODB music index. Media Manager can separately update Apple's
music/video database and photo library so selected folders are also available
when booting the original Apple OS.

Use [Media Manager's Apple OS workflow](media-manager.md#make-folders-available-in-apple-os)
for normal updates. Native Windows helpers are included in the packaged app and
built by `python linnet setup` in a source checkout.

## Supported boundary

Media Manager supports Apple library updates on the current Linnet iPod
families using a writable FAT32 volume:

| iPod family | Folders available in Apple OS |
| --- | --- |
| Fourth-generation monochrome | Music |
| Photo / Color | Music, Photos |
| Video | Music, Photos, Videos |
| Classic 6G, 6.5G and 7G | Music, Photos, Videos |

The app verifies the connected device and volume before writing. Classic also
requires its Apple device identifier for the library checksum; reconnect in
Apple Disk Mode if the app asks for it. Supported sector sizes are 512, 2048,
and 4096 bytes. The checks are in [apple_os.py](../tools/content/apple_os.py).
Compatible plain-text files under `/Notes` are read directly by Apple OS.

The managed writer stages and verifies updates, preserves existing Apple entries
and playlists, and retains backups. Selected folder contents remain in their
original locations. Apple OS support is narrower than Linnet's media support.

## Music path constraints

The current shared-library path accepts MP3 files beneath `/Music`.
It uses real FAT short aliases and a lowercase `.mp3` suffix in Apple's
database. The relative path below `/Music` must fit 23 characters. Long filenames
alone do not determine whether their aliases fit.

The normal update workflow reports unsupported paths without renaming originals
or rewriting FAT directories. Keep filenames and aliases unchanged after
preparing the libraries, or prepare and update them again.

See [managed music](../tools/apple/managed_music.c) and
[managed photos](../tools/apple/managed_photos.c) for the writers.
