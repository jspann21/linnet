#include "services/audio/mp3/mp3.h"
#include <inttypes.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORD_BYTES 560u
#define CHECKPOINT_FRAMES 128u

static int seek_file(FILE *file,uint64_t offset){
#ifdef _WIN32
    return _fseeki64(file,(__int64)offset,SEEK_SET);
#else
    return fseeko(file,(off_t)offset,SEEK_SET);
#endif
}
static uint64_t file_size(FILE *file){
#ifdef _WIN32
    if(_fseeki64(file,0,SEEK_END))return 0;
    __int64 size=_ftelli64(file);return size<0?0:(uint64_t)size;
#else
    if(fseeko(file,0,SEEK_END))return 0;
    off_t size=ftello(file);return size<0?0:(uint64_t)size;
#endif
}
static int read_media(void *context,uint64_t offset,uint8_t *bytes,uint32_t count){
    FILE *file=context;
    return seek_file(file,offset)||fread(bytes,1,count,file)!=count?-1:1;
}
static uint32_t crc32(const uint8_t *bytes,uint32_t count){
    uint32_t crc=0xffffffffu;
    for(uint32_t i=0;i<count;++i){crc^=bytes[i];for(uint32_t bit=0;bit<8u;++bit)crc=crc&1u?(crc>>1u)^0xedb88320u:crc>>1u;}
    return crc^0xffffffffu;
}
static uint32_t crc32_continue(uint32_t crc,const uint8_t *bytes,uint32_t count){
    for(uint32_t i=0;i<count;++i){crc^=bytes[i];for(uint32_t bit=0;bit<8u;++bit)crc=crc&1u?(crc>>1u)^0xedb88320u:crc>>1u;}
    return crc;
}
static void le32(uint8_t *out,uint32_t value){for(uint32_t i=0;i<4u;++i)out[i]=(uint8_t)(value>>(i*8u));}
static void le64(uint8_t *out,uint64_t value){for(uint32_t i=0;i<8u;++i)out[i]=(uint8_t)(value>>(i*8u));}
int main(int argc,char **argv){
    if(argc!=3)return 2;
    FILE *source=fopen(argv[1],"rb");
    if(!source)return 3;
    uint64_t bytes=file_size(source);
    if(bytes<1024u||bytes>0xffffffffu){fclose(source);return 4;}
    FILE *output=fopen(argv[2],"wb");
    if(!output){fclose(source);return 5;}
    struct ln_mp3 *decoder=malloc(sizeof(*decoder));
    if(!decoder){fclose(output);fclose(source);return 6;}
    ln_mp3_init(decoder,read_media,source,bytes);
    decoder->seeking=true;decoder->seek_target=UINT64_MAX/4u;
    uint32_t previous=0,records=0,prefix_crc=0xffffffffu;
    uint64_t prefix_at=0,parsed_total=0;
    int16_t pcm[1152u*2u];uint32_t frames=0;
    uint64_t calls=0,max_calls=bytes/2u+65536u;
    int result=0;
    while(calls++<max_calls){
        result=ln_mp3_decode(decoder,pcm,1152u,&frames);
        if(result<0||result==LN_MP3_EOF)break;
        if(decoder->recoveries||decoder->consecutive_recoveries||decoder->stream.freerate)break;
        if(decoder->compressed_frames!=previous){
            previous=decoder->compressed_frames;
            if(decoder->gapless&&!parsed_total)parsed_total=decoder->total_frames;
            if(previous%CHECKPOINT_FRAMES==0u){
                if(!decoder->gapless||decoder->stream.md_len>511u||
                   decoder->pcm_cursor!=decoder->synth.pcm.length||
                   !decoder->stream.next_frame||decoder->stream.next_frame<decoder->input||
                   decoder->stream.next_frame>decoder->input+decoder->valid)break;
                uint64_t next=decoder->read_offset-decoder->valid+
                    (uint64_t)(decoder->stream.next_frame-decoder->input);
                if(next>bytes||bytes-next<512u)break;
                uint8_t probe[512],record[RECORD_BYTES]={0};
                if(read_media(source,next,probe,sizeof probe)!=1)break;
                uint8_t chunk[8192];
                while(prefix_at<next){
                    uint32_t take=next-prefix_at>sizeof chunk?sizeof chunk:(uint32_t)(next-prefix_at);
                    if(read_media(source,prefix_at,chunk,take)!=1)break;
                    prefix_crc=crc32_continue(prefix_crc,chunk,take);prefix_at+=take;
                }
                if(prefix_at!=next)break;
                le64(record,next);le64(record+8,decoder->decoded_frames);
                le32(record+16,decoder->compressed_frames);
                le32(record+20,decoder->trim_remaining);
                record[24]=(uint8_t)decoder->stream.md_len;
                record[25]=(uint8_t)(decoder->stream.md_len>>8u);
                record[26]=(uint8_t)decoder->synth.phase;
                le32(record+28,crc32(probe,sizeof probe));
                memcpy(record+32,decoder->reservoir,decoder->stream.md_len);
                le32(record+544,prefix_crc^0xffffffffu);
                le32(record+556,crc32(record,556u));
                if(fwrite(record,1,sizeof record,output)!=sizeof record)break;
                ++records;
            }
        }
        if(decoder->gapless&&decoder->decoded_frames==decoder->total_frames){result=LN_MP3_EOF;break;}
    }
    uint32_t samples=(decoder->frame.header.flags&MAD_FLAG_LSF_EXT)?576u:1152u;
    bool valid=(result==LN_MP3_EOF&&decoder->gapless&&!decoder->recoveries&&
                !decoder->stream.freerate&&
                decoder->decoded_frames==decoder->total_frames&&records>=2u&&
                parsed_total==decoder->total_frames&&
                (uint64_t)decoder->compressed_frames*samples==
                    decoder->total_frames+decoder->lead_trim+decoder->tail_trim&&
                !ferror(source)&&!ferror(output)&&fflush(output)==0);
    if(valid)printf("{\"records\":%u,\"rate\":%u,\"channels\":%u,\"lead_trim\":%u,\"tail_trim\":%u,\"total_frames\":%" PRIu64 ",\"compressed_frames\":%u,\"samples_per_frame\":%u}\n",
                    records,decoder->rate,decoder->channels,decoder->lead_trim,
                    decoder->tail_trim,decoder->total_frames,decoder->compressed_frames,samples);
    free(decoder);fclose(output);fclose(source);
    return valid?0:7;
}
