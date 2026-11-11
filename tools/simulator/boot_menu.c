#include <stdint.h>
#include <stdio.h>
#include "ui/profiles/color320/boot_menu.h"

struct screen {
    const char *name;
    uint32_t available,selected,starting;
};

int main(int argc,char **argv) {
    if(argc!=2)return 2;
    static const struct screen screens[]={
        {"checking",0u,1u,0u},
        {"linnet",5u,1u,0u},
        {"apple",5u,3u,0u},
        {"rockbox",7u,2u,0u},
        {"starting-linnet",5u,1u,1u},
        {"starting-apple",5u,3u,1u},
    };
    uint16_t frame[LN_MENU_WIDTH*LN_MENU_HEIGHT];
    char path[1024];
    for(unsigned i=0;i<sizeof screens/sizeof screens[0];++i){
        for(uint32_t y=0;y<LN_MENU_HEIGHT;++y)
            ln_menu_320_row(frame+y*LN_MENU_WIDTH,y,screens[i].available,
                screens[i].selected,screens[i].starting);
        int length=snprintf(path,sizeof path,"%s/%s.rgb565",argv[1],screens[i].name);
        if(length<0||(unsigned)length>=sizeof path)return 3;
        FILE *file=fopen(path,"wb");
        if(!file)return 4;
        if(fwrite(frame,sizeof frame,1,file)!=1){fclose(file);return 5;}
        if(fclose(file))return 6;
    }
    return 0;
}
