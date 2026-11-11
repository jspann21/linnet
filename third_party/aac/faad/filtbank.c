/* Modified for Linnet, 2026-09-06: bounded freestanding AAC-LC.
 * See ../PROVENANCE.md for adaptation details; original license follows. */
/*
** FAAD2 - Freeware Advanced Audio (AAC) Decoder including SBR decoding
** Copyright (C) 2003-2004 M. Bakker, Ahead Software AG, http://www.nero.com
**  
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
** 
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
** 
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software 
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**
** Any non-GPL usage of this software or parts of this software is strictly
** forbidden.
**
** Commercial non-GPL licensing of this software is possible.
** For more info contact Ahead Software through Mpeg4AAClicense@nero.com.
**
**/

#include "common.h"
#include "workspace.h"
#include "structs.h"
#include "codeclib.h"
#include "codeclib.h"
#ifdef _WIN32_WCE
#define assert(x)
#else
#include "codeclib.h"
#endif

#include "filtbank.h"
#include "decoder.h"
#include "syntax.h"
#include "kbd_win.h"
#include "sine_win.h"


/* static variables */

#define transf_buf (ln_faad_current()->transform)
#ifdef LTP_DEC
static real_t windowed_buf[2*FRAME_LEN] MEM_ALIGN_ATTR = {0};
#endif


#include "services/audio/aac/window.h"

#ifdef LTP_DEC
static INLINE void mdct(fb_info *fb, real_t *in_data, real_t *out_data, uint16_t len)
{
    mdct_info *mdct = NULL;

    switch (len)
    {
    case 2048:
    case 1920:
        mdct = fb->mdct2048;
        break;
    case 256:
    case 240:
        mdct = fb->mdct256;
        break;
#ifdef LD_DEC
    case 1024:
    case 960:
        mdct = fb->mdct1024;
        break;
#endif
    }

    faad_mdct(mdct, in_data, out_data);
}
#endif

