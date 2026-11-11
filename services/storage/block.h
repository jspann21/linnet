#ifndef LINNET_BLOCK_H
#define LINNET_BLOCK_H
#include "hal/api/ata.h"
#define LINNET_BLOCK_QUEUE 8u
#define LN_BLOCK_READ_AHEAD_DEFAULT 32u
enum ln_block_op { LN_BLOCK_READ=1,LN_BLOCK_WRITE=2,LN_BLOCK_FLUSH=3,LN_BLOCK_STANDBY=4,LN_BLOCK_WAKE=5 };
struct ln_block_completion { uint32_t request_id,generation,status,sectors_done; };
struct ln_block_info {
    uint64_t sectors;
    uint32_t ready,initializing,quarantined,pending,completed,submitted,retries;
    uint32_t failed,canceled,coalesced,cache_hits,read_ahead_sectors,max_service_us;
    uint32_t idle_failures,power_transition;
};
void ln_block_init(uint32_t now);
/* Optional immutable cache lease. Admission requires an entirely idle owner.
 * Pin/read never issue I/O; a missing sector fails without touching output.
 * Collision fills bypass pinned pages. Writes to any sector of a pinned page,
 * recovery, window changes and quiesce revoke the lease synchronously.
 * Cookies are never reused during a boot; release of an old cookie is inert. */
uint32_t ln_block_cache_begin(void);
bool ln_block_cache_valid(uint32_t cookie);
/* Recheck the idle admission barrier before extending an existing lease. */
bool ln_block_cache_extend(uint32_t cookie);
const uint8_t *ln_block_cache_sector(uint32_t cookie,uint64_t lba);
bool ln_block_cache_pin(uint32_t cookie,uint64_t lba,uint32_t sectors);
bool ln_block_cache_copy(uint32_t cookie,uint64_t lba,uint32_t sectors,void *output);
void ln_block_cache_end(uint32_t cookie);
#if defined(LINNET_EXTENDED_RAM) && defined(LINNET_IMAGE_OS)
void ln_block_extended_diagnostics(uint32_t output[4]);
#endif
/* Absolute deadlines must be future and within30 seconds. Zero sectors and
 * null buffer required for non-data operations. Caller retains buffer until
 * completion is taken, even after cancel. Writes are snapshotted at submit.
 * Cancellation after issue cannot undo media writes; sectors_done counts
 * PIO sectors transferred, never durable sectors. FLUSH is the durable barrier.
 * A failed issued write/flush/standby poisons subsequent durability operations,
 * including already queued requests, until explicit recovery. */
bool ln_block_submit(enum ln_block_op op,uint64_t lba,uint32_t sectors,void *buffer,
    uint32_t generation,uint32_t deadline_us,uint32_t *request_id);
/* Read-ahead ceiling is1 (no speculation),8, or32 (hardware-tuned default).
 * Rotating media grows from8 to32 sectors only when continuing a cached run.
 * Captured with the request; coalesced explicit sectors always remain covered.
 * A request group uses its most restrictive ceiling and never expands past the window. */
bool ln_block_submit_read(uint64_t lba,uint32_t sectors,void *buffer,
    uint32_t generation,uint32_t deadline_us,uint32_t read_ahead,uint32_t *request_id);
/* Direct reads transfer up to 192 contiguous sectors into the caller buffer.
 * Resident clean windows may satisfy them; successful reads up to8 sectors
 * populate a window. Qualified extended RAM also retains larger reads in its
 * independent clean cache. Never speculative. */
bool ln_block_submit_direct_read(uint64_t lba,uint32_t sectors,void *buffer,
    uint32_t generation,uint32_t deadline_us,uint32_t *request_id);
void ln_block_step(uint32_t now);
bool ln_block_take(uint32_t generation,struct ln_block_completion *out);
void ln_block_cancel(uint32_t generation);
void ln_block_snapshot(struct ln_block_info *out);
uint32_t ln_block_next_deadline(uint32_t now);
/* The filesystem owner withdraws idle permission during mount/transactions. */
void ln_block_allow_idle(bool allowed);
uint32_t ln_block_request_timeout(void);
/* Stops acceptance immediately. Existing requests drain in FIFO order.
 * Returns0pending,1flush plus supported standby proved,-1failed; call repeatedly until terminal.
 * Resume accepts new requests only after a successful quiesce. */
int ln_block_quiesce(uint32_t now,uint32_t deadline_us);
/* Chainload barrier: drain and flush without spinning down a rotating disk. */
int ln_block_quiesce_keep_awake(uint32_t now,uint32_t deadline_us);
bool ln_block_resume(void);
/* Explicit recovery is allowed only with no outstanding caller requests. */
bool ln_block_recover(uint32_t now);
/* Restrict all subsequent data requests and read-ahead to a discovered media
 * partition. Requires an empty queue; init defaults to the full physical disk
 * so the owner can discover MBR/GPT. No implicit partition selection. */
bool ln_block_set_window(uint64_t first,uint64_t sectors);
/* True only when block/ATA work exists but its next transition is governed by
 * a future deadline. The output deadline is from the same classification.
 * This policy seam is available in release; diagnostics remain development-only. */
bool ln_block_idle_wait(uint32_t now,uint32_t *deadline);
#if !defined(LINNET_PROFILE_RELEASE)
enum ln_block_schedule {LN_BLOCK_SCHEDULE_IDLE=0,LN_BLOCK_SCHEDULE_RUNNABLE=1,
    LN_BLOCK_SCHEDULE_WAITING=2,LN_BLOCK_SCHEDULE_COMPLETION=3};
/* Read-only scheduler classification. RUNNABLE means another block step can
 * change state now; WAITING is governed by the returned ATA/block deadline;
 * COMPLETION means an untaken terminal slot exists. */
enum ln_block_schedule ln_block_schedule_state(uint32_t now,uint32_t *deadline);
/* count/total ready age/max ready age/flags for DONE-to-take wall time.
 * This latency can overlap caller CPU work and is not device service time. */
void ln_block_schedule_metrics(uint32_t out[4]);
/* One-shot software fault at the next issued operation:1timeout beforeissue,
 * 2short result aftertransfer (read data withheld),3reset beforeissue,
 * 4stale completion rejection (callerdata withheld),5flush error.
 * These exercise owner error paths; they do not emulate electrical faults. */
bool ln_block_inject(uint32_t fault);
/* SIOP read-only snapshot,32words. New cumulative counters saturate; bit31
 * invalidates deltas. Use same-reset-epoch idle/drained endpoints for totals. */
void ln_block_performance(uint32_t out[32]);
void ln_block_workload_diagnostics(uint32_t out[8]);
#endif
#endif
