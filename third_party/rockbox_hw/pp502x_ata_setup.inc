/* SPDX-License-Identifier: GPL-2.0-or-later
     * Controller clock ordering and PIO timings follow stock Rockbox:
     * firmware/target/arm/pp/ata-pp5020.c, revision
     * 99c18f336c730f533d3b7d2d8951f5dc4a6da802.
     * Copyright (C) 2006 Barry Wardell; GPL-2.0-or-later, see third_party/libmad/COPYING.
     * The timing values describe the controller; Linnet owns the asynchronous
     * transfer state machine. Select the pio80mhz table's
     * interface clock explicitly on our verified 80 MHz operating point;
     * otherwise a native restart leaves slower inherited clocking in place.
     * Transfer mode still comes from IDENTIFY, with DMA disabled above.
     * Recovery Boot can run at a different PLL and keeps its existing setup. */
/* Included inside controller_setup(); R32 and LN_PLL_80 come from the HAL. */
    R32(0xc3000400u)=(R32(0xc3000400u)|2u)&~1u;
    R32(0xc3000028u)=(R32(0xc3000028u)&~0x10008010u)|0x20u;

    if((R32(0x60006020u)&65535u)==0x7777u&&R32(0x60006034u)==LN_PLL_80&&
       (R32(0x6000600cu)&0x02000000u)&&!(R32(0x60006004u)&0x02000000u)&&!(R32(0xc3000400u)&1u)){
        R32(0x600060c4u)=0xc0000000u;
        R32(0xc3000028u)|=0x20000000u;
    }
    R32(0xc3000000u)=0xc293u;R32(0xc3000004u)=0x80002150u;
