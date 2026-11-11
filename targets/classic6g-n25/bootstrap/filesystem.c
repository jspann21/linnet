#include "services/storage/fat32.h"
#include <stddef.h>
void ln_storage_probe(unsigned operation,uint32_t lba,uint32_t result[16]);
static struct ln_fat32 volume;
static bool pending;
static uint64_t pending_lba;
static uint8_t *pending_buffer;
static uint32_t pending_generation;
static int sector_io(void *context,enum ln_fat32_io operation,uint64_t lba,uint8_t *buffer,uint32_t generation) {
    (void)context;
    if(operation!=LN_FAT32_READ || lba>0x0fffffffu || !buffer)return -1;
    uint32_t status[16]={0};
    if(!pending) {
        pending=true;pending_lba=lba;pending_buffer=buffer;pending_generation=generation;
        ln_storage_probe(4,(uint32_t)lba,status);
    } else {
        if(lba!=pending_lba || buffer!=pending_buffer || generation!=pending_generation)return -1;
        ln_storage_probe(3,0,status);
    }
    if(status[4]){pending=false;return -1;}
    if(status[3]!=8)return 0;
    if(status[5]!=256){pending=false;return -1;}
    const uint8_t *source=(const uint8_t *)(uintptr_t)status[7];
    for(unsigned n=0;n<512;n++)buffer[n]=source[n];
    pending=false;return 1;
}
void ln_filesystem_probe(const uint32_t *request,uint32_t result[16]) {
    int status;
    if(request[0]==5) {
        pending=false;
        struct ln_fat32_config config={.io=sector_io,.disk_sectors=request[3],.writable=false};
        ln_fat32_init(&volume,&config);
        status=ln_fat32_mount(&volume,request[1],request[2],1)?0:LN_FAT32_ARGUMENT;
    } else if(request[0]==7)status=ln_fat32_open(&volume,"/.LINNET",2)?0:LN_FAT32_ARGUMENT;
    else status=ln_fat32_poll(&volume);
    result[3]=(uint32_t)status;result[4]=volume.mounted;result[5]=volume.clusters;
    result[6]=volume.sectors_per_cluster;result[7]=(uint32_t)volume.fat_start;
    result[8]=(uint32_t)volume.data_start;result[9]=volume.root;result[10]=volume.file.size;
    result[11]=volume.file.cluster;result[12]=volume.file.attributes;
}
