#include "mpeg4_vpu.h"
#include "registers.h"
#include "hal/api/platform.h"
#define R(offset) LN_REG(0x39600000u+(offset))
#define CLOCK (*(volatile uint16_t *)(uintptr_t)0x3c50000au)
#define MODE LN_REG(0x38100314u)
static uint32_t gates,started;
static uint16_t clock;
static bool opened,busy;
uint32_t ln_mpeg4_vpu_status,ln_mpeg4_vpu_fault,ln_mpeg4_vpu_vld;
/* Register facts verified against retail 1.1.2 frame setup at 0x08183a74,
 * bitstream setup at 0x08053cdc and completion at 0x0803a0c4. */
static void reset(void){
    R(0x2c)=2;R(0x1c)=0xffffffffu;R(0xc)=0;
    R(0x10000)=0xffffffffu;R(0x10000)=0xffffffffu;
    R(0x50000)=0xffffffffu;R(0x30100)=0xffffffffu;
    R(0x60000)=0xffffffffu;R(0x60000)=0xffffffffu;
    LN_REG(0x38e0200cu)=1u<<13;
}
bool ln_mpeg4_vpu_open(void){
    if(opened)return !busy;
    if((LN_REG(0x3c500040u)&0x44u)!=0x44u)return false;
    gates=LN_CLOCK_GATE0&(31u<<14);clock=CLOCK;
    LN_CLOCK_GATE0|=31u<<14;MODE&=~1u;
    CLOCK=(uint16_t)((clock&~0xb000u)|0x3000u);
    LN_CLOCK_GATE0&=~((7u<<14)|(1u<<18));
    LN_REG(0x38e01014u)=1u<<13;LN_REG(0x38e02004u)|=1u<<13;
    reset();opened=true;ln_mpeg4_vpu_status=ln_mpeg4_vpu_fault=ln_mpeg4_vpu_vld=0;return true;
}
bool ln_mpeg4_vpu_busy(void){return busy;}
void ln_mpeg4_vpu_close(void){
    if(!opened)return;
    reset();LN_CLOCK_GATE0=(LN_CLOCK_GATE0&~(31u<<14))|gates;
    CLOCK=clock;opened=busy=false;
}
bool ln_mpeg4_vpu_start(const struct ln_mpeg4_picture *p){
    if(!opened||busy||!p||!p->width||!p->height||p->width>640||p->height>480||
       ((p->input|p->output|p->reference|p->work)&31u)||
       ((p->output|p->reference|p->work)&1023u)||
       (p->chroma_offset&1023u)||p->chroma_offset<p->width*p->height||
       (p->width|p->height)&15u||p->type>1||!p->bytes||p->bytes>262144u||p->bits>=p->bytes*8u||
       !p->quantizer||p->quantizer>31u||!p->fcode||p->fcode>7u||p->rounding>1u||
       p->dc_threshold>7u||!p->time_bits||p->time_bits>16u)return false;
    reset();uint32_t size=p->chroma_offset,mw=p->width/16,mh=p->height/16;
    R(0x60018)=p->input;R(0x60020)=p->work;R(0x60024)=p->work+1024;
    R(0x60078)=p->work+1024;R(0x6007c)=p->work+1024+0x20000;R(0x60080)=0;
    R(0x10)=0x80000000u;R(0x1001c)=1;R(4)=0x142;
    R(0x30104)=0x14;R(0x10004)=0x3f;R(0x60004)=0x12;
    /* H.263 quantization does not use the MPEG quantization matrices. */
    for(unsigned i=0;i<64;i++){R(0x41200+4*i)=16;R(0x41300+4*i)=16;}
    R(0x6006c)=(mw<<16)|mh;R(0x10)|=((mw-1)<<16)|((mh-1)<<8);R(0x60010)=0x200;
    R(0x6002c)=p->reference;R(0x60030)=0;R(0x60034)=p->reference+size;R(0x60038)=0;
    R(0x6003c)=p->output;R(0x60040)=0;R(0x60044)=p->output+size;R(0x60048)=0;
    R(0x41804)=0x20;R(0x50010)=0xc02|(p->rounding<<2);
    R(0x10010)=(p->dc_threshold<<11)|(p->fcode<<2)|p->type;
    R(0x10014)=p->quantizer;R(0x10034)=3;
    uint32_t mbits=0;for(unsigned n=mw*mh;n;n>>=1)++mbits;
    R(0x1009c)=(p->time_increment<<8)|(mbits<<4)|p->time_bits;
    R(0x100a0)=0;R(0x100a4)=0;R(0x100a8)=0;
    R(0x30100)=0x20;R(0x10000)=4;R(0x60000)=0xffffffffu;
    uint32_t first=p->input+p->bits/8;
    R(0x60018)=first;R(0x6001c)=p->input+p->bytes+0x103;
    R(0x30110)=2;R(0x6000c)=3;
    for(unsigned i=0;i<(first&3u);i++)(void)R(0x30020);
    for(unsigned i=0;i<(p->bits&7u);i++)(void)R(0x30004);
    started=ln_time_us();busy=true;R(0x1000c)=12;return true;
}
int ln_mpeg4_vpu_poll(void){
    if(!busy)return -1;
    uint32_t status=R(0x60000);ln_mpeg4_vpu_status=status;
    if(status&2u){
        ln_mpeg4_vpu_vld=R(0x10000);
        /* A malformed VOP asserts VLD bit 1 even when motion compensation
         * reports completion. Never publish that frame as a reference. */
        bool ok=!(ln_mpeg4_vpu_vld&2u);
        if(!ok)ln_mpeg4_vpu_fault=ln_mpeg4_vpu_vld;
        reset();busy=false;return ok?1:-1;
    }
    if(ln_time_us()-started<50000u)return 0;
    ln_mpeg4_vpu_vld=R(0x10000);ln_mpeg4_vpu_fault=R(0)|0x80000000u;reset();busy=false;return -1;
}
