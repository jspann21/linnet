#include "services/audio/mp3/reorder.h"

void ln_mp3_reorder(int32_t samples[576],const uint8_t *widths,unsigned mixed)
{
    int32_t reordered[576];
    unsigned first=mixed?36u:0u,skip=0;
    while(skip<first)skip+=*widths++;
    unsigned position[3]={first,first+6u,first+12u};
    unsigned source=first,window=0;
    while(source<576){
        unsigned remaining=*widths++,dest=position[window];
        while(remaining){
            unsigned count=6u-dest%6u;
            if(count>remaining)count=remaining;
            for(unsigned n=0;n<count;++n)reordered[dest++]=samples[source++];
            remaining-=count;
            if(dest%6u==0)dest+=12u;
        }
        position[window]=dest;
        if(++window==3)window=0;
    }
    for(unsigned n=first;n<576;++n)samples[n]=reordered[n];
}
