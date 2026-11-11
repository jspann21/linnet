# Native M25 video

M25 plays H.264 Baseline and MPEG-4 Simple Profile through BCM2722, with AAC audio
decoded on the ARM. Required vendor resources come from the device's verified
firmware backup and remain outside the source tree.

The shared [MP4 reader](../../services/video/mp4.c) validates the container.
BCM transfer, display ownership, resource selection, and recovery are target
responsibilities. Board revision determines display/runtime configuration.

## Host preparation

The target manifest requires host normalization to H.264 Baseline at the display
size. The encoder disables in-loop deblocking for BCM2722 playback.
This setting must be applied while encoding;
changing slice flags in an existing stream would change its reference pictures.

Compatible MP4 inputs are also normalized. Prepared copies remain cached on the
computer, and a changed target profile invalidates older preparations. Selecting
no conversions leaves source videos needing preparation; it does not index them
as prepared output. Container inspection reports decoder limits separately from
this preparation policy. One-second MP4 chunks reduce index and storage work on
the iPod.

## Playback

Native video owns the display. Play or Select pauses; Left/Right seeks to a
keyframe; the wheel changes volume; Menu exits. Seeking aligns audio to the
selected keyframe's timestamp rather than promising exact-frame access.

The player prioritizes audio and drains its final block at EOF. On a prepared
M25 installation, the verified native display runtime starts once before Home.
The UI and movies then share that runtime, avoiding a display reset at every
video start. A separate native UI layer keeps the selected video's startup
message visible until the selected frame has a compositor acknowledgement,
including after a saved-position seek. Audio waits for that same acknowledgement.
Video startup does not turn the backlight off.
The first selected frame already shows "Starting video...", including time
spent draining prior media work. Its dim theme background remains visible;
there is no initial empty black frame while waiting to admit the video open.

The native UI uses two independently allocated RGB565 surfaces and the existing
ARM framebuffer. Only the inactive surface receives pixel uploads. Presentation
swaps surfaces after the update acknowledgement. Partial uploads combine the
previous committed frame's original damaged rows with the current frame's
damage, matching the two-buffer scheme in the device's RetailOS. Full frames
retain the existing transfer size; small updates can repair an additional prior
row span. The second surface costs 153,600 bytes of coprocessor memory.

Stopping waits for a fresh native stream-stop request and its published host
reply, then verifies idle. LCD playback can retain the idle application shell
after ARM audio and I/O drain, all file handles close, and both native file and
stream rings remain empty for ten milliseconds. The guarded native context
must report idle with zero submitted/completed/selection counters and a null
current-frame pointer; stopped codec frame arrays are never dereferenced.
The proof has a 250 ms bound before falling back to unloading the exact task.
Transport or task-identity failure uses display recovery instead.

A repeat LCD start rechecks status, the full task token, zero/null display
state, empty rings and mount binding before skipping application loading.
Stream selection, codec setup and the current-frame presentation gate still
run. Storage recovery revokes reuse even if an epoch repeats. An old task token
is revalidated before unloading, including cancellation before selection.
TV starts unload a retained LCD shell and retain their existing full lifecycle;
TV output disables its DAC before releasing ownership.
An early cancellation that loaded an application but never sent selectplay
unloads that known task directly; no stream-stop handshake exists in that state.
The UI layer returns through the same native display runtime. A bounded failure
path restores the verified NOR display runtime. Handoffs unload a retained idle
shell with a bounded acknowledgement before restoring that original runtime.

Every start freshly checks the complete selected NOR image extent. Its
512-byte checksum slices run in batches of at most four; a 250 microsecond
work check stops admission of another slice. The checksum result and timeout
checks remain unchanged, and the next startup phase waits for another turn.

The video and audio MP4 opening scans may advance up to eight cached parser
transitions per turn, checking a 250 microsecond work budget before continuing.
Any existing or newly staged read ends the batch, including a completed read
that was pending when the call began. Cancellation and ownership changes discard
the old scan result. Parsing, validation and the file format remain unchanged.

