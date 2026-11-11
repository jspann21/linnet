# Architecture

Linnet is freestanding native firmware. Host tools prepare media and compile
static TSX layouts to C. The device uses fixed memory budgets, bounded service
work, and a native renderer; there is no JavaScript runtime.

## Source map

| Path | Responsibility |
| --- | --- |
| `boot/` | Image validation, slot selection, recovery, and handoff |
| `kernel/` | Executive, deadlines, and service integration |
| `hal/api/` | Portable hardware interfaces |
| `hal/<family>/`, `hal/codecs/` | SoC implementations and audio codec drivers |
| `targets/` | Hardware manifests, board wiring, display adapters, and policies |
| `services/` | Storage, playback, settings, games, and other native services |
| `ui/`, `themes/` | Native renderer, built-in screens, and theme definitions |
| `tools/` | CLI, build system, media preparation, Link, and simulator |
| `sdk/`, `scenarios/` | Authoring templates and existing verification scenarios |
| `third_party/` | Imported codecs, notices, and provenance |

Built-in screens, including the optional UI Demo, live under `ui/system/`.
Custom firmware screens can be authored under `apps/<name>/`, which the content
compiler also discovers. These screens are compiled into the OS; see the
[UI SDK](../sdk/README.md) for templates and menu integration.

Targets select their HAL, memory layout, capabilities, and presentation through
manifests. Photo, Video, and monochrome 4G use PP502x; Classic uses S5L8702.
See the [porting guide](../targets/README.md) for source resolution and validation.

Optional runtime power policies consume workload hints through
`hal/api/power_policy.h`. The executive reports pending work and audio headroom;
the selected target owns frequency thresholds and fallback decisions. Classic
selects its implementation in `targets/classic6g-n25/SOURCES` and enables it in
that directory's `hardware.h`. Register transitions remain in the S5L8702 HAL.
See [Classic runtime power](classic-power.md) for the policy and its diagnostics.

## Boot and updates

Boot validates the image's target, bounds, and integrity before entry. CRC and
SHA-256 detect corruption; they do not provide signature authentication.

Persistent OS updates write the inactive slot and publish redundant state.
A candidate is confirmed after memory, services, storage, UI, and audio readiness.
An unsuccessful candidate falls back to the confirmed image; corrupt state
enters recovery. RAM boot is temporary and does not install an image.

M25 OS checksum work uses shared code and tables in internal memory after a
complete copy and readback check. Its byte fallback reads the immutable SDRAM
load copy, including before initialization or after failed verification. Both
paths retain complete checksum coverage and progress boundaries.
Development screenshot checksums share the same immutable byte table while
retaining their existing single-pass pixel copy and checksum order.

Native installation preserves the original Apple firmware for the MENU startup
chooser. The loader checks its target, length, hash, and configuration before
handoff. Boot installation is separate from an A/B OS update and uses its own
backup-bound procedure. Classic's NOR bootstrap differs from the PortalPlayer
directory installer; see the [bootstrap map](bootstrap-tooling.md).

Startup code depends on target-specific RAM initialization, CPU/COP ownership,
reserved memory, and quiescent DMA. An image passing container checks does not
prove those hardware conditions.

Development and trace images include Link and diagnostics. Release builds remove
the developer transport, injection, screenshot, and crash services; the release
verifier checks their exclusion.

## Runtime and media

The executive schedules bounded work around audio, input, and watchdog deadlines.
Storage retains buffer ownership until completion or drained cancellation.
Filesystem validation and write barriers remain active during media work.

Before idle, PP502x rechecks the wake deadline and pending interrupts. It reuses
an earlier timer only while that timer and its CPU interrupt are enabled and
the deadline is still in the future.

