# Video formats

Video preparation depends on the target:

| Target | Playback path |
| --- | --- |
| Photo P98 | Prepared IPVF with RGB565 frames and IMA ADPCM audio |
| Video M25 | H.264 Baseline or MPEG-4 Simple Profile through BCM2722, with AAC audio |
| Classic N25 | Native MP4 backend with H.264 and MPEG-4 playback |
| Monochrome Q21 | Video disabled |

Use [Media Manager](media-manager.md) to select the appropriate preparation path.
Native video must satisfy the target's codec, profile, dimensions, rate, and
container limits; an MP4 extension alone is not sufficient.

## IPVF

IPVF host files use `.ipvf` or `.ipv`. The device catalog is
`/.LINNET/VIDEOS/INDEX.VID`; prepared entries use the `.IPV` extension.

The sector-aligned stream contains a header, bounded metadata, audio/video
records, and a keyframe seek index. Video supports raw/LZ4 keyframes and temporal
updates; audio supports stereo, mono, and silence. RGB565 frame words are
little-endian. IPVF uses an MSB-first CRC32 without a final XOR, unlike the usual
Linnet record CRC.

```console
python linnet video compile --source path/to/input.mp4 --output out/video.ipvf --target photo4g-p98
python linnet video inspect out/video.ipvf
```

These are placeholder paths. FFmpeg/FFprobe are required; `--ffmpeg` and
`--ffprobe` select explicit executables. `--fps` accepts 4–30 within the
applicable target limits.

## Native MP4

The Video M25 and Classic N25 CLI preparation paths produce compatible MP4/M4V
output. Select the target with `video compile --target`; native inspection also
requires the target:

```console
python linnet video inspect path/to/prepared.mp4 --target video5g-m25
```

[M25 video notes](../targets/video5g-m25/NATIVE_VIDEO.md) describe its controls
and hardware boundary.

## Implementation

- [IPVF encoder](../tools/content/ipvf_encode.py), [validator](../tools/content/ipvf_validate.py), and [firmware player](../services/video/ipvf/player.c)
- [Native host preparation](../tools/content/native_video.py) and [MP4 reader](../services/video/mp4.c)
- [Catalog](../tools/content/video_catalog.py) and [publisher](../tools/content/publish_video.py)

The validators check bounds, indexes, record chains, checksums, and supported
decoding before publication. Keep original media and recovery receipts when an
update is interrupted.
