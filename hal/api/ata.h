#ifndef LINNET_HAL_ATA_H
#define LINNET_HAL_ATA_H
#include <stdbool.h>
#include <stdint.h>
#include "linnet_target.h"
#define LINNET_ATA_SECTOR_BYTES 512u
#define LINNET_ATA_MAX_SECTORS 8u
#define LINNET_ATA_MAX_READ_SECTORS LINNET_STORAGE_MAX_READ_SECTORS
#if defined(LINNET_IMAGE_OS) && (defined(LINNET_SOC_PP5022) || defined(LINNET_TARGET_CLASSIC6G_N25) || defined(LINNET_TARGET_PHOTO4G_P98))
/* Disk Mode supplies a larger buffer; ordinary storage writes stay at 8 sectors. */
#if defined(LINNET_SOC_PP5022)
#define LINNET_ATA_MAX_WRITE_SECTORS 128u
#elif defined(LINNET_TARGET_PHOTO4G_P98)
#define LINNET_ATA_MAX_WRITE_SECTORS 64u
#else
#define LINNET_ATA_MAX_WRITE_SECTORS 32u
#endif
#else
#define LINNET_ATA_MAX_WRITE_SECTORS LINNET_ATA_MAX_SECTORS
#endif
enum ln_ata_op { LN_ATA_READ=1,LN_ATA_WRITE=2,LN_ATA_FLUSH=3,LN_ATA_STANDBY=4,LN_ATA_WAKE=5 };
enum ln_ata_status { LN_ATA_OK=0,LN_ATA_TIMEOUT=1,LN_ATA_IO=2,LN_ATA_UNSUPPORTED=3,
    LN_ATA_RANGE=4,LN_ATA_OFFLINE=5,LN_ATA_CANCELED=6,LN_ATA_QUARANTINED=7 };
enum ln_ata_media_kind { LN_ATA_MEDIA_UNKNOWN=0, LN_ATA_MEDIA_ROTATING=1, LN_ATA_MEDIA_SOLID_STATE=2 };
struct ln_ata_info {
    uint64_t sectors;
    uint32_t initialized,initializing,busy,sleeping,failed,lba48,can_flush;
    uint32_t commands,sectors_read,sectors_written,timeouts,errors,last_status,last_error;
    uint32_t media_kind,rotation_rpm,logical_sector_bytes,physical_sector_bytes,compact_flash;
    /* Capabilities: LBA48, FLUSH, PM enabled, APM supported, write cache enabled.
     * Quirks: bit0 missing-LBA CFA probe, bit1 optional APM ABRT, bit2 CFA claim,
     * bit3 exact iFlash platform model, bit4 host-aligned physical PATA transfers. */
    uint32_t capabilities,quirks,powered,standby_supported,apm_supported,write_cache_enabled,can_power_off,power_safe;
    uint32_t idle_timeout_us,wake_timeout_us,poweroffs,spinups;
    char model[41],serial[21],firmware[9];
    /* phase, failedphase, failurestatus, lastcommand, initelapsed_us;
     * packed16bit IDENTIFY pairs:0/49,60/61,83/80,100/101,102/103,
     * 106/117,118/255; IDEcfg,timing0,timing1,DEVen,DEVreset,GPIOJout,
     * GPIOIenable,GPIOKenable,ALTstatus,identifyreject,taskfile0,taskfile1. */
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t diagnostic[24];
#endif
};
struct ln_ata_result { uint32_t status,sectors_done; };
/* Single executive owner. Init/recover and start only issue bounded register
 * writes; step services bounded PIO batches or completed DMA. No hidden waits or heap. */
void ln_ata_init(uint32_t now);
void ln_ata_recover(uint32_t now);
/* Rail removal only with power_safe from a completed standby operation.
 * Known iFlash permits flush-only quiescence; other known media requires PM.
 * Unknown adapters retain power. Power-on asynchronously revalidates identity. */
bool ln_ata_power_off(void);
bool ln_ata_power_on(uint32_t now);
bool ln_ata_start(enum ln_ata_op op,uint64_t lba,uint32_t sectors,uint8_t *buffer,uint32_t deadline);
void ln_ata_step(uint32_t now);
bool ln_ata_take(struct ln_ata_result *result);
void ln_ata_snapshot(struct ln_ata_info *out);
/* Compact live state for the block owner. Copies current fields synchronously;
 * no cached duplicate, MMIO, callbacks, or changes to the full diagnostic layout. */
struct ln_ata_state {
    uint64_t sectors;
    uint32_t initialized,initializing,busy,sleeping,failed,can_flush;
    uint32_t powered,standby_supported,can_power_off,power_safe,idle_timeout_us;
    uint32_t media_kind;
};
void ln_ata_state_snapshot(struct ln_ata_state *out);

#if !defined(LINNET_PROFILE_RELEASE)
void ln_ata_diagnostics(uint32_t out[24]);
#endif
uint32_t ln_ata_next_deadline(uint32_t now);
#if !defined(LINNET_PROFILE_RELEASE)
/* Read-only SIOP components; cumulative new counters saturate, bit31 signals
 * unusable deltas. Timestamps wrap normally; reset epoch separates captures. */
/* ACAP: cached IDENTIFY and guarded passive registers; no ATA commands. */
void ln_ata_capabilities(uint32_t out[16]);
void ln_ata_clock_diagnostics(uint32_t out[8]);
void ln_ata_performance(uint32_t out[16]);
#endif
#endif
