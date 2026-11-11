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
#include "syntax.h"
#include "drc.h"
#include "services/audio/aac/gain.h"

/* static variables */

#define s_drc_info (ln_faad_current()->drc)

drc_info *drc_init(real_t cut, real_t boost)
{
    drc_info *drc = &s_drc_info;
    memset(drc, 0, sizeof(drc_info));

    drc->ctrl1 = cut;
    drc->ctrl2 = boost;

    drc->num_bands = 1;
    drc->band_top[0] = 1024/4 - 1;
    drc->dyn_rng_sgn[0] = 1;
    drc->dyn_rng_ctl[0] = 0;

    return drc;
}

void drc_decode(drc_info *drc, real_t *spec)
{
    unsigned bottom=0;
    if(drc->num_bands==1)drc->band_top[0]=255;
    for(unsigned band=0;band<drc->num_bands;++band){
        unsigned top=4u*(drc->band_top[band]+1u);
        int gain=(int)drc->dyn_rng_ctl[band]-(DRC_REF_LEVEL-drc->prog_ref_level);
        if(drc->dyn_rng_sgn[band])gain=-gain;
        if(top>bottom)ln_aac_gain(spec+bottom,top-bottom,gain);
        bottom=top;
    }
}
