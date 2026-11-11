#include "services/audio/vorbis/coupling.h"

void ln_vorbis_couple(int32_t *magnitude,int32_t *angle,unsigned count)
{
    #pragma clang loop unroll_count(4)
    for(unsigned i=0;i<count;++i){
        int32_t m=magnitude[i],a=angle[i];
        uint32_t delta=m>0?(uint32_t)a:0u-(uint32_t)a;
        if(a>0)angle[i]=(int32_t)((uint32_t)m-delta);
        else{magnitude[i]=(int32_t)((uint32_t)m+delta);angle[i]=m;}
    }
}
