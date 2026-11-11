#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "services/audio/aac/m4a.h"
#include "boot/include/linnet_bytes.h"
#if defined(_WIN32)
#define seek _fseeki64
#define tell _ftelli64
#define open_read(path) _wfopen(path,L"rb")
#define open_write(path) _wfopen(path,L"wb")
#define ENTRY wmain(int argc,wchar_t **argv)
#else
#define seek fseeko
#define tell ftello
#define open_read(path) fopen(path,"rb")
#define open_write(path) fopen(path,"wb")
#define ENTRY main(int argc,char **argv)
#endif
uint32_t ln_crc32(const uint8_t *b,size_t n){
    uint32_t crc=UINT32_MAX;
    while(n--){crc^=*b++;for(unsigned i=0;i<8;i++)crc=(crc>>1)^(0xedb88320u&-(crc&1u));}
    return crc^UINT32_MAX;
}
static int read_at(void *context,uint64_t offset,uint8_t *buffer,uint32_t bytes){
    FILE *f=context;return seek(f,(int64_t)offset,SEEK_SET)||fread(buffer,1,bytes,f)!=bytes?-1:1;
}
static struct ln_aac_m4a decoder;
int ENTRY {
    if(argc!=2&&argc!=3)return 2;
    FILE *source=open_read(argv[1]);if(!source)return 3;
    if(seek(source,0,SEEK_END))return 3;
    int64_t size=tell(source);if(size<8||size>UINT32_MAX)return 3;
    ln_aac_m4a_init(&decoder,read_at,source,(uint64_t)size);
    int r;do{r=ln_aac_m4a_open_step(&decoder);}while(!r);
    if(argc==2&&r==LN_AAC_IO)return 3;
    /* A size-zero top-level box owns the rest of the file. Preserve it and
     * use normal seeking: an appended sibling would be hidden inside it. */
    if(r!=1||decoder.index_offset||decoder.container.extends_to_eof)return 4;
    if(argc==2){fclose(source);return 0;}
    struct ln_m4a *a=&decoder.container;
    uint32_t count=(a->packets+15u)/16u,bytes=72u+count*32u;
    uint8_t *output=calloc(1,bytes);if(!output)return 5;
    output[0]=bytes>>24;output[1]=bytes>>16;output[2]=bytes>>8;output[3]=bytes;
    output[4]='l';output[5]='n';output[6]='s';output[7]='k';
    uint8_t *h=output+8;
    ln_write_le32(h,0x31534e4cu);ln_write_le32(h+4,16);ln_write_le32(h+8,a->packets);
    ln_write_le32(h+12,count);ln_write_le64(h+16,(uint64_t)size);
    ln_write_le32(h+24,a->rate);ln_write_le32(h+28,a->channels);
    ln_write_le32(h+32,ln_crc32(a->asc,a->asc_bytes));ln_write_le32(h+60,ln_crc32(h,60));
    for(uint32_t packet=0;packet<a->packets;packet++){
        do{r=ln_m4a_read_packet(a);}while(!r);if(r!=1)return 6;
        if(!(packet&15u)){
            uint32_t recurrence[3];if(!ln_aac_checkpoint(&decoder.core,recurrence,false))return 7;
            uint8_t *b=output+72u+(packet/16u)*32u;
            ln_write_le32(b,packet);for(unsigned i=0;i<3;i++)ln_write_le32(b+4u+i*4u,recurrence[i]);
            ln_write_le64(b+16,a->packet_offset);ln_write_le32(b+24,ln_crc32(a->buffer,a->packet_size));
            ln_write_le32(b+28,ln_crc32(b,28));
        }
        if(ln_aac_raw_skip_synthesis(&decoder.core,a->buffer,a->packet_size)!=1)return 8;
    }
    FILE *result=open_write(argv[2]);if(!result)return 9;
    if(fwrite(output,1,bytes,result)!=bytes||fclose(result))return 9;
    fclose(source);free(output);return 0;
}
