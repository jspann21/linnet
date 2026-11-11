#include "linnet_target.h"
ENTRY(ln_start)
MEMORY {
#if defined(LINNET_IMAGE_OS) && LINNET_HAS_GAMES
 RAM (rwx) : ORIGIN = LINNET_LOAD_ADDRESS, LENGTH = LINNET_GAME_SLOT_OFFSET
 GAME (rwx) : ORIGIN = LINNET_LOAD_ADDRESS + LINNET_GAME_SLOT_OFFSET, LENGTH = LINNET_GAME_SLOT_BYTES
#else
 RAM (rwx) : ORIGIN = LINNET_LOAD_ADDRESS, LENGTH = LINNET_OS_CAPACITY
#endif
 COVER_LOW (rw) : ORIGIN = LINNET_LOAD_ADDRESS + 0x800000, LENGTH = 0x800000
 COVER_HIGH (rw) : ORIGIN = LINNET_STAGE_ADDRESS + LINNET_STAGE_CAPACITY, LENGTH = 0x1400000
 COVER_CATALOG (rw) : ORIGIN = LINNET_STAGE_ADDRESS + LINNET_STAGE_CAPACITY + 0x1400000, LENGTH = 0x400000
 TITLES (rw) : ORIGIN = LINNET_MUSIC_TITLES_ADDRESS, LENGTH = LINNET_MUSIC_TITLES_BYTES
 CAPTURE (rw) : ORIGIN = LINNET_LOADER_SNAPSHOT + LINNET_LOADER_CAPACITY, LENGTH = 153600
 MAILBOX (rw) : ORIGIN = LINNET_MAILBOX_ADDRESS, LENGTH = LINNET_MAILBOX_BYTES
}
SECTIONS {
 .vectors : { KEEP(*(.vectors)) } > RAM
 .text : { *(.text.startup) *(.text*) *(.rodata*) *(.audio_icode*) } > RAM
 .data : { *(.data*) . = ALIGN(32); } > RAM
 __image_end = .;
 .bss (NOLOAD) : { . = ALIGN(16384); __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(32); __bss_end = .; } > RAM
 .music_metadata (NOLOAD) : { *(.music_metadata) } > RAM
 .media_workspace (NOLOAD) : { *(.media_workspace) } > RAM
 .stacks (NOLOAD) : {
  . = ALIGN(8); . += 1024; __abort_stack = .;
  . += 1024; __undef_stack = .; . += 2048; __irq_stack = .;
  . += 1024; __fiq_stack = .; . += 32768; __stack_top = .;
 } > RAM
 .cover_low (NOLOAD) : { *(.cover_low) } > COVER_LOW
 .cover_high (NOLOAD) : { *(.cover_high) } > COVER_HIGH
 .cover_catalog (NOLOAD) : { *(.cover_catalog) } > COVER_CATALOG
 .music_titles (NOLOAD) : { *(.music_titles) } > TITLES
 .dev_capture (NOLOAD) : { *(.dev_capture) } > CAPTURE
 .noinit (NOLOAD) : { KEEP(*(.noinit*)) } > MAILBOX
 .persistent_handoff (LINNET_MAILBOX_ADDRESS + 0x100) (NOLOAD) : { . += 0x80; } > MAILBOX

#if defined(LINNET_IMAGE_OS) && LINNET_HAS_GAMES
 .game_slot (NOLOAD) : { __game_slot = .; . += LINNET_GAME_SLOT_BYTES; __game_slot_end = .; } > GAME
 ASSERT(__stack_top <= ORIGIN(GAME), "Firmware overlaps executable game slot")
 ASSERT(ORIGIN(GAME)+LENGTH(GAME) <= LINNET_LOAD_ADDRESS+LINNET_OS_CAPACITY, "Game slot exceeds OS allocation")
#endif
 /DISCARD/ : { *(.comment) *(.ARM.exidx*) *(.ARM.extab*) *(.note*) }
 ASSERT(SIZEOF(.vectors)==64,"ARM926 absolute vectors require 64 bytes")
 ASSERT(SIZEOF(.noinit)<=0x100,"Crash record overlaps persistent handoff")
 ASSERT(__stack_top<=ORIGIN(RAM)+LENGTH(RAM),"OS RAM overflow")
 ASSERT(ORIGIN(RAM)+LENGTH(RAM)<=ORIGIN(COVER_LOW),"OS RAM overlaps artwork")
 ASSERT(ORIGIN(COVER_LOW)+LENGTH(COVER_LOW)<=LINNET_STAGE_ADDRESS,"Artwork overlaps stage")
 ASSERT(ORIGIN(COVER_HIGH)+LENGTH(COVER_HIGH)<=ORIGIN(COVER_CATALOG),"Artwork overlaps catalog")
 ASSERT(ORIGIN(COVER_CATALOG)+LENGTH(COVER_CATALOG)<=LINNET_LOADER_SNAPSHOT,"Catalog overlaps recovery")
}
