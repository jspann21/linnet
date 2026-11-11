ENTRY(ln_start)
MEMORY {
 SRAM (rwx) : ORIGIN = 0x22000000, LENGTH = 0x18000
 FRAME (rw) : ORIGIN = 0x08000000, LENGTH = 0x100000
}
SECTIONS {
 .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) } > SRAM
 .data : { *(.data*) } > SRAM
 . = ALIGN(32);
 .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(32); __bss_end = .; } > SRAM
 .dram.framebuffers (NOLOAD) : { *(.dram.framebuffers) } > FRAME
 .dram.pcm (NOLOAD) : { *(.dram.pcm) } > FRAME
 .dram.mmu (NOLOAD) : { *(.dram.mmu) } > FRAME
 .dram.audio (NOLOAD) : { *(.dram.audio) } > FRAME
 .dram.flash (NOLOAD) : { *(.dram.flash) } > FRAME
 .dram.usb (NOLOAD) : { *(.dram.usb) } > FRAME
 /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment*) }
 ASSERT(__bss_end <= 0x22016000, "Bootstrap must leave 4 KiB staging below retained diagnostics")
}