M25 overlaps a bounded wake of known storage with muted file and header preparation.
Audible startup and resume wait for fresh disk readiness, including any power
transition; paused preparation and decoded EOF retain their memory-only path.
The reservation ends after preparation, restoring the normal storage idle policy. A
full muted PCM ring yields to storage deadlines while wake completes.
Rotating-media compressed caches keep their sequential fills during preparation
and while the PCM buffer has sufficient audio. If the active playback feed falls
below half its background reserve, a required decoder region can use the next
free read admission before the remaining speculative fill. An admitted chunk
still completes or drains with its original buffer; validated cache prefixes
remain available and filling resumes after the demanded read retires. This
priority does not change cache capacity, next-track thresholds, or drive idle
policy.

| Service | Runtime approach |
| --- | --- |
| Music | Native decoders feed a PCM ring and DMA; metadata and next-track preparation share a bounded work budget. |
| Photos | Host-prepared RGB565 tiles, paged metadata, and fixed caches; bounded JPEG fallback where available. |
| Video | IPVF on Photo, BCM2722 video on M25, and the native MP4 backend on Classic. Preparation and limits follow the target. |
| Files | Host-sorted directory pages and prepared previews; M25 retains validated pages and their live folder handles for repeat navigation. |
| UI | Immutable compiled scenes, baked fonts, runtime state, and bounded rendering. Target/profile layouts use the appropriate display geometry. |
| Settings | Validated records and redundant publication; rendering does not write storage. |

Music verification also captures song titles and, on targets with resident
metadata, group names under the existing title-cache budget. Extra group names
require room for the whole string pool and the host-built browse index. Browser
labels and group members that are resident can be resolved before drawing;
larger libraries retain the asynchronous lookup path.
On M25, verification can retain the remaining raw group metadata in an unused
STAGE tail. That cache publishes only after the complete database hash and
browse index pass, and firmware upload revokes it before reusing STAGE.
Resident search labels and metadata then resolve without waking storage.
Search group navigation on M25 prepares its retained twelve-row window
from resident members before drawing. It validates the group once per bounded
preparation slice and reuses those bounds for each member. Partial cached work
continues in another slice before admitting a metadata query; cache misses retain
the asynchronous path and playback keeps its existing work budget.
Completed Search windows skip repeated row preparation and readiness scans.
Result, viewport, and library changes invalidate that readiness; admitted queries
still drain before closing or returning. Group-title queries also resolve verified
resident members through the ordinary query owner before parsing database records.
Completed Play Queue windows also skip repeated library-identity work after
pending queries and reloads drain. Scrolling retains complete cached labels with
bounded bulk copies; traversal order preserves rows when the windows overlap.
Music browser rows retain their full label capacity and use an invalid ordinal
to mark an empty cache slot. Navigation and library changes invalidate all rows,
and asynchronous results still require the current navigation epoch.
Favorites and Bookmarks that a fully verified history summary proves empty can
show their final empty state on entry. Most Played uses the same proof when no
play counts exist. Unverified or nonempty history keeps its bounded scan; recent
history, pending reads, and error handling retain their normal paths.
Jump to letter also consumes ready labels before its first frame, within a
250-microsecond slice. Missing labels retain the existing asynchronous lookup;
long scans yield between slices and preserve reverse-order and cancellation rules.
M25 database verification reads through a bounded 16 KiB window in the existing
page cache. Logical hash and metadata-capture slices retain their original order
and bounds; the window reduces physical reads without skipping verification.
Cover Flow can use the unoccupied title-cache tail for additional complete
album records and previews. Library reload and cancellation drain its reader
before reusing that memory. M25 stops its background catalog scan when every
record is validated and resident; larger catalogs retain paged warming.
Video can retain all native artwork as lossless LZ4
blocks when the set fits its artwork banks and an unused memory lease. A 64 MiB
device can lend the unused extended metadata tail; a 32 MiB solid-state device
can lend inactive audio read windows, excluding decoder and PCM storage. Upload
admission drains and releases that STAGE lease before writing firmware, and
prevents artwork from reclaiming it until reboot. Visible cards are pinned during
draws; larger libraries retain the bounded native working cache.

