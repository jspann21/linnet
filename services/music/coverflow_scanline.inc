/* Host-baked scanlines retain all 33 poses. Each run stores its exclusive
 * column end and source row, so projection and row offsets stay out of pixel loops. */
struct ln_coverflow_scanline {uint16_t offset;uint8_t left,width,runs,span_end;};
extern const struct ln_coverflow_scanline ln_coverflow_rows[33][190];
extern const uint8_t ln_coverflow_samples[];
extern const uint16_t ln_coverflow_settled_offsets[16][190];
extern const uint16_t ln_coverflow_settled_spans[];
#if defined(LINNET_SOC_PP5022)
extern const uint8_t ln_coverflow_source_map[];
extern const uint16_t ln_coverflow_source_offsets[2][33];
extern const uint16_t ln_coverflow_tilted_samples[];
#else
extern const uint8_t ln_coverflow_sources[33][COVER_WIDTH];
#endif
#if defined(LINNET_SOC_S5L8702)
extern const uint8_t ln_coverflow_mirror_sources[33][COVER_WIDTH];
#endif
/* The ambient palette is unchanged on redraws at the same album position. */
static bool glow_palette_valid;
static uint32_t glow_ambient[3], glow_pairs[2][192];

void ln_coverflow_draw(const struct ln_ui_canvas_context *c){
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t began=ln_time_us();
#endif
    struct card {int32_t x,width;uint32_t pose;bool mirror,preview,decoded;
        const uint16_t *tilted;
#if defined(LINNET_SOC_S5L8702) || defined(LINNET_SOC_PP5022)
        const uint16_t *palette;const uint8_t *pixels,*indices,*sources;
#else
        const uint16_t *palette;const uint8_t *pixels,*indices;uint8_t sources[COVER_WIDTH];
#endif
    } cards[7];
    uint32_t card_count=0;
    int32_t center=(position+128)/256;
#if defined(LINNET_SOC_PP5022) || defined(LINNET_SOC_S5L8702)
    int16_t card_map[7];
    for(uint32_t n=0;n<7;n++)card_map[n]=-1;
    bool settled=count&&position>=0&&!(position&255)&&center>=0&&(uint32_t)center<count;
    uint32_t settled_case=0;
    if(settled){uint32_t left=(uint32_t)center,right=count-1u-left;
        settled_case=(left<3u?left:3u)*4u+(right<3u?right:3u);}
#endif
#if defined(LINNET_COVERFLOW_PACKED)
    packed_frame_begin(center);
#endif
    for(int32_t layer=0;layer<=3;layer++)for(int32_t side=1;side>=-1;side-=2){
        if(!layer&&side==1)continue;
        int32_t index=center+layer*side;if(index<0||(uint32_t)index>=count)continue;
        struct card *card=&cards[card_count++];
#if defined(LINNET_SOC_PP5022) || defined(LINNET_SOC_S5L8702)
        card_map[index-center+3]=(int16_t)(card_count-1u);
#endif
        int32_t distance=index*256-position,ad=distance<0?-distance:distance;
        card->pose=ad>=256?32u:(uint32_t)(ad+4)/8u;
        card->width=ln_coverflow_widths[card->pose];card->mirror=distance>=0;
        card->x=160+(distance<0?-1:1)*(ad<=256?ad*69/256:69+(ad-256)*25/256)*320/220-card->width/2;
        const uint8_t *r=row((uint32_t)index),*packed=0;
        const uint16_t *pixels=0;
        card->tilted=r&&card->pose==32u?tilted_pixels(ln_read_le32(r+248)):0;
        if(r&&!card->tilted){
            uint32_t art=ln_read_le32(r+248);packed=retained_art(art);
            pixels=raw_rgb_pixels(art);
            int32_t slot=find_art(art);
#if defined(LINNET_COVERFLOW_PACKED)
            if(packed_mode&&!packed_pixels)slot=-1;
#endif
            if(!pixels&&!packed_mode&&slot<0&&layer<2)slot=decode_art(art);
            if(!pixels&&slot>=0){pixels=cache_slot_pixels((uint32_t)slot);cache[slot].age=++age;}}
        card->pixels=(const uint8_t *)pixels;card->indices=packed?packed+512:r?r+256:0;
        card->decoded=pixels!=0;
        card->preview=!packed;
        card->palette=packed?(const uint16_t*)packed:r?(const uint16_t*)(header+512):0;
#if defined(LINNET_SOC_PP5022)
        card->sources=ln_coverflow_source_map+ln_coverflow_source_offsets[card->mirror?1:0][card->pose];
#elif defined(LINNET_SOC_S5L8702)
        card->sources=card->mirror?ln_coverflow_mirror_sources[card->pose]:ln_coverflow_sources[card->pose];
#else
        for(int32_t x=0;x<card->width;x++){
            uint32_t sx=ln_coverflow_sources[card->pose][card->mirror?card->width-1-x:x];
            if(card->mirror)sx=TEXTURE_WIDTH-1u-sx;
            card->sources[x]=(uint8_t)sx;
        }
#endif
    }
    uint32_t ambient[3]={0};
    if(count){
        uint32_t left=(uint32_t)position/256u,blend=(uint32_t)position&255u,a[3],b[3];
        ambient_color(left,a);ambient_color(left+1u<count?left+1u:left,b);
        for(uint32_t ch=0;ch<3;ch++)a[ch]=(a[ch]*(256u-blend)+b[ch]*blend)>>8;
        int32_t mean=(int32_t)(a[0]+a[1]+a[2])/3;
        for(uint32_t ch=0;ch<3;ch++){int32_t v=mean+((int32_t)a[ch]-mean)*3/2;a[ch]=v<0?0u:v>255?255u:(uint32_t)v;}
        for(uint32_t ch=0;ch<3;ch++)ambient[ch]=a[ch];
    }
    if(!glow_palette_valid||ambient[0]!=glow_ambient[0]||ambient[1]!=glow_ambient[1]||ambient[2]!=glow_ambient[2]){
        uint16_t colors[256];
        uint32_t rs=ambient[0]*70u*31u*2048u/(63u*100u*255u),gs=ambient[1]*70u*63u*2048u/(63u*100u*255u),bs=ambient[2]*70u*31u*2048u/(63u*100u*255u);
        for(uint32_t k=0;k<64;k++)for(uint32_t d=0;d<4;d++){
            uint32_t bias=d*512u+256u;
            colors[k*4+d]=(uint16_t)(((rs*k+bias)>>11)<<11|((gs*k+bias)>>11)<<5|((bs*k+bias)>>11));
        }
        for(uint32_t parity=0;parity<2;parity++)for(uint32_t k=0;k<192;k++)
            glow_pairs[parity][k]=(uint32_t)colors[ln_coverflow_glow_colors[parity][k][0]]|
                ((uint32_t)colors[ln_coverflow_glow_colors[parity][k][1]]<<16);
        for(uint32_t ch=0;ch<3;ch++)glow_ambient[ch]=ambient[ch];
        glow_palette_valid=true;
    }
    int32_t bottom=c->clip_y+c->clip_h;if(bottom>190)bottom=190;
#if defined(LINNET_SOC_PP5022) || defined(LINNET_SOC_S5L8702)
    struct span {int16_t left,right,card;} spans[15];
    uint32_t spans_count=0;
    int32_t span_next=c->clip_y;
#endif
    for(int32_t y=c->clip_y;y<bottom;y++){
#if defined(LINNET_SOC_PP5022) || defined(LINNET_SOC_S5L8702)
        if(y>=span_next){
            span_next=ln_coverflow_rows[0][y].span_end;
#else
        struct span {int16_t left,right,card;} spans[15];
        uint32_t spans_count;
#endif
        spans[0]=(struct span){c->clip_x,c->clip_x+c->clip_w,-1};
        spans_count=1;
#if defined(LINNET_SOC_PP5022) || defined(LINNET_SOC_S5L8702)
        if(settled){
            const uint16_t *recipe=ln_coverflow_settled_spans+ln_coverflow_settled_offsets[settled_case][y];
            int32_t left=0,clip_end=c->clip_x+c->clip_w;spans_count=0;
            do {
                uint32_t token=*recipe++,label=token&7u;int32_t right=(int32_t)(token>>3);
                int32_t lo=left>c->clip_x?left:c->clip_x,hi=right<clip_end?right:clip_end;
                if(lo<hi)spans[spans_count++]=(struct span){lo,hi,label==7u?-1:card_map[label]};
                left=right;
            }while(left<320);
        }else
#endif
        for(uint32_t n=0;n<card_count;n++){
            const struct card *card=&cards[n];const struct ln_coverflow_scanline *line=&ln_coverflow_rows[card->pose][y];
            if(!line->width)continue;
            int32_t left=card->x+(card->mirror?card->width-line->left-line->width:line->left),right=left+line->width;
            for(uint32_t j=0;j<spans_count;j++){
                struct span old=spans[j];if(old.card>=0||left>=old.right||right<=old.left)continue;
                int32_t lo=left>old.left?left:old.left,hi=right<old.right?right:old.right;
                uint32_t extra=(lo>old.left)+(hi<old.right);
                for(uint32_t k=spans_count;k>j+1;k--)spans[k-1+extra]=spans[k-1];
                spans_count+=extra;
                if(lo>old.left)spans[j++]=(struct span){old.left,lo,-1};
                spans[j]=(struct span){lo,hi,(int16_t)n};
                if(hi<old.right)spans[++j]=(struct span){hi,old.right,-1};
            }
        }
#if defined(LINNET_SOC_PP5022) || defined(LINNET_SOC_S5L8702)
        }
#endif
        for(uint32_t n=0;n<spans_count;n++){
            const struct span *span=&spans[n];uint16_t *dest=c->pixels+y*320+span->left;
            if(span->card<0){
                const uint8_t *mask=ln_coverflow_glow_pairs[y]+span->left/2;
                const uint32_t *palette=glow_pairs[y&1];int32_t x=span->left;
                if(x&1){*dest++=(uint16_t)(palette[*mask++]>>16);++x;}
                typedef uint32_t pixel_pair __attribute__((may_alias));
                pixel_pair *output=(pixel_pair *)dest;
#if defined(LINNET_SOC_PP5022) || defined(LINNET_SOC_S5L8702)
                for(;x+7<span->right;x+=8){
                    output[0]=palette[mask[0]];
                    output[1]=palette[mask[1]];
                    output[2]=palette[mask[2]];
                    output[3]=palette[mask[3]];
                    output+=4;mask+=4;
                }
#endif
                for(;x+1<span->right;x+=2)*output++=palette[*mask++];
                if(x<span->right)*(uint16_t *)output=(uint16_t)palette[*mask];
            }else{
                const struct card *card=&cards[span->card];
                const uint8_t *sources=card->sources;
                const struct ln_coverflow_scanline *line=&ln_coverflow_rows[card->pose][y];
                int32_t x=span->left-card->x,end=span->right-card->x;
                if(card->tilted){
                    const uint16_t *source=card->tilted+
                        ((card->mirror?TILTED_HEIGHT:0u)+(uint32_t)y)*TILTED_STRIDE+x;
                    /* Host-prepared RGB565 includes tilt and reflection.
                     * No palette lookup or scattered sampling in this path. */
                    extern void ln_memory_copy(void *,const void *,uint32_t);
                    ln_memory_copy(dest,source,(uint32_t)(end-x)*2u);
                    continue;
                }
#if defined(LINNET_SOC_PP5022)
                if(card->decoded&&card->pose==32u){
                    /* Projection, run boundaries and reflection are resolved
                     * by the host compiler. Only sample the prepared RGB tile. */
                    const uint16_t *source=(const uint16_t *)card->pixels;
                    const uint16_t *indices=ln_coverflow_tilted_samples+
                        ((card->mirror?190u:0u)+(uint32_t)y)*(uint32_t)card->width;
                    for(;x+3<end;x+=4){
                        dest[0]=source[indices[x]];dest[1]=source[indices[x+1]];
                        dest[2]=source[indices[x+2]];dest[3]=source[indices[x+3]];dest+=4;
                    }
                    for(;x<end;++x)*dest++=source[indices[x]];
                    continue;
                }
#endif
                const uint8_t *samples=ln_coverflow_samples+line->offset;
                int32_t u=card->mirror?card->width-1-x:x,run;
                /* Begin at the nearer end of the host-prepared runs. A
                 * mirrored card usually begins at its final run. */
                if(u>card->width/2){run=line->runs-1;while(run&&samples[(run-1)*2]>u)--run;}
                else {run=0;while(samples[run*2]<=u)++run;}
                while(x<end){
                    int32_t left=run?samples[(run-1)*2]:line->left;
                    int32_t length=card->mirror?u-left+1:samples[run*2]-u;
                    if(length>end-x)length=end-x;
                    int32_t limit=x+length;uint32_t sy=samples[run*2+1];
                    if(card->decoded){
                        const uint16_t *source=(const uint16_t *)card->pixels+sy*TEXTURE_WIDTH;
                        if(card->pose==0){
#if defined(__arm__)
                            extern void ln_memory_copy(void *,const void *,uint32_t);
                            ln_memory_copy(dest,source+x,(uint32_t)(limit-x)*2u);
#else
                            __builtin_memcpy(dest,source+x,(uint32_t)(limit-x)*2u);
#endif
                            dest+=limit-x;x=limit;
                        }else{
#if defined(LINNET_SOC_PP5022)
                            for(;x+3<limit;x+=4){
                                dest[0]=source[sources[x]];
                                dest[1]=source[sources[x+1]];
                                dest[2]=source[sources[x+2]];
                                dest[3]=source[sources[x+3]];
                                dest+=4;
                            }
#endif
                            for(;x<limit;x++)*dest++=source[sources[x]];
                        }
                    }else if(!card->palette){
                        uint16_t pixel=sy<TEXTURE_WIDTH?0x18e4u:0;
                        for(;x<limit;x++)*dest++=pixel;
                    }else if(card->preview){
                        sy=ln_coverflow_preview_y[sy];
                        for(;x<limit;x++)*dest++=card->palette[card->indices[ln_coverflow_preview_x[sources[x]]*32u+sy]];
                    }else{
                        for(;x<limit;x++)*dest++=card->palette[card->indices[sy*TEXTURE_WIDTH+sources[x]]];
                    }
                    if(card->mirror){u-=length;--run;}else{u+=length;++run;}
                }
            }
        }
    }
#if defined(LINNET_COVERFLOW_PACKED)
    packed_drawing=false;
#endif
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t elapsed=ln_time_us()-began;
    ++render_calls;render_us+=elapsed;if(elapsed>render_max_us)render_max_us=elapsed;
#endif
}
