/* SPDX-License-Identifier: GPL-2.0-or-later
 * Panel initialization sequences/data adapted from stock Rockbox:
 * firmware/target/arm/s5l8702/ipod6g/lcd-6g.c, revision
 * 99c18f336c730f533d3b7d2d8951f5dc4a6da802.
 * Copyright (C) 2009 Dave Chapman; GPL-2.0-or-later, see third_party/libmad/COPYING.
 * Register/gamma values describe the panels. Linnet's bounded polling, DMA
 * ownership and deadline state machine implement their own transfer driver. */
/* Included inside configure(); send() is supplied by the Linnet driver. */
    /* N25 panel-1 electrical timing and gamma register values. These are
     * panel configuration data, separate from the S5L transfer driver. */
    static const uint8_t timing[]={0x3a,0x3a,0x80,0x80,0x0a,0x0a,0x0a,0x0a,
        0x0a,0x0a,0x0a,0x0a,0x3c,0x30,0x0f,0,1,0x54,6,0x66,0x66};
    static const uint8_t drive[]={0x14,0x59,0,0x15,0x57,0x27,4,0x85,
        0x14,0x59,0,0x15,0x57,0x27,4,0x85,0x14,9,0x15,0x57,0x27,4,5,
        0x14,9,0x15,0x57,0x27,4,5};
    static const uint8_t gamma[3][13]={
        {0,0,0,5,0x0b,0x12,0x16,0x1f,0x25,0x22,0x24,0x29,0x1c},
        {8,1,1,6,0x0b,0x11,0x15,0x1f,0x27,0x26,0x29,0x2f,0x1e},
        {7,1,1,5,9,0x0f,0x13,0x1e,0x26,0x25,0x28,0x2e,0x1e}};
    uint8_t value=0xd8;
    if(!send(0xb0,timing,sizeof(timing)) || !send(0xb8,&value,1) ||
       !send(0xb1,drive,sizeof(drive)))return false;
    value=1;if(!send(0xd2,&value,1))return false;
    for(unsigned i=0;i<6;i++)if(!send((uint8_t)(0xe0+i),gamma[i%3],13))return false;
    value=6;if(!send(0x3a,&value,1))return false;
    value=0;if(!send(0xc2,&value,1) || !send(0x35,&value,1) || !send(0x11,0,0))return false;
