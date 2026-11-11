#ifndef LINNET_FAT32_H
#define LINNET_FAT32_H
#include <stdbool.h>
#include <stdint.h>

#define LN_FAT32_PATH_MAX 255u
#define LN_FAT32_CACHE_SIZE 16u
#define LN_FAT32_SET_MAX 18u
#define LN_FAT32_DIR_MAX 32u
/* Classic OS alternates photo, music and library files through one reader.
 * Match the proof cache capacity so active files retain their proven seek
 * maps; denser anchors shorten uncached positions in large photo packages. */
#if defined(LINNET_TARGET_CLASSIC6G_N25) && defined(LINNET_IMAGE_OS)
#define LN_FAT32_SEEK_MAPS LN_FAT32_CACHE_SIZE
#define LN_FAT32_SEEK_ANCHORS 512u
#else
#define LN_FAT32_SEEK_MAPS 8u
#define LN_FAT32_SEEK_ANCHORS 64u
#endif
enum ln_fat32_error { LN_FAT32_BUSY=0, LN_FAT32_DONE=1,
    LN_FAT32_ARGUMENT=-1, LN_FAT32_IO=-2, LN_FAT32_FORMAT=-3,
    LN_FAT32_CORRUPT=-4, LN_FAT32_NOT_FOUND=-5, LN_FAT32_UNSUPPORTED=-6,
    LN_FAT32_READ_ONLY=-7, LN_FAT32_BOUNDS=-8, LN_FAT32_STALE=-9,
    LN_FAT32_EXISTS=-10, LN_FAT32_NO_SPACE=-11 };
enum ln_fat32_io { LN_FAT32_READ=1, LN_FAT32_WRITE=2, LN_FAT32_FLUSH=3 };
/* io returns 0 while pending, 1 on complete, negative on failure. Repeated
 * calls with identical arguments poll the SAME request, never resubmit it.
 * Buffer ownership lasts through completion. FLUSH has NULL buffer/LBA0.
 * The adapter verifies block request ID, generation and full sector transfer.
 * Callers must drain an operation before reusing or unmounting this context. */
struct ln_fat32_config {
    int (*io)(void *context, enum ln_fat32_io operation, uint64_t lba,
              uint8_t *sector, uint32_t generation);
    /* Optional contiguous READ/WRITE transfer, with the same polling and
     * buffer ownership as io; completion covers exactly sectors sectors.
     * Writes are bounded to eight sectors. */
    int (*sectors_io)(void *context, enum ln_fat32_io operation, uint64_t lba, uint8_t *buffer,
                        uint32_t sectors, uint32_t generation);
    void *context;
    uint64_t disk_sectors;
    bool writable;
};
struct ln_fat32_file {
    uint32_t cluster, size, epoch;
    uint64_t directory_sector;
    uint16_t directory_offset;
    uint8_t attributes;
};
/* Read-only directory stream. A zeroed cursor starts at root; cluster may be
 * set to a directory handle's cluster with its epoch. No write-proof slots are
 * consumed. Keep the cursor/buffer alive until poll completes; cancellation
 * invalidates that in-progress cursor, so resume from a saved copy. */
struct ln_fat32_directory {
    uint32_t cluster, sector, epoch, steps, anchor, power, distance;
    uint64_t lba;
    bool end;
};
struct ln_fat32_cache { uint32_t cluster, next; };
struct ln_fat32_verified { uint32_t cluster, size; };
struct ln_fat32_seek { uint32_t first, index, cluster; };
struct ln_fat32_seek_map {
    uint32_t first,size,age,shift;
    struct {uint32_t cluster,run;} anchors[LN_FAT32_SEEK_ANCHORS];
};
struct ln_fat32 {
    struct ln_fat32_config config;
    struct ln_fat32_file file;
    struct ln_fat32_cache cache[LN_FAT32_CACHE_SIZE];
    /* Fully associative chain proofs, most recent first; exclusive mount only. */
    struct ln_fat32_verified verified[LN_FAT32_CACHE_SIZE];
    struct ln_fat32_seek seeks[LN_FAT32_CACHE_SIZE];
    /* Sparse anchors cover the whole published file rather than only its
     * most recently visited tail. Runs contain proven consecutive FAT edges. */
    struct ln_fat32_seek_map seek_maps[LN_FAT32_SEEK_MAPS];
    uint32_t seek_clock,seek_slot;
    struct ln_fat32_file validated[LN_FAT32_SET_MAX];
    uint32_t directories[LN_FAT32_DIR_MAX];
    uint32_t terminals[LN_FAT32_SET_MAX+LN_FAT32_DIR_MAX];
    uint32_t directory_count, set_count, set_index;
    uint32_t proven_directory_count,open_directory_count;
    bool proof_base_valid;
    bool write_proven;
    bool open_write_proven;
    uint64_t partition, partition_sectors, fat_start, data_start, io_lba;
    uint32_t total, fat_sectors, clusters, root, epoch, generation;
    uint32_t cluster, chain_steps, fat_cluster, fat_value, fat_phase;
    /* Primary-buffer FAT page plus1; zero invalid, VERIFY_CHAIN request only. */
    uint32_t verified_fat_sector;
    uint32_t offset, length, transferred, skip, within, component, path_cursor;
    uint32_t directory_sector_index, directory_entry_index;
    uint32_t cycle_anchor, cycle_power, cycle_distance, verified_clusters, logical_cluster;
    uint32_t new_size,new_first,new_tail,candidate,allocation_count,allocation_needed;
    uint32_t source_cluster,source_sector,destination_sector,edit_phase,edit_value,volume_flags,old_next;
    uint64_t slot_sector,copy_offset;
    uint32_t fsinfo,backup,reserved;
    uint16_t slot_offset,mutation_return;
    uint8_t entry[32];
    bool slot_found,mutation_started,slot_end;
    uint16_t state,sectors_per_cluster;
    uint8_t cluster_shift, fats, active_fat, mirrored, mounted, operation;
    uint8_t *buffer;
    struct ln_fat32_directory *listing;
    int result;
    char path[LN_FAT32_PATH_MAX+1];
    uint8_t name[11];
    bool system_long_name,system_long_match;
    bool readonly_open;
    uint8_t system_long_checksum;
    uint8_t sector[512] __attribute__((aligned(32)));
    uint8_t mirror[512] __attribute__((aligned(32)));
};
void ln_fat32_init(struct ln_fat32 *, const struct ln_fat32_config *);
/* Explicit partition bounds in native 512-byte sectors. BPB sector sizes up
 * to 4096 bytes are normalized on mount. Operations return admission. */
