/*
 * FLAC (Free Lossless Audio Codec) decoder
 * Copyright (c) 2003 Alex Beregszaszi
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file flac.c
 * FLAC (Free Lossless Audio Codec) decoder
 * @author Alex Beregszaszi
 *
 * For more information on the FLAC format, visit:
 *  http://flac.sourceforge.net/
 *
 * This decoder can be used in 1 of 2 ways: Either raw FLAC data can be fed
 * through, starting from the initial 'fLaC' signature; or by passing the
 * 34-byte streaminfo structure through avctx->extradata[_size] followed
 * by data starting with the 0xFFF8 marker.
 */

#include <stdint.h>
#include <stdbool.h>
#ifndef BUILD_STANDALONE
#include "codeclib.h"
#endif
 
#include "bitstream.h"
#include "golomb.h"

#include "decoder.h"
#include "services/audio/flac/framing.h"
#include "services/audio/flac/fixed.h"
#include "services/audio/flac/lpc.h"

#if defined(CPU_COLDFIRE)
#include "coldfire.h"
#elif defined(CPU_ARM)
#include "arm.h"
#endif

static int decode_residuals(FLACContext *s, int32_t* decoded, int pred_order) ICODE_ATTR_FLAC;
static int decode_residuals(FLACContext *s, int32_t *decoded, int pred_order)
{
    GetBitContext gb = s->gb;
    int i, tmp, partition, method_type, rice_order;
    int rice_bits, rice_esc;
    int samples;

    method_type = get_bits(&gb, 2);
    rice_order  = get_bits(&gb, 4);

    samples   = s->blocksize >> rice_order;
    rice_bits = 4 + method_type;
    rice_esc  = (1 << rice_bits) - 1;

    decoded += pred_order;
    i        = pred_order;

    if (method_type > 1 ||  (samples << rice_order != s->blocksize) || pred_order > samples)
    {
        return -3;
    }

    for (partition = 0; partition < (1 << rice_order); partition++) {
        tmp = get_bits(&gb, rice_bits);
        if (tmp == rice_esc) {
            tmp = get_bits(&gb, 5);
            for (; i < samples; i++)
                *decoded++ = get_sbits(&gb, tmp);
        } else {
            int real_limit = tmp ? (INT_MAX >> tmp) + 2 : INT_MAX;
            for (; i < samples; i++) {
                int v = get_sr_golomb_flac(&gb, tmp, real_limit, 0);
                if ((unsigned) v == 0x80000000){
                    return -3;
                }

                *decoded++ = v;
            }
        }
        i= 0;
    }

    s->gb = gb;

    return 0;
}

//static int decode_subframe_fixed(FLACContext *s, int32_t* decoded, int pred_order, int bps) ICODE_ATTR_FLAC;
int decode_subframe_fixed(FLACContext *s, int32_t* decoded, int pred_order, int bps)
{
    const int blocksize = s->blocksize;
    if (pred_order > blocksize) return -4;
    int i;

    /* warm up samples */
    for (i = 0; i < pred_order; i++)
    {
        decoded[i] = get_sbits(&s->gb, bps);
    }

    if (decode_residuals(s, decoded, pred_order) < 0)
        return -4;

    return pred_order?ln_flac_fixed(decoded,(unsigned)blocksize,(unsigned)pred_order):0;
}

static int decode_subframe_lpc(FLACContext *s, int32_t* decoded, int pred_order, int bps) ICODE_ATTR_FLAC;
static int decode_subframe_lpc(FLACContext *s, int32_t* decoded, int pred_order, int bps)
{
    int i, j;
    int coeff_prec, qlevel;
    int coeffs[32];
    if (pred_order < 1 || pred_order > 32 || pred_order > s->blocksize) return -6;

    /* warm up samples */
    for (i = 0; i < pred_order; i++)
    {
        decoded[i] = get_sbits(&s->gb, bps);
    }

    coeff_prec = get_bits(&s->gb, 4) + 1;
    if (coeff_prec == 16)
    {
        //fprintf(stderr,"invalid coeff precision\n");
        return -6;
    }
    qlevel = get_sbits(&s->gb, 5);
    if (qlevel < 0) 
    {
        //fprintf(stderr,"qlevel %d not supported, maybe buggy stream\n", qlevel);
        return -7;
    }

    for (j = 0; j < pred_order; j++)
    {
        coeffs[j] = get_sbits(&s->gb, coeff_prec);
    }
    
    if (decode_residuals(s, decoded, pred_order) < 0)
        return -8;

    ln_flac_lpc(decoded,(unsigned)s->blocksize,coeffs,(unsigned)pred_order,
                (unsigned)qlevel,s->bps+coeff_prec+av_log2(pred_order)>32,(unsigned)bps);

    return 0;
}