Before the first LCD picture, completed BCM file-service responses copy in
fragments of up to 4 KiB per service call, matching the existing video-payload
bus bound. Playback after that picture, including silent clips and later seeks,
retains 512-byte file-service fragments. TV output, stopping and recovery also
keep those smaller fragments. Initial LCD stream metadata uses 512-byte writes.
Ring reads stay limited to 128 bytes. Space is checked for the
complete response, and its producer pointer publishes only after all fragments
are copied. Normal playback prepares its PCM reserve while the native runtime
starts. After the display-region command completes, playback waits for that
reserve instead of an unconditional two-second settle. Running, non-buffering
status permits checking the first picture; it does not by itself start audio
or remove the startup message. The revision-specific check validates the full
application task token, bounded native allocator extents, video descriptor and
frame-array membership, completed update counters, and current stream sequence.
The selected PASSTHRU admission path increments the host sequence once, so the
comparison converts it back before checking the saved-seek floor and published
frame ceiling. Repeated reads reject changing task, descriptor and frame state.
This orders UI removal after the current movie object enters the compositor.

Checks run only during the picture handoff, at most once per 10 ms, with an
absolute five-second timeout and normal display restoration on failure. File,
stream and audio preparation continue between checks. An unfinished picture
requirement survives pause/resume during a seek. Audio and its playback clock
start after the acknowledged frame, avoiding a soundtrack that advances during
native decoder startup. Ordinary pause/resume after a committed frame retains
its existing status synchronization.
Preparation yields extra audio decoding once the startup reserve is ready so
the native stream can produce its first picture. Running playback retains its
normal, larger audio buffer target. For normal playback below 44.1 kHz, startup
and seek reserves preserve at least the original 32,768 / 44,100 second duration,
rounded up to a whole 1,024-frame AAC block. Higher rates retain 32,768 frames;
steady playback retains its 49,152-frame target.

Rotating storage wakes asynchronously when video preparation begins, even if
all initial reads hit RAM. Playback waits for drive readiness before `mp_play`,
so a later cache miss cannot spend the PCM reserve on a cold spin-up. The wake
is attempted once per playback; stopping releases the existing standby hint.
Once the startup PCM reserve is ready and no transfer or control is runnable,
startup status and picture waits use the existing one-millisecond ring polling
interval. Their scheduling threshold matches the smaller startup reserve.

Native firmware and read-through decoder-library bytes borrow an idle music
decoder workspace. The lease ends before returning to idle UI or music. Only
completed reads from the five qualified immutable native libraries enter the
cache; no extra disk reads run to populate it. Byte coverage and file metadata
bind the current mount, and music decoder reuse invalidates coverage. Accepted
storage recovery explicitly invalidates it even if a new FAT mount reuses an
epoch value. Cached responses retain the same complete-response publication
and bounded BCM transfer rules.

Idle native UI does not poll display rings without pending presentation work.
Its native power policy is `min`; a foreground development query can inspect
native power status.

## References