bool ln_fat32_mount(struct ln_fat32 *,uint64_t lba,uint64_t sectors,uint32_t generation);
/* Absolute, bounded 8.3 paths, depth <=16. LFN aliases may be opened using
 * their on-disk 8.3 names. The root .LINNET directory also supports its verified
 * single-entry FAT long name, independent of its generated short alias. */
bool ln_fat32_open(struct ln_fat32 *,const char *path,uint32_t generation);
/* Same open semantics for one leaf under an already opened directory handle.
 * Rejects stale/unregistered parents; all directory-set proof remains required. */
bool ln_fat32_open_at(struct ln_fat32 *,const struct ln_fat32_file *parent,
                       const char *leaf,uint32_t generation);
/* Media-only lookup: absolute path with NULL parent, or one leaf under a live
 * parent. Does not register directories in the bounded mutable-file proof set;
 * handles still carry the mount epoch and reads retain chain validation. */
bool ln_fat32_open_readonly(struct ln_fat32 *,const struct ln_fat32_file *parent,
                          const char *path,uint32_t generation);
bool ln_fat32_read(struct ln_fat32 *,const struct ln_fat32_file *,uint32_t offset,
                   void *buffer,uint32_t length,uint32_t generation);
/* Produces one raw 512-byte directory sector, or zero bytes at chain end.
 * The cursor carries cycle detection across calls; consumers stop at entry0. */
bool ln_fat32_list(struct ln_fat32 *,struct ln_fat32_directory *,void *sector,uint32_t generation);
/* In-place writes only within a preallocated file's published size. Every
 * successful operation flushes before DONE. Atomicity across sectors is the
 * caller's dual-slot publication policy, not a FAT32 guarantee. */
bool ln_fat32_write(struct ln_fat32 *,const struct ln_fat32_file *,uint32_t offset,
                    const void *buffer,uint32_t length,uint32_t generation);
bool ln_fat32_sync(struct ln_fat32 *,uint32_t generation);
/* Requires a clean writable volume and ownership proof covering the parent
 * directories (and the old file for grow). Create does not expand directories.
 * New chains are detached, fully initialized and flushed before one directory
 * sector publishes start/size. Grow copies the old file and reclaims its chain
 * only AFTER publication is flushed. Failure may leak allocated clusters.
 * Any mutation error quarantines writes. Mutation invalidates handles/proofs
 * even on failure: reopen all files and validate_set again after success.
 * Directory sector atomicity requires
 * the block device's normal sector-write guarantee; no torn-write journal is
 * implied. size>0, <=65536 clusters. No shrink or overwrite-by-create. */
bool ln_fat32_create(struct ln_fat32 *,const char *path,uint32_t size,uint32_t generation);
bool ln_fat32_grow(struct ln_fat32 *,const struct ln_fat32_file *,uint32_t size,uint32_t generation);
/* Full chain/cycle/length proofs for all files and every directory reached
 * by open(), including root. Distinct terminal clusters prove disjoint chains
 * in a singly linked FAT. Writes require this set proof for their exact file.
 * Exclusive media ownership is mandatory until unmount/remount; an external
 * writer invalidates every cached proof. At most16 files/32 directory roots.
 * count0/filesNULL proves registered directories alone for first creation. */
bool ln_fat32_validate_set(struct ln_fat32 *,const struct ln_fat32_file *files,
                         uint32_t count,uint32_t generation);
/* Opening an additional directory temporarily disables writes. During the
 * same exclusive mount, refresh traverses only newly registered directory
 * chains and compares their terminals with the retained complete set proof.
 * It never rescans file chains. Poll to DONE before admitting writes; failure
 * retires the retained base. Pending is not a permanent read-only condition. */
bool ln_fat32_proof_pending(const struct ln_fat32 *);
bool ln_fat32_refresh_proof(struct ln_fat32 *,uint32_t generation);
/* At most one I/O callback per poll. Read/write and set-proof polling may
 * evaluate up to four chain edges (16 for Photo OS reads); additional edges
 * must already be cached, and a terminal or chain boundary always yields.
 * Other states perform one metadata step.
 * This bounds work count, not elapsed time; service runs between polls.
 * file holds the open result; transferred counts completed bytes after errors.
 * I/O failure quarantines writes until a successful remount. */
int ln_fat32_poll(struct ln_fat32 *);
/* Retires only an admitted read/open after its adapter has drained the current
 * block request. Result is STALE for clean/canceled completion or IO for a
 * failed completion; any earlier FAT error wins. STALE discards incomplete
 * seek/open proof while preserving the mount for the next owner. */
int ln_fat32_retire_read(struct ln_fat32 *,int result);
/* Adapter must first cancel AND drain its pending block request. Abandoning
 * remaining metadata work invalidates mount/handles; remount is mandatory. */
void ln_fat32_abandon(struct ln_fat32 *);
#endif
