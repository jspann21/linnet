ENTRY(ln_game_entry_point)
SECTIONS {
 . = GAME_ADDRESS;
 __game_start = .;
 .text : { KEEP(*(.text.ln_game_entry_point)) *(.text*) }
 __game_text_end = .;
 .rodata : { *(.rodata*) }
 .data : { *(.data*) . = ALIGN(32); }
 __game_image_end = .;
 .bss (NOLOAD) : { *(.bss*) *(COMMON) . = ALIGN(32); }
 __game_end = .;
 /DISCARD/ : { *(.comment) *(.ARM.exidx*) *(.ARM.extab*) *(.note*) }
 ASSERT(__game_end - __game_start <= 0x40000, "Game exceeds 256 KiB slot")
}
