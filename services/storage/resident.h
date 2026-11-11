#ifndef LINNET_RESIDENT_MEDIA_H
#define LINNET_RESIDENT_MEDIA_H
#include "services/storage/fat32.h"
#define LN_RESIDENT_FILES 6u
struct ln_resident_binding {struct ln_fat32_file file;char path[LN_FAT32_PATH_MAX+1u];};
/* Only a completed ordinary OPEN may publish this provenance. A cache hit
 * never renews it. Namespace changes retire the mount epoch before writing. */
bool ln_resident_remember(struct ln_resident_binding *,const char *,const struct ln_fat32_file *);
bool ln_resident_match(const struct ln_resident_binding *,const char *,uint32_t epoch,struct ln_fat32_file *);
void ln_resident_forget(struct ln_resident_binding *);
/* A small immutable index into the existing extended clean cache. No media
 * payloads are allocated. A private FAT copy is supplied as temporary scratch. */
struct ln_resident_media {
    uint32_t cookie,epoch,started,failure,elapsed,count;
    uint64_t missing_lba;
    int32_t result;
    uint64_t partition,sectors,fat_start,data_start;
    uint32_t clusters,fat_sectors;
    uint8_t shift,fats,active_fat,mirrored;
    struct {struct ln_fat32_file file;uint32_t shift,anchor[64];} entry[LN_RESIDENT_FILES];
};
bool ln_resident_admit(struct ln_resident_media *,const struct ln_fat32 *,
    const char *const *paths,uint32_t count,struct ln_fat32 *scratch);
bool ln_resident_append(struct ln_resident_media *,const struct ln_fat32 *,
    const char *const *paths,uint32_t count,struct ln_fat32 *scratch);
/* Trusted, successfully opened current-mount handles only. The caller owns
 * provenance and explicit remount invalidation, including reused epoch values.
 * FAT/data/EOC proof is complete; path metadata need not remain in RAM. */
bool ln_resident_admit_files(struct ln_resident_media *,const struct ln_fat32 *,
    const struct ln_fat32_file *,uint32_t count,bool append);
/* Caller must compare against the LIVE volume epoch before every read. The
 * storage adapter supplies that check; a retained file handle alone cannot. */
bool ln_resident_valid(const struct ln_resident_media *,uint32_t epoch);
int ln_resident_read(const struct ln_resident_media *,const struct ln_fat32_file *,
    uint32_t offset,void *output,uint32_t bytes);
void ln_resident_end(struct ln_resident_media *);
#endif
