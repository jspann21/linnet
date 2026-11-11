#include "linnet_target.h"
#if defined(LINNET_IMAGE_BOOT) && defined(LINNET_PLATFORM_CORE) && defined(LINNET_STORAGE) && LINNET_BOOT_HASH_IRAM
#define LINNET_USE_BOOT_HASH_IRAM 1
#ifdef LINNET_AUDIO
#error Boot hash IRAM and audio IRAM cannot coexist
#endif
#endif
#if defined(LINNET_IMAGE_OS) && defined(LINNET_PLATFORM_CORE) && defined(LINNET_AUDIO) && defined(LINNET_TARGET_VIDEO5G_M25)
#define LINNET_USE_OS_CRC_IRAM 1
#endif
ENTRY(ln_start)
MEMORY {
#if defined(LINNET_IMAGE_OS) && LINNET_HAS_GAMES
 SDRAM (rwx) : ORIGIN = LINNET_LOAD_ADDRESS, LENGTH = LINNET_GAME_SLOT_OFFSET
 GAME (rwx) : ORIGIN = LINNET_LOAD_ADDRESS + LINNET_GAME_SLOT_OFFSET, LENGTH = LINNET_GAME_SLOT_BYTES
#else
 SDRAM (rwx) : ORIGIN = LINNET_LOAD_ADDRESS, LENGTH = LINNET_OS_CAPACITY
#endif
#if defined(LINNET_COVERFLOW)
  COVER_LOW (rw) : ORIGIN = 0x00800000, LENGTH = 0x00800000
  COVER_HIGH (rw) : ORIGIN = 0x01800000, LENGTH = 0x00600000
#endif
#if !defined(LINNET_PROFILE_RELEASE) || defined(LINNET_STORAGE)
  STAGE (rw) : ORIGIN = LINNET_STAGE_ADDRESS, LENGTH = LINNET_STAGE_CAPACITY
#endif
#ifndef LINNET_PROFILE_RELEASE
  SNAPSHOT (rw) : ORIGIN = LINNET_LOADER_SNAPSHOT, LENGTH = LINNET_LOADER_CAPACITY
  CAPTURE (rw) : ORIGIN = LINNET_LOADER_SNAPSHOT + LINNET_LOADER_CAPACITY, LENGTH = LINNET_LCD_WIDTH_ASM * LINNET_LCD_HEIGHT_ASM * 2
  USB_DMA (rw) : ORIGIN = 0x4000c000, LENGTH = 0xe00
#endif
#if !defined(LINNET_PROFILE_RELEASE) || defined(LINNET_PLATFORM_CORE)
  CONTROL (rw) : ORIGIN = LINNET_MAILBOX_ADDRESS - 0x1000, LENGTH = 0x1000
#endif
#ifdef LINNET_PLATFORM_CORE
  COP_MAILBOX (rw) : ORIGIN = LINNET_MAILBOX_ADDRESS - 0x2000, LENGTH = 0x1000
  COP_IRAM (rwx) : ORIGIN = 0x40001000, LENGTH = 0x2000
#ifdef LINNET_USE_BOOT_HASH_IRAM
  BOOT_HASH_IRAM (rwx) : ORIGIN = 0x40003000, LENGTH = 0x1000
#endif
  CLOCK_IRAM (rwx) : ORIGIN = 0x4000d000, LENGTH = 0x200
#ifdef LINNET_AUDIO
  AUDIO_IRAM (rwx) : ORIGIN = 0x40003000, LENGTH = LINNET_AUDIO_IRAM_BYTES
  CACHE_IRAM (rwx) : ORIGIN = 0x4000d200, LENGTH = 0xe00
#endif
  CACHE_STACK (rw) : ORIGIN = 0x4000e000, LENGTH = 0x1000
#endif
#ifdef LINNET_AUDIO
  MUSIC_TITLES (rw) : ORIGIN = LINNET_MUSIC_TITLES_ADDRESS, LENGTH = LINNET_MUSIC_TITLES_BYTES
#endif
#if defined(LINNET_IMAGE_OS) && LINNET_TARGET_NUMBER == 98
  VIDEO_OVERLAY (rw) : ORIGIN = LINNET_LOADER_SNAPSHOT + LINNET_LOADER_CAPACITY + LINNET_LCD_WIDTH_ASM * LINNET_LCD_HEIGHT_ASM * 2,
    LENGTH = LINNET_MUSIC_TITLES_ADDRESS - (LINNET_LOADER_SNAPSHOT + LINNET_LOADER_CAPACITY + LINNET_LCD_WIDTH_ASM * LINNET_LCD_HEIGHT_ASM * 2)
#endif
  MAILBOX (rw) : ORIGIN = LINNET_MAILBOX_ADDRESS, LENGTH = LINNET_MAILBOX_BYTES
}
SECTIONS {
  .vectors : { KEEP(*(.vectors)) } > SDRAM
  .text : { *(.text.startup) *(.text*) *(.rodata*) } > SDRAM
  .data : { *(.data*) . = ALIGN(4); } > SDRAM
#ifdef LINNET_USE_BOOT_HASH_IRAM
  /* Separate Boot ownership, not an implicit reuse of the OS reservation. */
  .boot_hash ORIGIN(BOOT_HASH_IRAM) : ALIGN(4) {
    __boot_hash_begin = .;
    KEEP(*(.boot_hash.text)) KEEP(*(.boot_hash.rodata)) KEEP(*(.boot_hash.data))
    . = ALIGN(4);
    __boot_hash_end = .;
  } > BOOT_HASH_IRAM AT> SDRAM
  __boot_hash_load = LOADADDR(.boot_hash);
  __image_end = LOADADDR(.boot_hash) + SIZEOF(.boot_hash);
  ASSERT(SIZEOF(.boot_hash) <= LENGTH(BOOT_HASH_IRAM), "Boot hash IRAM overflow")
  ASSERT(((ABSOLUTE(__boot_hash_begin) | ABSOLUTE(__boot_hash_end) | ABSOLUTE(__boot_hash_load)) & 3) == 0, "Boot hash copy alignment")
  ASSERT(ORIGIN(BOOT_HASH_IRAM) >= ORIGIN(COP_IRAM) + LENGTH(COP_IRAM), "Boot hash overlaps COP stack")
  ASSERT(ORIGIN(BOOT_HASH_IRAM) + LENGTH(BOOT_HASH_IRAM) <= 0x4000c000, "Boot hash overlaps USB DMA")
  .bss ALIGN(__image_end, 128) (NOLOAD)
#elif defined(LINNET_AUDIO)
  /* Keep the stored copy contiguous with the SDRAM image. The VMA is beyond
   * the COP's downward-growing stack, and below development USB DMA. */
  .audio_icode ORIGIN(AUDIO_IRAM) : ALIGN(4) {
    __audio_icode_begin = .;
#ifdef LINNET_USE_OS_CRC_IRAM
    /* Keep the CRC code/table order while giving the fallback a checked
     * alias into this image's immutable SDRAM load copy. */
    *(.audio_icode.crc.text)
    __os_crc_byte_begin = .;
    *(.audio_icode.crc.byte)
    __os_crc_byte_end = .;
#endif
    *(.audio_icode*)
    . = ALIGN(4);
    __audio_icode_end = .;
  } > AUDIO_IRAM AT> SDRAM
  __audio_icode_load = LOADADDR(.audio_icode);
  __image_end = LOADADDR(.audio_icode) + SIZEOF(.audio_icode);
#ifdef LINNET_USE_OS_CRC_IRAM
  __os_crc_byte_load = ABSOLUTE(LOADADDR(.audio_icode) + (__os_crc_byte_begin - ADDR(.audio_icode)));
  ASSERT(__os_crc_byte_end - __os_crc_byte_begin == 1024, "OS CRC byte table size")
  ASSERT(((ABSOLUTE(__os_crc_byte_begin) | ABSOLUTE(__os_crc_byte_load)) & 3) == 0, "OS CRC byte table alignment")
  ASSERT(__os_crc_byte_begin >= ADDR(.audio_icode) && __os_crc_byte_end <= ADDR(.audio_icode) + SIZEOF(.audio_icode), "OS CRC byte table IRAM bounds")
  ASSERT(__os_crc_byte_load >= LOADADDR(.audio_icode) && __os_crc_byte_load + 1024 <= __image_end, "OS CRC byte table load bounds")
  ASSERT(__os_crc_byte_load >= ORIGIN(SDRAM) && __os_crc_byte_load + 1024 <= ORIGIN(SDRAM) + LENGTH(SDRAM), "OS CRC byte table SDRAM bounds")
  ASSERT(__os_crc_byte_load + 1024 <= __bss_start, "OS CRC byte table overlaps BSS")
#endif
  ASSERT(ORIGIN(AUDIO_IRAM) + LENGTH(AUDIO_IRAM) <= 0x4000c000, "Audio IRAM overlaps USB DMA")
  ASSERT(SIZEOF(.audio_icode) <= LENGTH(AUDIO_IRAM), "Audio IRAM code and veneers exceed target budget")
  ASSERT(((ABSOLUTE(__audio_icode_begin) | ABSOLUTE(__audio_icode_end) | ABSOLUTE(__audio_icode_load)) & 3) == 0, "Audio IRAM copy must be word aligned")
  /* BSS must not overlap the stored copy used by startup and recovery. */
  .bss ALIGN(__image_end, 128) (NOLOAD)
#else
  __image_end = .;
  .bss (NOLOAD)
#endif
  : { . = ALIGN(8); __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(8); __bss_end = .; } > SDRAM
  /* Verification fills this cache before publication; no startup clear needed. */
  .music_metadata (NOLOAD) : { *(.music_metadata) } > SDRAM
  .media_workspace (NOLOAD) : { *(.media_workspace) } > SDRAM
  .stacks (NOLOAD) : {
    . = ALIGN(8); . += 1024; __abort_stack = .;
    . += 1024; __undef_stack = .;
    . += 1024; __irq_stack = .;
    . += 1024; __fiq_stack = .;
#if defined(LINNET_AUDIO)
    /* libmad Layer III plus decoder/foreground frames need more than8 KiB. */
    . += 32768; __stack_top = .;
#else
    . += 8192; __stack_top = .;
#endif
  } > SDRAM
#if defined(LINNET_COVERFLOW)
  .cover_low (NOLOAD) : { *(.cover_low) } > COVER_LOW
  .cover_high (NOLOAD) : { *(.cover_high) } > COVER_HIGH
  ASSERT(ORIGIN(COVER_LOW) >= LINNET_LOAD_ADDRESS + LINNET_OS_CAPACITY, "Artwork overlaps OS")
  ASSERT(ORIGIN(COVER_LOW) + LENGTH(COVER_LOW) <= LINNET_STAGE_ADDRESS, "Artwork overlaps staging")
  ASSERT(ORIGIN(COVER_HIGH) >= LINNET_STAGE_ADDRESS + LINNET_STAGE_CAPACITY, "Artwork overlaps staging")
  ASSERT(ORIGIN(COVER_HIGH) + LENGTH(COVER_HIGH) <= LINNET_LOADER_SNAPSHOT, "Artwork overlaps recovery")
#endif
#ifdef LINNET_AUDIO
  .music_titles (NOLOAD) : { *(.music_titles) } > MUSIC_TITLES
  ASSERT(ORIGIN(MUSIC_TITLES) >= LINNET_LOADER_SNAPSHOT + LINNET_LOADER_CAPACITY + LINNET_LCD_WIDTH_ASM * LINNET_LCD_HEIGHT_ASM * 2, "Titles overlap capture")
  ASSERT(ORIGIN(MUSIC_TITLES) + LENGTH(MUSIC_TITLES) <= LINNET_MAILBOX_ADDRESS - 0x2000, "Titles overlap control")
#endif
#if defined(LINNET_IMAGE_OS) && LINNET_TARGET_NUMBER == 98
  /* Pixel contents are published before use; validity remains in zeroed BSS. */
  .video_overlay (NOLOAD) : { *(.video_overlay) } > VIDEO_OVERLAY
  ASSERT(SIZEOF(.video_overlay) <= LENGTH(VIDEO_OVERLAY), "Video overlay overlaps music titles")
#endif
  .noinit (NOLOAD) : { KEEP(*(.noinit*)) } > MAILBOX
#ifdef LINNET_STORAGE
  .persistent_handoff (LINNET_MAILBOX_ADDRESS + 0x100) (NOLOAD) : { . += 0x80; } > MAILBOX
  ASSERT(SIZEOF(.noinit) <= 0x100, "Retained crash record overlaps persistent handoff")
#endif
#ifdef LINNET_PLATFORM_CORE
  .cop_mailbox (NOLOAD) : { . += 0x1000; } > COP_MAILBOX
  .cop_iram (NOLOAD) : { . += 0x2000; } > COP_IRAM
  .clock_iram (NOLOAD) : { . += 0x200; } > CLOCK_IRAM
#ifdef LINNET_AUDIO
  .cache_iram (NOLOAD) : { . += 0xe00; } > CACHE_IRAM
#endif
  .cache_stack (NOLOAD) : { . += 0x1000; } > CACHE_STACK
#endif
#if defined(LINNET_STORAGE) && defined(LINNET_PROFILE_RELEASE)
  .update_staging (NOLOAD) : { . += LINNET_STAGE_CAPACITY; } > STAGE
#endif
#ifndef LINNET_PROFILE_RELEASE
  .dev_staging (NOLOAD) : { . += LINNET_STAGE_CAPACITY; } > STAGE
  .dev_snapshot (NOLOAD) : { . += LINNET_LOADER_CAPACITY; } > SNAPSHOT
  .dev_capture (NOLOAD) : { *(.dev_capture) } > CAPTURE
  ASSERT(SIZEOF(.dev_capture) <= LENGTH(CAPTURE), "Development screenshot exceeds diagnostic reservation")
  .dev_usb_dma (NOLOAD) : { . += 0xe00; } > USB_DMA
#endif
#if !defined(LINNET_PROFILE_RELEASE)
  .dev_control (NOLOAD) : { . += 0x1000; } > CONTROL
#elif defined(LINNET_PLATFORM_CORE)
  .platform_control (NOLOAD) : { . += 0x1000; } > CONTROL
#endif

#if defined(LINNET_IMAGE_OS) && LINNET_HAS_GAMES
 .game_slot (NOLOAD) : { __game_slot = .; . += LINNET_GAME_SLOT_BYTES; __game_slot_end = .; } > GAME
 ASSERT(__stack_top <= ORIGIN(GAME), "Firmware overlaps executable game slot")
 ASSERT(ORIGIN(GAME)+LENGTH(GAME) <= LINNET_LOAD_ADDRESS+LINNET_OS_CAPACITY, "Game slot exceeds OS allocation")
#endif
  /DISCARD/ : { *(.comment) *(.ARM.exidx*) *(.ARM.extab*) *(.note*) }
#ifdef LINNET_AUDIO
  /* Native codec arenas share the low8 MiB OS region. The independent Boot
   * recovery snapshot at30 MiB still has its unchanged1 MiB payload limit. */
  /* The metadata page cache and expanded browser rows fit below 4.5 MiB,
   * within the existing 8 MiB OS region and below all staging reservations. */
#if defined(LINNET_COVERFLOW)
  ASSERT(__stack_top <= LINNET_RESIDENT_LIMIT_BYTES, "Cover Flow resident image exceeds board budget")
#else
  ASSERT(__stack_top <= 0x00540000, "Music resident image exceeds 5.25 MiB")
#endif
#else
  ASSERT(__stack_top <= 0x00100000, "Resident image exceeds 1 MiB")
#endif
  ASSERT(SIZEOF(.vectors) == 32, "ARM vector table must be 32 bytes")
  ASSERT(LINNET_MAILBOX_ADDRESS + LINNET_MAILBOX_BYTES <= LINNET_SDRAM_BYTES,
         "Retained mailbox exceeds target SDRAM")
}