- [Original firmware research](https://github.com/giek2000/ipod-classic-firmware-research)
- [Resource image analysis](https://github.com/siggifly/ipod-emulator/blob/dev/research/10-the-resource-image.md)
- [VideoCore runtime analysis](https://github.com/siggifly/ipod-emulator/blob/dev/research/11-the-videocore-runtime.md)
- [openVC02](https://gitlab.com/cmdly/openVC02)

Protocol observations must be checked against the device's resources.
Emulator responses are not hardware evidence.

## Fully resident HDD playback

The optional 64 MiB clean-cache lease can keep a fully resident LCD video on
RAM reads while the HDD stays asleep. Admission reuses an owned exact path and
complete handle captured only after an ordinary OPEN succeeds in the current
exclusive mount. A cache hit never renews that provenance. A replacement that
fails or is canceled before its first picture, a direct-file override and
accepted storage invalidation clear it. FAT mutations retire epochs before
writing. Directory bytes need not remain
in RAM, matching the existing authority of a current-epoch opened handle.
Admission independently proves every published data sector, full mirrored
FAT sector and exact file-chain end. It issues no
disk reads to qualify a candidate. Missing bytes, an occupied owner or the
20 ms admission budget select the existing early-wake path.

After MP4 parsing, MPLAYER, PASSTHRU and the selected H264DEC or MPG4DEC must
have complete byte coverage in the existing leased firmware workspace, bound
to their complete previously opened file identities, live mount and expected
sizes. This avoids requiring unused raw library-file sectors in the physical
movie cache. The admitted resource mask is published after all three prove
complete. Unexpected native library opens fail before either the
resource-cache fast path or a disk open. AAC decoding remains on ARM. VMCS
must already be verified in its current leased music workspace.

The first uncached native-library read during LCD startup may fill that file's
existing workspace allocation in asynchronous chunks of at most 16 KiB. AAC
can run between chunks; it does not share a pending preload's storage owner.
Exact successful completion and a fresh ownership check precede coverage
publication. Warm reads perform no preload I/O. Stop ends optional warming,
and cancellation drains an admitted read before releasing workspace memory.
Changed file identities clear their old coverage. A leased replay never
preloads or falls back to disk on a missing library byte; it stops through
recovery. Missing complete coverage keeps the ordinary early-wake path.

The existing 4,096 tags and 16 MiB payload use 2,048 two-way sets, allowing
FAT metadata and colliding movie pages to coexist. One victim bit per set
adds 256 bytes of bookkeeping. Replacement never moves a pinned payload.
Pinned pages bypass collision fills and preserve existing bytes. An accepted
write to any sector of a pinned page revokes the lease before queue publication;
reset, window changes, failures and quiesce also revoke it. Every playback
step and read checks the live mount epoch and cookie. Revocation stops audio
through recovery instead of risking a late HDD wake during playback. Normal
stop keeps the lease until native file replies drain, then releases it at idle.
Files launches with externally supplied handles release any provisional lease.

Admission may promote clean sectors already held in either ordinary small read
window without disk I/O. Synchronous multi-sector checks validate ownership
once before walking their bounded range; no callback runs inside that walk.
Pinned payload addresses remain stable.

Reads allocate no payload buffers, copy at most 16 KiB, and use 64 anchors per
file. Admission caps each file at 4,096 clusters, bounding each residual seek
walk to 63 edges. Read-time FAT edges use the already proved immutable bytes;
full mirror comparisons occur during admission. Consecutive edges in the
same pinned FAT sector reuse its completed mirror comparison. The focused host check is
`python -m tools.qualification.cache_lease`, in development and release modes.

Startup may join the ready steps of one frame: finish its existing packet
metadata step, copy a fully cached payload, accept one native frame request,
transfer one payload chunk, then publish its descriptor. Only exact 5-to-6,
6-to-2, 2-to-3 and 3-to-4 progress may continue, with at most five substeps,
one payload transfer and 4,096 bulk bytes including the final 64-byte descriptor.
Ring control traffic is additional. A 250 microsecond admission budget does not
preempt an existing individual parser, copy or BCM access. Publication ends the
batch. Initial metadata, seeks, storage misses and steady playback retain their
existing service steps. The first-picture fence, ring validation, response
sequence and decoder buffers are unchanged.

An additional payload preparation requires complete coverage in the three
video cache windows, at most 16 KiB, valid file bounds and quiet media/library
owners. Its coverage probe performs no copies, cache-age changes or reads.
An ordinary preparation call that entered with pending work returns to the
executive even when that work completes. A parser that remains pending is not
retried. Proven callback-free response steps retain their original batching
when unrelated audio I/O is pending.

Development channel 3 words 24 through 31 report stream batch service microseconds,
maximum invocation, invocations, additional substeps, responses published in their
accepting invocation, maximum bulk bytes, time-limit stops and byte-limit stops.
Channel 4's corresponding tail reports phase-5 calls, 5-to-6 completions, quiet
phase-6 completions, pending-entry stops, uncovered/oversize extra-payload stops,
additional preparation microseconds, maximum additional preparation and responses
published by batches starting at preparation. All counters reset for each video
open and are absent from release images.

The native file service may send the first reply fragment in the same
invocation that accepts one synchronous successful request during LCD startup.
Only entry phase zero can join a ready phase-three response. Asynchronous
open/read completions, library warming, errors and steady playback retain
their original steps. The extra call never accepts another request and keeps
the existing 4,096-byte fragment limit, whole-response ring-space check and
final-only write-pointer publication. Ring wrap may split that fragment into
two underlying bus writes.

Both admission checks require quiet media/library owners, unchanged generation,
outer phase, channel, workspace pointer/generation and resource epoch. Pending
seek or pause controls, cancellation, stop and restore prohibit joining. A
250-microsecond admission bound permits one existing bus operation to exceed
it. No decoder buffers, storage requests or native acknowledgements change.

Development channel 5 words 24 through 31 report eligible phase-zero service
microseconds, maximum invocation, synchronous successful replies passing the
owner checks, additional transmit attempts, replies completed by those attempts,
response bytes copied by the additional attempts, maximum fragment and time
budget stops. Bytes reflect completed fragment progress, including a copied
fragment whose later publication fails. These counters reset at video open
and are absent from release images.

A queued foreground video request on M25 pauses new Cover Flow read admission.
The artwork service continues polling any admitted read and processing its
terminal result, then returns before requesting another chunk. This lets the
runtime acquire storage
for the selected video while music, settings and resume services continue.
Completed artwork and partial warming progress remain valid. Warming resumes
after cancellation or playback ends.

The caller samples this permission immediately before servicing Cover Flow.
Its deferred storage poll suppresses runtime and Link command dispatch, keeping
that permission stable through the call. An existing packed-art request may
still need to drain: the nonexpanded iFlash chunk is bounded to 93,184 bytes.
The service's per-call polling budget bounds CPU work rather than total storage
latency. First-picture, audio reserve, native response and power policies retain
their existing behavior.

Development selector `0x4d25002c` observes M25 video scheduling without
changing it. Its eight words are selector, fresh ARM microsecond timestamp,
music deadline, Cover Flow I/O deadline (or UINT32_MAX when disabled), video
deadline, video-active flag, playback-pending flag and storage-busy flag.
Compare deadlines with the timestamp using wrapping signed subtraction.
It adds no storage or native-bus reads and is absent from release images.

During active or pending M25 video playback, the music deadline matches the
executive's volume-only music service. It retains an earlier pending volume
retry within the existing 200 ms idle ceiling. Suspended browser, library,
history and artwork deadlines no longer keep the CPU awake after they expire.
The native video, storage, input and power deadlines still apply independently.
A queued video request keeps the full music deadline until playback activates,
so accepted music I/O continues draining before ownership changes. Stopping
restores the full deadline and resumes the existing music and artwork work.
This changes neither native polling intervals nor audio-buffer reserves.

A fully settled LCD pause may poll native receive queues every 10 ms. Native
pause acknowledgement, stopped and paused ARM audio, the existing PCM reserve
(or EOF), a committed selected picture and empty validated file/event/stream
receive snapshots are required. Pending I/O, cache fills, display work, seeks
or controls keep the shorter existing path. The check reads cached snapshots
and adds no bus transactions. After a real service pass, a fixed deadline
suppresses only native queue service on unrelated wakes. Skipped passes and
deadline queries never extend that deadline. Native work arriving after a
snapshot waits for this bounded poll; an earlier status deadline still wins.
Input interrupts wake the CPU, and resume, seek and stop invalidate the pause
conditions immediately. The existing native status cadence remains 500 ms.

Timeline conversion uses one 32-bit division when multiplying the tick count
by 1000 fits in 32 bits and the rate is nonzero. It produces the same rounded
down milliseconds as the existing 64-bit path. Larger tick counts, zero rates
and overflow retain that path and its saturation behavior. This avoids the
long conversion loop on frequent short PCM position queries, including pause.
