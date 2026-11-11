/* Board policy consumed by the shared PP502x drivers. */
#pragma once
#define LINNET_BOOT_HASH_IRAM 1
#define LINNET_INPUT_FIRST_PACKET_FIX 1
/* The keypad query reconfigures the optical ASIC; run it before streaming. */
#define LINNET_BOOT_MENU_QUERY_BEFORE_INPUT 1
/* Refresh the mono panel once after a chooser change has settled. */
#define LINNET_BOOT_MENU_REDRAW_US 250000u
/* Restore Apple's interrupt/codec state and synchronize both cores. */
#define LINNET_RETAIL_TIMER2_IRQ 1
#define LINNET_RETAIL_I2C_DIVIDER 0x1eu
#define LINNET_RETAIL_COP_ACK 1
#define LINNET_STORAGE_TAIL_SECTOR 0
#define LINNET_CLOCK_SETTLE_US 1000u
#define LINNET_ATA_TRACK_FLUSH 1
#define LINNET_ATA_ALIGNED_WRITE 1
#define LINNET_DISK_MODE_MARKER 0x40017f00u

#define LINNET_AUDIO_IRAM_BYTES 8192
#define LINNET_HARDWARE_CLICKER 1

/* Nominal 0..100% voltage estimate; not a battery capacity measurement. */
#define LINNET_BATTERY_CURVE_MV {3450,3660,3700,3730,3770,3820,3870,3920,4040,4100,4170}

#define LINNET_DISPLAY_SERVICE_PAIRS 256u
