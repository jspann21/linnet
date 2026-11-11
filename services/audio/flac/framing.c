#include "services/audio/flac/framing.h"
#include "third_party/flac/decoder.h"

int ln_flac_frame_header(struct FLACContext *s)
{
    const uint8_t *p=s->gb.buffer;
    unsigned bytes=(unsigned)s->gb.size_in_bits/8u;
    if(bytes<4||p[0]!=255||(p[1]&254u)!=248)return -41;
    unsigned block_code=p[2]>>4,rate_code=p[2]&15u;
    unsigned assignment=p[3]>>4,size_code=(p[3]>>1)&7u;
    unsigned mode=0;
    if(assignment<8){if(assignment+1u!=(unsigned)s->channels)return -13;}
    else if(assignment<=10&&s->channels==2)mode=assignment-7u;
    else return -13;
    static const uint8_t depths[8]={0,8,12,0,16,20,24,0};
    if(size_code==3||size_code==7)return -14;
    unsigned bits=size_code?depths[size_code]:(unsigned)s->bps;
    if(p[3]&1u)return -15;

    unsigned at=4;
    if(at==bytes)return -15;
    unsigned first=p[at++],following=0;
    uint32_t number=first;
    if(first&128u){
        unsigned mask=128;
        while(first&mask){++following;mask>>=1;}
        if(following<2||following>6)return -15;
        number=first&(mask-1u);--following;
        if(following>bytes-at)return -15;
        while(following--){unsigned next=p[at++];if((next&192u)!=128u)return -15;number=(number<<6)|(next&63u);}
    }
    if(s->min_blocksize==s->max_blocksize)number*=(unsigned)s->min_blocksize;
    unsigned block;
    if(!block_code)block=(unsigned)s->min_blocksize;
    else if(block_code==1)block=192;
    else if(block_code<=5)block=576u<<(block_code-2u);
    else if(block_code<=7){
        unsigned count=block_code-5u;
        if(count>bytes-at)return -16;
        block=p[at++];if(count==2)block=(block<<8)|p[at++];++block;
    }else block=256u<<(block_code-8u);
    if(!block||block>MAX_BLOCKSIZE||block>(unsigned)s->max_blocksize)return -16;

    static const uint32_t rates[12]={0,88200,176400,192000,8000,16000,22050,24000,32000,44100,48000,96000};
    unsigned rate=(unsigned)s->samplerate;
    if(rate_code==15)return -17;
    if(rate_code&&rate_code<12)rate=rates[rate_code];
    else if(rate_code>=12){
        unsigned count=rate_code==12?1u:2u;
        if(count>bytes-at)return -18;
        rate=p[at++];if(count==2)rate=(rate<<8)|p[at++];
        if(rate_code==12)rate*=1000u;
        if(rate_code==14)rate*=10u;
    }
    if(at==bytes||bits<4||bits>24||bits!=(unsigned)s->bps||rate!=(unsigned)s->samplerate)return -18;
    ++at;
    static const uint8_t crc_nibble[16]={0x00,0x07,0x0e,0x09,0x1c,0x1b,0x12,0x15,0x38,0x3f,0x36,0x31,0x24,0x23,0x2a,0x2d};
    unsigned crc=0;
    for(unsigned i=0;i<at;++i){
        crc^=p[i];crc=((crc<<4)&255u)^crc_nibble[crc>>4];
        crc=((crc<<4)&255u)^crc_nibble[crc>>4];
    }
    if(crc)return -18;
    s->blocksize=(int)block;s->ch_mode=(enum decorrelation_type)mode;
    s->samplenumber=number;s->gb.index=(int)(at*8u);
    return 0;
}
