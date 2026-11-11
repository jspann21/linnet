# Apple library helpers

[Media Manager](../../docs/media-manager.md#make-folders-available-in-apple-os)
uses these Windows helpers to prepare staged Apple library updates:

- `managed_music.c`: updates Apple music/video databases while preserving entries and playlists.
- `managed_photos.c`: creates Apple photo data.
- `managed_io.h`: shared Windows filesystem and helper compatibility.

Run `python linnet setup` from the repository root to build the helpers.
The [builder](../studio/apple_tools.py) downloads pinned dependencies, verifies
them, and supplies the packaged app's helper distribution. Media Manager validates
the device and publishes the verified staged output.

See [Apple OS library updates](../../docs/apple-stock-music.md) for supported
formats and path limits.