void ifilter_bank(uint8_t window_sequence, uint8_t window_shape,
                  uint8_t window_shape_prev, real_t *freq_in,
                  real_t *time_out, real_t *overlap,
                  uint8_t object_type, uint16_t frame_len)
{
    int32_t i, idx0, idx1;
    real_t win0, win1, win2;
     
    const real_t *window_long       = NULL;
    const real_t *window_long_prev  = NULL;
    const real_t *window_short      = NULL;
    const real_t *window_short_prev = NULL;

    int32_t nlong    = frame_len;
    int32_t nshort   = frame_len/8;
    int32_t nflat_ls = (nlong-nshort)/2;

    bool emit_output = true;
#if defined(LC_ONLY_DECODER) && !defined(SBR_DEC) && !defined(PS_DEC) && !defined(DRM_PS)
    if (object_type == 2 && frame_len == 1024)
        emit_output = !ln_faad_current()->discard_output;
    if (!emit_output && ln_faad_current()->skip_synthesis)
        return; /* The caller retains one final synthesized predecessor. */
#endif

#ifdef PROFILE
    int64_t count = faad_get_ts();
#endif

    /* Each transform range read below is overwritten by its IMDCT. Discard
     * paths may leave unused output-only transform ranges untouched. */
    /* select windows of current frame and previous frame (Sine or KBD) */
#ifdef LD_DEC
    if (object_type == LD)
    {
        window_long       = fb->ld_window[window_shape];
        window_long_prev  = fb->ld_window[window_shape_prev];
    } else {
#else
        (void) object_type;
#endif

    /* AAC uses two different window shapes depending on spectal features */
    if (window_shape == 0) {
        window_long  = sine_long_1024;
        window_short = sine_short_128;
    } else {
        window_long  = kbd_long_1024;
        window_short = kbd_short_128;            
    }
    
    if (window_shape_prev == 0) {
        window_long_prev  = sine_long_1024;
        window_short_prev = sine_short_128;
    } else {
        window_long_prev  = kbd_long_1024;
        window_short_prev = kbd_short_128;
    }

#ifdef LD_DEC
    }
#endif

#if 0
    for (i = 0; i < 1024; i++)
    {
        printf("%d\n", freq_in[i]);
    }
#endif

#if 0
    printf("%d %d\n", window_sequence, window_shape);
#endif
    switch (window_sequence)
    {
    case ONLY_LONG_SEQUENCE:
        /* perform iMDCT */
        ff_imdct_calc(11, transf_buf, freq_in);

        /* add second half output of previous frame to windowed output of current frame */
        if (emit_output)
            ln_aac_window_add(time_out, transf_buf, window_long_prev, overlap,  nlong);

        /* window the second half and save as overlap for next frame */
        ln_aac_window_reverse(overlap, transf_buf+nlong, window_long, nlong);

        break;

    case LONG_START_SEQUENCE:
        /* perform iMDCT */
        ff_imdct_calc(11, transf_buf, freq_in);

        /* add second half output of previous frame to windowed output of current frame */
        if (emit_output)
            ln_aac_window_add(time_out, transf_buf, window_long_prev, overlap,  nlong);

        /* window the second half and save as overlap for next frame */
        /* construct second half window using padding with 1's and 0's */
        
        memcpy(overlap, transf_buf+nlong, nflat_ls*sizeof(real_t));

        ln_aac_window_reverse(overlap+nflat_ls, transf_buf+nlong+nflat_ls, window_short, nshort);

        memset(overlap+nflat_ls+nshort, 0, nflat_ls*sizeof(real_t));
        break;

    case EIGHT_SHORT_SEQUENCE:
        /* this could be assemblerized too, but this case is extremely uncommon */   
         
        /* The first three short transforms feed only discarded time output.
         * Overlap reads start at sample 960, inside the fourth transform. */
        if (emit_output) {
            idx0 = 0;       ff_imdct_calc(8, transf_buf            , freq_in       );
            idx0 += nshort; ff_imdct_calc(8, transf_buf + (idx0<<1), freq_in + idx0);
            idx0 += nshort; ff_imdct_calc(8, transf_buf + (idx0<<1), freq_in + idx0);
        }
        idx0 = 3*nshort; ff_imdct_calc(8, transf_buf + (idx0<<1), freq_in + idx0);
        idx0 += nshort; ff_imdct_calc(8, transf_buf + (idx0<<1), freq_in + idx0);
        idx0 += nshort; ff_imdct_calc(8, transf_buf + (idx0<<1), freq_in + idx0);
        idx0 += nshort; ff_imdct_calc(8, transf_buf + (idx0<<1), freq_in + idx0);
        idx0 += nshort; ff_imdct_calc(8, transf_buf + (idx0<<1), freq_in + idx0);

        if (emit_output) {
            /* Add second half output of previous frame to windowed output of current 
             * frame */
            /* Step 1: copy */
            memcpy(time_out, overlap, nflat_ls*sizeof(real_t));
            /* Step 2: First window half, first half of nshort */
            for (i = 0; i < nshort/2; i++) {
                win0 = window_short[nshort-1-i];
                win1 = window_short[i];
                win2 = window_short_prev[i];
                idx0 = nflat_ls + i;
                idx1 = i;
                time_out[idx0] = overlap[idx0] +                                        MUL_F(transf_buf[idx1], win2); idx0 += nshort; idx1 += (nshort<<1);
                time_out[idx0] = overlap[idx0] + MUL_F(transf_buf[idx1-nshort], win0) + MUL_F(transf_buf[idx1], win1); idx0 += nshort; idx1 += (nshort<<1);
                time_out[idx0] = overlap[idx0] + MUL_F(transf_buf[idx1-nshort], win0) + MUL_F(transf_buf[idx1], win1); idx0 += nshort; idx1 += (nshort<<1);
                time_out[idx0] = overlap[idx0] + MUL_F(transf_buf[idx1-nshort], win0) + MUL_F(transf_buf[idx1], win1); idx0 += nshort; idx1 += (nshort<<1);
                time_out[idx0] = overlap[idx0] + MUL_F(transf_buf[idx1-nshort], win0) + MUL_F(transf_buf[idx1], win1);
            }
            /* Step 3: First window half, second half of nshort */
            for (; i < nshort; i++) {
                win0 = window_short[nshort-1-i];
                win1 = window_short[i];
                idx0 = nflat_ls + i;
                idx1 = i;
                time_out[idx0] = overlap[idx0] +                                        MUL_F(transf_buf[idx1], win1); idx0 += nshort; idx1 += (nshort<<1);
                time_out[idx0] = overlap[idx0] + MUL_F(transf_buf[idx1-nshort], win0) + MUL_F(transf_buf[idx1], win1); idx0 += nshort; idx1 += (nshort<<1);
                time_out[idx0] = overlap[idx0] + MUL_F(transf_buf[idx1-nshort], win0) + MUL_F(transf_buf[idx1], win1); idx0 += nshort; idx1 += (nshort<<1);
                time_out[idx0] = overlap[idx0] + MUL_F(transf_buf[idx1-nshort], win0) + MUL_F(transf_buf[idx1], win1);
            }

        }

        /* Window the second half and save as overlap for next frame */
        /* Step 1: Second window half, first half of nshort */
        for (i = 0; i < nshort/2; i++) {
            win0 = window_short[nshort-1-i];
            win1 = window_short[i];
            idx0 = nflat_ls + 5*nshort + i - nlong;
            idx1 = nshort*10 + i;
            overlap[idx0] = MUL_F(transf_buf[idx1-nshort], win0) + MUL_F(transf_buf[idx1], win1); idx0 += nshort; idx1 += (nshort<<1);
            overlap[idx0] = MUL_F(transf_buf[idx1-nshort], win0) + MUL_F(transf_buf[idx1], win1); idx0 += nshort; idx1 += (nshort<<1);
            overlap[idx0] = MUL_F(transf_buf[idx1-nshort], win0) + MUL_F(transf_buf[idx1], win1); idx0 += nshort; idx1 += (nshort<<1);
            overlap[idx0] = MUL_F(transf_buf[idx1-nshort], win0);
        }
        /* Step 2: Second window half, second half of nshort */
        for (; i < nshort; i++) {
            win0 = window_short[nshort-1-i];
            win1 = window_short[i];
            idx0 = nflat_ls + 4*nshort + i - nlong;
            idx1 = nshort*8 + i;
            overlap[idx0] = MUL_F(transf_buf[idx1-nshort], win0) + MUL_F(transf_buf[idx1], win1); idx0 += nshort; idx1 += (nshort<<1);
            overlap[idx0] = MUL_F(transf_buf[idx1-nshort], win0) + MUL_F(transf_buf[idx1], win1); idx0 += nshort; idx1 += (nshort<<1);
            overlap[idx0] = MUL_F(transf_buf[idx1-nshort], win0) + MUL_F(transf_buf[idx1], win1); idx0 += nshort; idx1 += (nshort<<1);
            overlap[idx0] = MUL_F(transf_buf[idx1-nshort], win0) + MUL_F(transf_buf[idx1], win1); idx0 += nshort; idx1 += (nshort<<1);
            overlap[idx0] = MUL_F(transf_buf[idx1-nshort], win0);
        }
        /* Step 3: Set to zero */
        memset(overlap+nflat_ls+nshort, 0, nflat_ls*sizeof(real_t));

        break;

    case LONG_STOP_SEQUENCE:
        /* perform iMDCT */
        ff_imdct_calc(11, transf_buf, freq_in);

        if (emit_output) {
            /* add second half output of previous frame to windowed output of current frame */
            /* construct first half window using padding with 1's and 0's */
            memcpy(time_out, overlap, nflat_ls*sizeof(real_t));

            ln_aac_window_add(time_out+nflat_ls, transf_buf+nflat_ls, window_short_prev, overlap+nflat_ls,  nshort);

            /* nflat_ls can be divided by 2. */
            idx0 = nflat_ls + nshort;
            for (i = 0; i < nflat_ls; i+=2) {
                time_out[idx0] = overlap[idx0] + transf_buf[idx0]; idx0++;
                time_out[idx0] = overlap[idx0] + transf_buf[idx0]; idx0++;
            }

        }

        /* window the second half and save as overlap for next frame */
        ln_aac_window_reverse(overlap, transf_buf+nlong, window_long, nlong);
        break;
    }

#if 0
    for (i = 0; i < 1024; i++)
    {
        printf("%d\n", time_out[i]);
        //printf("0x%.8X\n", time_out[i]);
    }
#endif


#ifdef PROFILE
    count = faad_get_ts() - count;
    fb->cycles += count;
#endif
}


