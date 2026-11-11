# Binary contracts

Use this page to find the owner of a binary format. Exact field offsets, accepted
identifiers, lengths, and limits belong with the encoder and reader, rather than
a second handwritten specification.

Most records use little-endian integers and CRC32/ISO-HDLC. Each reader enforces
its own reserved fields and bounds. IPVF also stores little-endian RGB565 pixels
but uses a different CRC; see its [format guide](video-format.md). Do not assume
all headers have the same version field or that every reserved field is a version.

## Firmware and state

| Record | Purpose | Definition and reader |
| --- | --- | --- |
| `LNOS` / `.los` | 128-byte image header followed by an ARM payload; target, profile, entry, length, build identity, and CRCs | [Boot API](../boot/include/linnet_boot.h), [host image inspector](../tools/inspect/image.py) |
| `BOOT`, `CRSH`, `LNID` | Boot selection, development crash mailbox, and device identity | [Boot API](../boot/include/linnet_boot.h), [host identity](../tools/link/identity.py) |
| `STAT` | Redundant 512-byte state copies with generation, bounded payload, and CRC | [State store](../services/state/store.c) |
| `UPDT`, `HAND`, `RLBK` | Persistent image slots, retained handoff, and failed-candidate information | [Update owner](../services/update/update.c) |
| `SETT`, `THMS` | User settings; version-six THMS shares theme, scores, brightness and menus in the existing A/B theme records | [Display settings](../services/display/settings.c), [shared host preferences](../tools/studio/device_preferences.py), [storage owner](../services/storage/storage.c) |
| `MENU` menu envelope | Stable version-three menu IDs, order and shortcuts, stored in shared THMS preferences and accepted as simulator input | [Host menu settings](../tools/studio/screen_settings.py), [device menus](navigation.md#saved-customization) |
| `HIST`, `HPGE` | Redundant paged playback history | [History records](../services/music/history_record.h), [page owner](../services/music/history_pages.c) |
| `MST3`, `QUEU`, `PROF` | Music queue and sound state | [Music settings](../services/music/settings.c), [queue](../services/music/queue.c), [profile](../services/music/profile.c) |

State generations order publications; they are not product versions. A valid
checksum does not make an incompatible layout acceptable. Build identity,
payload checksum, and full-image hash have distinct meanings.

## Content and transport

| Format | Guide or implementation |
| --- | --- |
| `PODB` music index | [Music library](music-format.md) |
| AAC seek checkpoints and FLAC seek metadata | [AAC producer](../tools/content/aac_index.py), [AAC/M4A reader](../services/audio/aac/m4a.c), [FLAC producer](../tools/content/flac_index.py), [FLAC reader](../services/audio/flac/flac.c) |
| `LNGM` / `.LGM`, `LNGC` game catalogues | [Package encoder](../tools/content/game_package.py), [native validator](../services/games/package.c), [loader](../services/games/manager.c), [authoring](game-authoring.md) |
| `GMSAVE0.BIN` / `GMSAVE1.BIN` | Retained identity/schema-bound `STAT` records in fixed cells: [publisher](../tools/content/game_install.py), [save owner](../services/games/saves.c) |
| RGB565 artwork, `LCF2` / `LCFN` Cover Flow | [Album artwork](artwork.md) |
| `LPIC` prepared photos | [Prepared photos](photo-format.md) |
| `IPVF` and native MP4 video | [Video formats](video-format.md) |
| `FILS` directory index and `FIMG` previews | [Files formats](files.md#on-disk-formats) |
| `NOTE` notes and `LDSP` DSP profiles | [Notes compiler](../tools/content/notes.py), [DSP compiler](../tools/content/dsp.py) |
| `FONT`, `LNUI`, `APPL`, `THME`, `CNTN` | [UI compiler](ui-compiler.md); these are host artifacts |
| `LNFONT1` / `PODIUM.FNT` | Optional locally extracted Podium Sans: [producer and host validator](../tools/content/apple_font.py), [native validator](../ui/runtime/ui.c), [startup reader](../services/display/settings.c) |
| `LINK` USB frames and diagnostic records | [Linnet Link](device-link.md#protocol-reference) |

When changing a format, update its producer, validator, device reader, and
publication path together. Check truncation, overflow, references, and unknown
values. Unsupported layouts are rejected. Hashes and CRCs provide integrity,
not authentication.

The optional font is exactly 8,570 bytes, little-endian: an eight-byte
`LNFONT1\0` magic, six 32-bit values (width 15, height 17, advance 14, first
character 32, count 95, packed alpha bytes 7,678), 95 one-byte advances, one
zero byte, 95 eight-byte glyph records, packed alpha coverage at byte 888, and
a CRC32 of all preceding bytes. Glyph records contain a 32-bit coverage offset
and one-byte x, y, width, and height. The reader bounds every glyph and keeps
the validated buffer for the session. Missing, truncated, incompatible, or
damaged files leave the included font active.
