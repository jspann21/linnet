#include "services/audio/aac/tns.h"

void ln_aac_tns(int32_t *samples,unsigned count,int direction,const int32_t *coefficients,unsigned order)
{
    /* Filter history is the already reconstructed spectrum, in either direction. */
    for(unsigned n=0;n<count;++n,samples+=direction){
        unsigned available=n<order?n:order;
        uint32_t value=(uint32_t)*samples;
        const int32_t *history=samples;
        for(unsigned tap=0;tap<available;++tap){
            history-=direction;
            int64_t product=(int64_t)*history*coefficients[tap+1];
            value-=(uint32_t)(product>>28)+(((uint32_t)product>>27)&1u);
        }
        *samples=(int32_t)value;
    }
}