static inline int decode_subframe(FLACContext *s, int channel, int32_t* decoded)
{
    int type, wasted = 0;
    int i, tmp;
    int bps = s->bps;
    if(channel == 0){
        if(s->ch_mode == RIGHT_SIDE)
            bps++;
    }else{
        if(s->ch_mode == LEFT_SIDE || s->ch_mode == MID_SIDE)
            bps++;
    }

    if (get_bits1(&s->gb))
    {
        //fprintf(stderr,"invalid subframe padding\n");
        return -9;
    }
    type = get_bits(&s->gb, 6);

    if (get_bits1(&s->gb))
    {
        wasted = 1;
        while (!get_bits1(&s->gb))
            if (++wasted >= bps || s->gb.error) return -9;
        bps -= wasted;
        if (bps < 1 || s->gb.error) return -9;
        //fprintf(stderr,"%d wasted bits\n", wasted);
    }

//FIXME use av_log2 for types
    if (type == 0)
    {
        //fprintf(stderr,"coding type: constant\n");
        tmp = get_sbits(&s->gb, bps);
        for (i = 0; i < s->blocksize; i++)
            decoded[i] = tmp;
    }
    else if (type == 1)
    {
        //fprintf(stderr,"coding type: verbatim\n");
        for (i = 0; i < s->blocksize; i++)
            decoded[i] = get_sbits(&s->gb, bps);
    }
    else if ((type >= 8) && (type <= 12))
    {
        //fprintf(stderr,"coding type: fixed\n");
        if (decode_subframe_fixed(s, decoded, type & ~0x8, bps) < 0)
            return -10;
    }
    else if (type >= 32)
    {
        //fprintf(stderr,"coding type: lpc\n");
        if (decode_subframe_lpc(s, decoded, (type & ~0x20)+1, bps) < 0)
            return -11;
    }
    else
    {
        //fprintf(stderr,"Unknown coding type: %d\n",type);
        return -12;
    }

    if (wasted && wasted < 32)
    {
        int i;
        for (i = 0; i < s->blocksize; i++)
            decoded[i] = (unsigned)decoded[i] << wasted;
    }

    return 0;
}

static int decode_frame(FLACContext *s,
                        void (*yield)(void)) ICODE_ATTR_FLAC;
static int decode_frame(FLACContext *s,
                        void (*yield)(void))
{
    int res, ch;
    GetBitContext *gb = &s->gb;
    res=ln_flac_frame_header(s);
    if(res<0)return res;

    for (ch=0; ch<s->channels; ++ch) {
        yield();
        if ((res=decode_subframe(s, ch, s->decoded[ch])) < 0)
            return res-100;
    }
    
    yield();
    align_get_bits(gb);

    /* frame footer */
    skip_bits(gb, 16); /* data crc verified by Linnet wrapper */
    if(gb->error) return -19;

    return 0;
}

int flac_decode_frame(FLACContext *s,
                      uint8_t *buf, int buf_size,
                      void (*yield)(void))
{
    int framesize;
    if(s->channels<1||s->channels>2)return -13;

    /* check that there is at least the smallest decodable amount of data.
       this amount corresponds to the smallest valid FLAC frame possible.
       this amount corresponds to the smallest valid FLAC frame possible.
       FF F8 69 02 00 00 9A 00 00 34 46 */
    if (buf_size < MIN_FRAME_SIZE)
        return buf_size;

    init_get_bits(&s->gb, buf, buf_size*8);

    if ((framesize=decode_frame(s,yield)) < 0){
       s->bitstream_size=0;
       s->bitstream_index=0;
       return framesize;
    }

    yield();
    
    s->framesize = (get_bits_count(&s->gb)+7)>>3;

    return 0;
}
