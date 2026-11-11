#include "boot/include/linnet_bytes.h"
static void copy(void *out,const void *in,uint32_t bytes){
#if defined(__arm__)
    extern void ln_memory_copy(void *,const void *,uint32_t);ln_memory_copy(out,in,bytes);
#else
    __builtin_memcpy(out,in,bytes);
#endif
}
__attribute__((noinline)) void ln_video_lz_copy_long(uint8_t*write,uint32_t offset,uint32_t remaining){
        /* ARM7 cannot load unaligned words. Align only the destination,
         * then use aligned loads or explicit byte assembly for the source.
         * Each four-byte load completes before its store: offsets >=4 may
         * overlap across successive words, exactly as LZ forward copying. */
        typedef uint32_t match_word __attribute__((may_alias));
        const uint8_t*match=write-offset;
        while(((uintptr_t)write&3u)&&remaining){*write++=*match++;--remaining;}
        if(offset<=2u||offset==4u){
            uint32_t pattern;
            if(offset==4u)pattern=*(const match_word*)match;
            else {pattern=match[0];
                if(offset==2u)pattern|=(uint32_t)match[1]<<8;else pattern|=pattern<<8;
                pattern|=pattern<<16;}
            while(remaining>=16u){match_word*words=(match_word*)write;
                words[0]=pattern;words[1]=pattern;words[2]=pattern;words[3]=pattern;
                write+=16u;match+=16u;remaining-=16u;}
            while(remaining>=4u){*(match_word*)write=pattern;write+=4u;match+=4u;remaining-=4u;}
        }else if(remaining>=64u&&(offset==3u||!((uintptr_t)match&3u))){
            /* Preserve the ARM bulk copier for long aligned spans, growing
             * completed output only through disjoint source/destination runs. */
            uint32_t copied=offset<remaining?offset:remaining;copy(write,match,copied);
            while(copied<remaining){uint32_t n=copied;if(n>remaining-copied)n=remaining-copied;
                copy(write+copied,write,n);copied+=n;}
            write+=remaining;match+=remaining;remaining=0;
        }else if(offset>=4u){
            if(!((uintptr_t)match&3u)){
                while(remaining>=4u){*(match_word*)write=*(const match_word*)match;
                    write+=4u;match+=4u;remaining-=4u;}
            }else if(!((uintptr_t)match&1u)){
                typedef uint16_t match_half __attribute__((may_alias));
                while(remaining>=4u){const match_half *halves=(const match_half*)match;
                    uint32_t word=(uint32_t)halves[0]|((uint32_t)halves[1]<<16);
                    *(match_word*)write=word;write+=4u;match+=4u;remaining-=4u;}
            }else{
                while(remaining>=4u){uint32_t word=ln_read_le32(match);*(match_word*)write=word;
                    write+=4u;match+=4u;remaining-=4u;}
            }
        }
        while(remaining){*write++=*match++;--remaining;}
}