Host-prepared packed artwork includes a separate index checksum. M25 validates
that index in bounded steps and can decode visible covers from a completed
prefix while the remaining artwork warms. Each early cover still passes
its own checksum; complete payload and index validation precede publication of
the resident set.
Packed blocks within one resident bank decode directly from that bank; blocks
crossing a bank boundary use the existing input scratch buffer.
When decoded RGB storage is available, M25 retains ten covers, using three
additional slots in texture workspace freed by a shared raw decode buffer.
Playback scratch, compressed input, Files and history allocations stay disjoint.
The completed sealed-index proof remains valid while later reads fill the
remaining payload. Final publication reuses that proof and still checks the
complete payload checksum and saved index geometry.

Small raw-only libraries on a qualified 64 MiB M25 can retain decoded covers
directly by artwork ID in the unused high artwork array. Admission preserves
the winning low-bank Files region after the existing history loan. Normal
bounded visible preparation expands validated tiles; lookups check library
identity and mount epoch. Upload, reload and failure revoke the cache.

Once lossless artwork is resident, Cover Flow prepares its first visible decoded
cards in bounded background steps rather than waiting for the first open. A
ready view also acknowledges its revision before the first transition paints,
avoiding a duplicate settled frame while preserving ongoing animation deadlines. A
settled M25 frame uses host-prepared visible spans for all library edge cases,
so card overlap is resolved during compilation. Fractional animation keeps the
dynamic splitter; both paths clip spans before writing the framebuffer. A
resident video list also fills during an already awake, idle disk window, using
at most 32 rows per read and stopping before any published page. Background
catalog reads do not own playback navigation or block firmware upload. Shutdown
still drains them. Prefetch checks ATA and block power transitions before each
admission and never wakes a sleeping disk on its own.
Resolved empty video lists also suppress the playback wake reservation.
M25 native video startup can reuse validated AAC bytes already read during the
media scan, transferring them into an empty playback cache after ownership and
bounds checks. This removes a duplicate read without adding a buffer.
Completed BCM file-service responses copy up to 4 KiB per service call before
the first LCD video picture and up to 512 bytes during steady playback,
stopping, recovery, and TV output. Ring reads retain their 128-byte limit. The
complete response must fit before copying, and its producer pointer publishes
only after every fragment is copied.

Files keeps two guaranteed cache pages in the unused M25 artwork-bank tail when
Cover Flow is enabled. A fully loaded resident video catalog can lend its aligned
unused tail for more pages. On a verified 64 MiB M25, the original metadata
reservation can lend a larger cache while the music database uses extended RAM.
Every borrowed-memory access checks its owner's ready state and mount epoch;
revocation drops the pointer without touching returned bytes. Files DMA stays in
its private page buffer and only completed pages copy into the cache.
A page enters the cache only after its record checks complete;
folder handles also bind the live parent cluster and catalog record identity.
Each cache hit checks the current mount epoch. Remounts, malformed-page fallback,
and explicit Live refresh invalidate the retained pages. Ready pages publish
their selection and content together, avoiding a duplicate redraw; asynchronous
loads still restore selection after the row count becomes known.
On M25, the first Files page also prepares while Home is visible and the disk
is already awake. Hidden preparation pauses for playback or other storage owners
and never scans live directories when a host index is missing or corrupt. Pending
reads drain before cancellation reuses their buffers. Entering Files binds the
prepared page before the transition's first frame.

Visible M25 activity scans can borrow spare artwork memory for up to 32 pages
from each history bank while playback is stopped. Admission preserves the full
512-page Files allocation; the first history window and ordinary record lookups
keep their existing small reads. DMA stays in the static 16 KiB scratch buffers,
and only completed reads copy into the borrowed windows. View changes, firmware
upload, and storage recovery revoke the loan; admitted DMA drains before its
scratch buffers are reused. Playback prohibits further grouped read admissions.
Every history page still passes its normal bank,
CRC, and record checks. Empty pages first prove their zero tail before using
precomputed CRC advancement constants.

The simulator executes the same native renderer and compiled UI definitions.
It checks layouts and pixels, not physical timing, audio, USB, or power.

Next: [SDK](../sdk/README.md), [storage](storage.md),
[binary contracts](formats.md), [media formats](music-format.md), and
[Linnet Link](device-link.md).
