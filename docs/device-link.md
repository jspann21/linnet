# Linnet Link

Linnet Link provides USB diagnostics, input, screenshots, RAM boot, and persistent
OS updates in **development and trace OS builds**. User OS builds exclude it.
Classic installation and recovery also use a separate temporary Link connection.
General library transfers use Disk Mode.

Run commands from the source environment described in the
[README](../README.md#develop-on-windows). Replace `PORT`, `DEVICE_SERIAL`,
and file paths with observed values; never use a remembered port or disk number
after a mode change.

## Everyday USB operations

Discover with `python linnet device info`, then handshake the listed Linnet USB serial:

```console
python linnet device info --serial DEVICE_SERIAL
```

Everyday Link commands accept either `--serial DEVICE_SERIAL` or `--port PORT`.
Prefer the serial when Windows may assign a different port after reboot.
Prefix the following commands with `python linnet device`:

| Operation | Command | Result |
| --- | --- | --- |
| Temporary image | `boot --serial DEVICE_SERIAL --image path/to/os.los --wait --library` | Runs in RAM; verifies the exact image, new boot generation, OS health and music library readiness. |
| Persistent update | `install --port PORT --image path/to/os.los` | Publishes an A/B candidate; requires stopped media and prepared storage. Does not reboot. |
| Boot installed OS | `reboot --port PORT --mode persistent` | OS saves state, stops media, drains storage, and reboots. |
| Restart RAM image | `reboot --port PORT --mode os` | Restarts the retained RAM candidate. |
| Recovery Boot | `reboot --port PORT --mode boot` | Enters recovery when available. |
| Native Disk Mode | `reboot --port PORT --mode disk` | Enters USB mass storage; rejected while storage is busy. On-device Sync saves and stops first. |

Without `--wait`, boot reports acceptance. After any other transition, verify with:

```console
python linnet device wait-ready --serial DEVICE_SERIAL --image path/to/os.los --library --timeout 60
```

`--image` requires the exact running build and SHA-256; omit it to accept any
healthy development OS on that serial. `--library` additionally requires phase 3,
error 0. Readiness waits only repeat read-only discovery and status requests.
The timeout defaults to 60 seconds, with a 600-second maximum. A User OS image
will not expose Link. Recovery Boot needs prepared, idle storage before persistent
reboot. RAM boot and install require the image's adjacent device-bound build report.

## Disk Mode and refresh sessions

Native Disk Mode is available on the four current targets. Link is unavailable
while the disk is mounted. Use Media Manager for normal eject, or identify the
mounted iPod explicitly:

```console
python linnet device detect-retail --mount X:/
python linnet device eject --disk-number DISK_NUMBER --expected-serial STORAGE_SERIAL --wait-link --link-serial LINK_SERIAL
```

The placeholders must come from the current discovery. Storage and Link serials
can have different representations. Native eject flushes and returns to Linnet.
`--wait-link` allows up to 60 seconds for a handshake; use `wait-ready` afterward
when the running image, health or library matters.

When using **Apple Disk Mode**, a plan-bound directory refresh may be required
before eject. Prepare that return path before switching modes:

```console
python linnet device refresh-session prepare --plan path/to/installed-plan.json --plan-sha256 PLAN_SHA256 --hours 8 > out/refresh-session.json
python linnet device refresh-session start --prepared out/refresh-session.json --allow-uac
python linnet device refresh-session status
python linnet device eject --disk-number DISK_NUMBER --expected-serial STORAGE_SERIAL --reboot --plan path/to/installed-plan.json --plan-sha256 PLAN_SHA256
python linnet device refresh-session stop
```

The finite elevated helper accepts only its reviewed operations. A new plan needs
a new session. Plain eject from Apple Disk Mode may leave the device in
Disk Mode with **No Media** reported by Windows. Retain the matching backup,
plan, and receipts; see [local artifacts](local-artifacts.md).

## Diagnostics, input, and screenshots

| CLI command, followed by `--serial DEVICE_SERIAL` (or `--port PORT`) | Use |
| --- | --- |
| `ui-state` | Scene, selection, lifecycle, and paint state |
| `library-state`, `player-state` | Library readiness and playback state |
| `player-timing-state` | Decoder and PCM timing |
| `photos-state`, `video-state` | Media state and counters |
| `startup-timing`, `video-startup-timing` | Startup milestones |
| `boot-timeline` | Retained Boot and OS stage timings |
| `media-info` | Storage identity, power, and error state without waking the drive |
| `platform-state`, `executive-state` | Input, display, queue and service counters |
| `storage-performance`, `storage-schedule` | Storage latency, workload and scheduling |
| `render-timing`, `ata-capabilities`, `power-state` | Rendering costs, cached controller capabilities and power samples |
| `crash-info` | Retained crash record, or null if absent; leaves the record intact |

Platform and media diagnostics require the development OS. Boot exposes identity,
update and recovery services, including `crash-info`. Compare performance
counters on the same image, boot generation and counter epoch; saturated counters
are unusable, and overlapping timings must not be added.

Send input and capture its result without maintaining a Python script:

```console
python linnet device input --serial DEVICE_SERIAL --wheel 1
python linnet device input --serial DEVICE_SERIAL --button select
python linnet device screenshot --serial DEVICE_SERIAL --output out/screen.png
```

`input` reads the current boot generation and last input sequence, sends once,
and waits up to five seconds for processed events and completed UI presentation.
Buttons are `menu`, `play`, `next`, `previous`, `select`, or `release`; a press
includes its release. Wheel values range from -64 to 64. These are synthetic
events; physical controls still need observation. Do not repeat Play while priming.

Screenshots freeze the native framebuffer and verify transfer offsets and CRC
before writing the PNG. They prove framebuffer contents, not the physical LCD.
[Session](../tools/link/session.py) also provides bounded storage APIs and raw
diagnostic records. `READ_LOG` is binary LCD/USB/recovery diagnostics, not a text log
stream. Advanced input uses increasing event sequences; reconnecting does not
reset them. Button masks are defined in [kernel/shell.h](../kernel/shell.h).

## Updates and uncertain results

Storage APIs operate on fixed files, not arbitrary paths or sectors. Uploads are
bounded and contiguous; the host verifies image identity and target compatibility.
Hashes provide integrity checks, not signature authentication.

A missing, corrupt, extra or mismatched acknowledgement poisons the session and may
raise `AmbiguousCommand`. Close it, rediscover, and inspect state before deciding
what to do. Never automatically replay input, reboot, crash-clear, or a write.
An interrupted transaction also poisons its session.
Offset-based upload resume is not implemented.

Persistent OS updates can re-admit an explicitly rejected CHUNK or FINISH within
15 seconds. The client first requires a matched single ERROR with no extra bytes,
then checks the original running identity and a clean, active updater at the
unchanged offset and expected slot. It refreshes the read-only storage proof when
needed. An advanced offset, failed proof, write fault, or uncertain acknowledgement
stops the transaction. BEGIN is never replayed.

## Existing device checks

- `platform-cycle`: input, framebuffer, presentation, sleep, and wake checks.
- `storage-cycle`: readback, settings, bounded file growth, and injected faults.
- `update-cycle`: candidate confirmation, rollback, and publication cuts.
- `storage-sync-cycle`: guarded Disk Mode roundtrip and return to the expected OS.

The host sync check reads partition LBAs using the reviewed USB sector size,
including factory 2048-byte Video layouts. It rediscovers the same logic-board
serial after the mode change and pins the fresh disk and volume identity for
file writes and readback. Native Disk Mode returns through its own safe eject;
Apple Disk Mode requires the matching directory-refresh session or an elevated
process. The check never requests UAC automatically.

Use each command's `--help` for required images, scenarios, and identities.
Receipts go under `out/runs`. Injected software faults do not qualify electrical
failures; physical controls, sound, current, and panel quality need observation.

## Protocol reference

Frames contain a 20-byte little-endian `LINK` header, at most 1,024 payload bytes,
and a four-byte CRC32 over header and payload. Reserved fields must be zero.
Acknowledgements echo the request command and sequence. Boot, USB, input, scene,
and storage generations have different purposes and are not interchangeable.

Use the implementation for exact command IDs and record offsets:

| Contract | Source |
| --- | --- |
| Framing and upload | [Host protocol](../tools/link/protocol.py), [firmware link](../hal/link/link.h), [dispatcher](../hal/link/dispatch.h) |
| Device identity | [Host identity](../tools/link/identity.py), [binary formats](formats.md) |
| Input and capture | [Platform Link](../kernel/platform_link.h), [Session](../tools/link/session.py) |
| Storage and updates | [Storage Link](../kernel/storage_link.h), [storage owner](../services/storage/storage.c) |
| Media records | [Host decoders](../tools/link/music.py) |

Classic development OS builds accept `AUDIO_STATE` with a little-endian
`u32(1)` payload for a read-only codec snapshot. The 64-byte reply contains
`AUDC`, a successful-read flag, the 32 bytes at CS42L55 addresses `01h..20h`,
then six words: I2S clock control, transmit configuration, transmit command,
clock divider, system audio clock, and codec-ready state. The I2C read has a
20 ms deadline. An ordinary empty `AUDIO_STATE` request is unchanged.

Release verification checks that developer transport, injection, capture, and
related services are excluded. Keep ACK retirement, DMA quiescence, bounded
waits, and buffer ownership intact when changing firmware transport code.

## Windows loses the iPod during Disk Mode

**Device Descriptor Request Failed / Code 43** means the endpoint is not normally
enumerated. Stop I/O, retain the receipt, and distinguish completed writes from
uncertain ones. Do not replay a write or bypass its guard to make eject succeed.

Reconnect/reset the affected iPod, then rediscover its identity. The finite
refresh helper also supports reviewed tracing and device restart actions; inspect
`refresh-session --help`. Do not reset a shared USB hub.