#ifdef LTP_DEC
/* only works for LTP -> no overlapping, no short blocks */
void filter_bank_ltp(fb_info *fb, uint8_t window_sequence, uint8_t window_shape,
                     uint8_t window_shape_prev, real_t *in_data, real_t *out_mdct,
                     uint8_t object_type, uint16_t frame_len)
{
    int16_t i;

    const real_t *window_long = NULL;
    const real_t *window_long_prev = NULL;
    const real_t *window_short = NULL;
    const real_t *window_short_prev = NULL;

    uint16_t nlong = frame_len;
    uint16_t nshort = frame_len/8;
    uint16_t nflat_ls = (nlong-nshort)/2;

    //assert(window_sequence != EIGHT_SHORT_SEQUENCE);

    memset(windowed_buf,0,sizeof(windowed_buf));
#ifdef LD_DEC
    if (object_type == LD)
    {
        window_long       = fb->ld_window[window_shape];
        window_long_prev  = fb->ld_window[window_shape_prev];
    } else {
#else
        (void) object_type;
#endif
        window_long       = fb->long_window[window_shape];
        window_long_prev  = fb->long_window[window_shape_prev];
        window_short      = fb->short_window[window_shape];
        window_short_prev = fb->short_window[window_shape_prev];
#ifdef LD_DEC
    }
#endif

    switch(window_sequence)
    {
    case ONLY_LONG_SEQUENCE:
        for (i = nlong-1; i >= 0; i--)
        {
            windowed_buf[i] = MUL_F(in_data[i], window_long_prev[i]);
            windowed_buf[i+nlong] = MUL_F(in_data[i+nlong], window_long[nlong-1-i]);
        }
        mdct(fb, windowed_buf, out_mdct, 2*nlong);
        break;

    case LONG_START_SEQUENCE:
        for (i = 0; i < nlong; i++)
            windowed_buf[i] = MUL_F(in_data[i], window_long_prev[i]);
        for (i = 0; i < nflat_ls; i++)
            windowed_buf[i+nlong] = in_data[i+nlong];
        for (i = 0; i < nshort; i++)
            windowed_buf[i+nlong+nflat_ls] = MUL_F(in_data[i+nlong+nflat_ls], window_short[nshort-1-i]);
        for (i = 0; i < nflat_ls; i++)
            windowed_buf[i+nlong+nflat_ls+nshort] = 0;
        mdct(fb, windowed_buf, out_mdct, 2*nlong);
        break;

    case LONG_STOP_SEQUENCE:
        for (i = 0; i < nflat_ls; i++)
            windowed_buf[i] = 0;
        for (i = 0; i < nshort; i++)
            windowed_buf[i+nflat_ls] = MUL_F(in_data[i+nflat_ls], window_short_prev[i]);
        for (i = 0; i < nflat_ls; i++)
            windowed_buf[i+nflat_ls+nshort] = in_data[i+nflat_ls+nshort];
        for (i = 0; i < nlong; i++)
            windowed_buf[i+nlong] = MUL_F(in_data[i+nlong], window_long[nlong-1-i]);
        mdct(fb, windowed_buf, out_mdct, 2*nlong);
        break;
    }
}
#endif
