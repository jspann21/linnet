/* Focused host qualification of the actual block owner and its private cache.
 * White-box access seeds successful cache fills and checks ownership invariants;
 * public admission/queue APIs exercise write and lifecycle ordering. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "services/storage/block.c"
#include "services/storage/resident.h"
#include "boot/include/linnet_bytes.h"
static uint8_t bank[32u*1024u*1024u];
static struct ln_ata_state disk;
static uint32_t now,commands,tick,snapshots;
uint32_t ln_time_us(void){uint32_t value=now;now+=tick;return value;}
void *ln_platform_extended_memory(uint32_t *bytes){*bytes=sizeof(bank);return bank;}
void ln_ata_init(uint32_t t){(void)t;disk=(struct ln_ata_state){.sectors=1000000,.initialized=1,.powered=1,.can_flush=1};}
void ln_ata_recover(uint32_t t){(void)t;}
bool ln_ata_power_off(void){disk.powered=0;return true;}
bool ln_ata_power_on(uint32_t t){(void)t;disk.powered=1;return true;}
bool ln_ata_start(enum ln_ata_op op,uint64_t lba,uint32_t n,uint8_t *p,uint32_t d){
    (void)op;(void)lba;(void)n;(void)p;(void)d;++commands;return true;
}
void ln_ata_step(uint32_t t){(void)t;}
bool ln_ata_take(struct ln_ata_result *r){(void)r;return false;}
void ln_ata_state_snapshot(struct ln_ata_state *out){++snapshots;*out=disk;}
uint32_t ln_ata_next_deadline(uint32_t t){return t+1000u;}
#if !defined(LINNET_PROFILE_RELEASE)
void ln_ata_performance(uint32_t out[16]){memset(out,0,16u*sizeof(*out));}
#endif
static void reset(void){now=100;commands=tick=0;ln_block_init(now);assert(ln_block_set_window(8,900000));}
static void fill(uint64_t lba,uint32_t count,uint8_t value){
    uint8_t bytes[4096];memset(bytes,value,sizeof(bytes));assert(count<=8);extra_store(lba,count,bytes);
}
static void payload_and_collision(void){
    reset();fill(80,1,0x5a);uint32_t lease=ln_block_cache_begin();assert(lease);
    assert(!ln_block_cache_pin(lease,80,2));assert(ln_block_cache_valid(lease));
    uint8_t out[1024];memset(out,0x7e,sizeof(out));
    assert(!ln_block_cache_copy(lease,80,2,out));for(unsigned i=0;i<sizeof(out);++i)assert(out[i]==0x7e);
    fill(80+4096u*8u,8,0x11);assert(ln_block_cache_sector(lease,80)[0]==0x5a);
    fill(80,2,0x33);assert(ln_block_cache_sector(lease,80)[0]==0x5a);assert(ln_block_cache_sector(lease,81)[0]==0x33);
    disk.busy=1;power_task=1;assert(ln_block_cache_copy(lease,80,2,out));
    assert(out[0]==0x5a&&out[512]==0x33&&commands==0);disk.busy=0;power_task=0;
    ln_block_cache_end(lease);fill(80+4096u*8u,8,0x11);fill(80+8192u*8u,8,0x22);assert(!ln_block_cache_valid(lease));
    uint32_t next=ln_block_cache_begin();assert(next>lease);ln_block_cache_end(lease);assert(ln_block_cache_valid(next));
    assert(!ln_block_cache_sector(next,80));assert(ln_block_cache_sector(next,80+4096u*8u)[0]==0x11);
    ln_block_cache_end(next);
}
static void write_revocation(void){
    reset();fill(80,1,0x5a);uint32_t lease=ln_block_cache_begin();assert(ln_block_cache_pin(lease,80,1));
    uint8_t bytes[512]={0};uint32_t request;
    assert(ln_block_submit(LN_BLOCK_WRITE,80+4096u*8u,1,bytes,1,now+1000,&request));
    assert(ln_block_cache_valid(lease));assert(commands==0);ln_block_cancel(1);
    struct ln_block_completion completion;assert(ln_block_take(1,&completion));
    assert(ln_block_submit(LN_BLOCK_WRITE,87,1,bytes,2,now+1000,&request));
    assert(!ln_block_cache_valid(lease));assert(commands==0); /* Before issue, same 4 KiB page. */
    ln_block_cancel(2);assert(ln_block_take(2,&completion));
}
static void lifecycle(void){
    reset();fill(80,8,1);uint32_t lease=ln_block_cache_begin();assert(ln_block_cache_pin(lease,80,8));
    assert(ln_block_set_window(8,900000));assert(!ln_block_cache_valid(lease));
    lease=ln_block_cache_begin();assert(lease);assert(ln_block_recover(now));assert(!ln_block_cache_valid(lease));
    assert(ln_block_set_window(8,900000));lease=ln_block_cache_begin();assert(lease);
    assert(ln_block_quiesce(now,now+1000)==0);assert(!ln_block_cache_valid(lease));
    reset();lease=ln_block_cache_begin();assert(lease);invalidate_cache();assert(!ln_block_cache_valid(lease));
    extra_serial=UINT32_MAX;assert(!ln_block_cache_begin());
    reset();disk.failed=1;assert(!ln_block_cache_begin());disk.failed=0;
    disk.initializing=1;assert(!ln_block_cache_begin());disk.initializing=0;
    disk.powered=disk.initialized=0;assert(ln_block_cache_begin()); /* Retained geometry, sleeping rail. */
}
static int forbidden_io(void *c,enum ln_fat32_io op,uint64_t lba,uint8_t *b,uint32_t g){
    (void)c;(void)op;(void)lba;(void)b;(void)g;assert(!"original filesystem adapter called");return -1;
}
static struct ln_fat32 volume,workspace,before;
static struct ln_resident_media resident;
#define BCM_IMAGE_BYTES 201376u
static uint8_t native_workspace[0xa0000u];
static struct {uint8_t *image;bool image_leased,image_valid;} vc;
static bool native_mounted;
bool ln_storage_media_root(struct ln_fat32_file *out){
    if(!native_mounted)return false;*out=(struct ln_fat32_file){.epoch=volume.epoch};return true;
}
void ln_memory_copy(void *to,const void *from,uint32_t bytes){memcpy(to,from,bytes);}
#include "targets/video5g-m25/native_resources.inc"
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
static void fat(uint32_t first,uint32_t second,bool mismatch){
    uint8_t sector[512]={0};put32(sector+3*4,first);put32(sector+9*4,second);
    extra_store(16,1,sector);if(mismatch)sector[400]=1;extra_store(24,1,sector);
}
static void filesystem(void){
    reset();memset(&resident,0,sizeof(resident));memset(&volume,0,sizeof(volume));
    volume.config.io=forbidden_io;volume.config.context=(void *)1;volume.config.writable=true;
    volume.config.disk_sectors=disk.sectors;volume.partition=8;volume.partition_sectors=900000;
    volume.fat_start=16;volume.fat_sectors=8;volume.data_start=64;volume.clusters=100;
    volume.root=2;volume.epoch=7;volume.mounted=1;volume.sectors_per_cluster=1;volume.fats=2;volume.mirrored=1;
    volume.buffer=(void *)1;volume.listing=(void *)1;
    volume.cache[3%16]=(struct ln_fat32_cache){3,9}; /* An inherited shortcut is not residency. */
    uint8_t directory[512]={0};memcpy(directory,"MOVIE   MP4",11);directory[11]=0x20;directory[26]=3;put32(directory+28,768);
    extra_store(64,1,directory);fat(9,0x0fffffff,false);fill(65,1,0xa5);fill(71,1,0x5a);before=volume;
}
static bool admit(void){const char *paths[]={"/MOVIE.MP4"};return ln_resident_admit(&resident,&volume,paths,1,&workspace);}
static void file_proofs(void){
    filesystem();assert(admit());assert(!memcmp(&volume,&before,sizeof(volume)));assert(commands==0);
    struct ln_fat32_file file=resident.entry[0].file;uint8_t out[700];memset(out,0x77,sizeof(out));
    disk.busy=1;power_task=1;assert(ln_resident_read(&resident,&file,111,out,600)==1);
    for(unsigned i=0;i<401;++i)assert(out[i]==0xa5);for(unsigned i=401;i<600;++i)assert(out[i]==0x5a);
    assert(commands==0);file.directory_offset=32;memset(out,0x77,sizeof(out));
    assert(ln_resident_read(&resident,&file,0,out,600)<0);for(unsigned i=0;i<sizeof(out);++i)assert(out[i]==0x77);
    disk.busy=0;power_task=0;ln_resident_end(&resident);
    filesystem();fat(0x0fffffff,0x0fffffff,false);assert(!admit()); /* Short chain. */
    filesystem();fat(9,10,false);assert(!admit()); /* Long chain. */
    filesystem();fat(9,3,false);assert(!admit()); /* Cycle. */
    filesystem();fat(9,0x0fffffff,true);assert(!admit()); /* Other entry in mirrored sector differs. */
    filesystem();extra_tags[extra_find(71>>3)].valid&=~(1u<<(71&7));assert(!admit());
    assert(commands==0);assert(!memcmp(&volume,&before,sizeof(volume)));assert(!resident.cookie);
    filesystem();tick=30000;assert(!admit());assert(resident.failure==2);tick=0;
    filesystem();assert(admit());file=resident.entry[0].file;
    extra_written(71,1);memset(out,0x77,sizeof(out));assert(ln_resident_read(&resident,&file,0,out,600)<0);
    for(unsigned i=0;i<sizeof(out);++i)assert(out[i]==0x77);
    filesystem();assert(admit());
    uint8_t bytes[512]={0};uint32_t request;
    assert(ln_block_submit(LN_BLOCK_WRITE,160,1,bytes,3,now+1000,&request));
    fill(160,1,0x66); /* An older read completed after the write was accepted. */
    const char *paths[]={"/MOVIE.MP4"};
    assert(!ln_resident_append(&resident,&volume,paths,1,&workspace));assert(!resident.cookie);
    filesystem();assert(admit());uint32_t first=resident.cookie;
    assert(ln_resident_append(&resident,&volume,paths,1,&workspace));assert(resident.cookie==first&&resident.count==2);
}
static void bounded_fragmented_reads(void){
    filesystem();ln_resident_end(&resident);
    volume.fat_sectors=40;volume.data_start=128;volume.clusters=5000;
    uint8_t directory[512]={0};memcpy(directory,"MOVIE   MP4",11);directory[11]=0x20;directory[26]=3;
    put32(directory+28,4096u*512u);extra_store(128,1,directory);
    uint8_t table[40u*512u]={0};
    for(uint32_t i=0;i<4096u;++i){
        uint32_t cluster=3u+(i*2053u)%4096u,next=i==4095u?0x0fffffffu:3u+((i+1u)*2053u)%4096u;
        put32(table+cluster*4u,next);fill(128u+cluster-2u,1,(uint8_t)i);
    }
    for(uint32_t i=0;i<40u;++i){extra_store(16u+i,1,table+i*512u);extra_store(56u+i,1,table+i*512u);}
    before=volume;assert(admit());assert(resident.entry[0].shift==6);
    uint8_t byte=0;snapshots=0;
    assert(ln_resident_read(&resident,&resident.entry[0].file,4095u*512u+3u,&byte,1)==1);
    assert(byte==255u&&snapshots<=130u); /* Two bounded 63-edge walks, no mirror rereads. */
    assert(!memcmp(&volume,&before,sizeof(volume)));assert(commands==0);ln_resident_end(&resident);
}
static void selected_fat_and_directories(void){
    filesystem();volume.mirrored=0;volume.active_fat=1;
    uint8_t bad[512]={0};extra_store(16,1,bad);assert(admit());ln_resident_end(&resident);
    filesystem();volume.root=130;volume.clusters=200;
    uint8_t directory[512]={0};for(unsigned i=0;i<16;++i)directory[i*32]=0xe5;
    extra_store(192,1,directory);memset(directory,0,sizeof(directory));
    memcpy(directory,"VIDEOS     ",11);directory[11]=16;directory[26]=4;extra_store(193,1,directory);
    memset(directory,0,sizeof(directory));memcpy(directory,"MOVIE   MP4",11);directory[11]=0x20;directory[26]=3;
    put32(directory+28,768);extra_store(66,1,directory);
    uint8_t table[512]={0};put32(table+(130%128)*4,131);put32(table+(131%128)*4,0x0fffffff);
    extra_store(17,1,table);extra_store(25,1,table);volume.cache[130%16]=(struct ln_fat32_cache){130,131};
    before=volume;const char *paths[]={"/VIDEOS/MOVIE.MP4"};
    assert(ln_resident_admit(&resident,&volume,paths,1,&workspace));
    assert(!memcmp(&volume,&before,sizeof(volume)));assert(commands==0);ln_resident_end(&resident);
    /* Inherited directory-edge metadata must not hide a missing source sector. */
    extra_tags[extra_find(17>>3)].valid&=~(1u<<(17&7));
    assert(!ln_resident_admit(&resident,&volume,paths,1,&workspace));assert(commands==0);
}
static void small_window_promotion(void){
    filesystem();assert(extra_read(64,1,cache[0]));cache_lba[0]=64;cache_sectors[0]=1;
    fill(64u+4096u*8u,8,0x11);fill(64u+8192u*8u,8,0x22);fill(65,1,0xa5);fill(71,1,0x5a);
    assert(!(extra_tags[extra_find(64>>3)].valid&1u));assert(admit());assert(commands==0);
    assert(ln_block_cache_sector(resident.cookie,64)[0]=='M');ln_resident_end(&resident);
    filesystem();assert(extra_read(64,1,cache[0]));cache_lba[0]=64;cache_sectors[0]=1;
    fill(64u+4096u*8u,8,0x11);fill(64u+8192u*8u,8,0x22);uint32_t lease=ln_block_cache_begin();
    assert(ln_block_cache_pin(lease,64u+4096u*8u,1));
    assert(ln_block_cache_pin(lease,64u+8192u*8u,1));
    assert(!ln_block_cache_sector(lease,64));assert(ln_block_cache_sector(lease,64u+4096u*8u)[0]==0x11);
    assert(commands==0);ln_block_cache_end(lease);
}
static void qualified_handle_provenance(void){
    filesystem();assert(admit());struct ln_fat32_file file=resident.entry[0].file;ln_resident_end(&resident);
    struct ln_resident_binding binding={0};struct ln_fat32_file output={0};
    assert(ln_resident_remember(&binding,"/MOVIE.MP4",&file));assert(ln_resident_match(&binding,"/MOVIE.MP4",7,&output));
    assert(output.cluster==file.cluster&&output.directory_sector==file.directory_sector);
    assert(!ln_resident_match(&binding,"/OTHER.MP4",7,&output));assert(!ln_resident_match(&binding,"/MOVIE.MP4",8,&output));
    /* The completed OPEN remains authoritative when its directory bytes have
     * left RAM. Every payload/FAT sector must still pass independent proof. */
    extra_tags[extra_find(64>>3)].valid&=~1u;
    assert(ln_resident_admit_files(&resident,&volume,&file,1,false));assert(commands==0);ln_resident_end(&resident);
    ++file.epoch;assert(!ln_resident_admit_files(&resident,&volume,&file,1,false));--file.epoch;
    ln_resident_forget(&binding);assert(!ln_resident_match(&binding,"/MOVIE.MP4",7,&output));
    file.attributes=16;assert(!ln_resident_remember(&binding,"/MOVIE.MP4",&file));
    assert(!ln_resident_match(&binding,"/MOVIE.MP4",7,&output));assert(!memcmp(&volume,&before,sizeof(volume)));
}
static void two_way_ownership(void){
    const uint32_t stride=2048u*8u;uint8_t out[512];uint32_t request;struct ln_block_completion completion;
    reset();fill(80,1,0x11);fill(80+stride,1,0x22);
    assert(extra_read(80,1,out)&&out[0]==0x11);fill(80+2u*stride,1,0x33);
    assert(extra_read(80,1,out)&&out[0]==0x11);assert(!extra_read(80+stride,1,out));
    uint32_t lease=ln_block_cache_begin();assert(ln_block_cache_pin(lease,80,1));
    assert(ln_block_cache_pin(lease,80+2u*stride,1));fill(80+3u*stride,8,0x44);
    assert(ln_block_cache_sector(lease,80)[0]==0x11);
    assert(ln_block_cache_sector(lease,80+2u*stride)[0]==0x33);
    assert(!ln_block_cache_sector(lease,80+3u*stride));
    fill(80+2u*stride,2,0x55);assert(ln_block_cache_sector(lease,80+2u*stride)[0]==0x33);
    assert(ln_block_cache_sector(lease,81+2u*stride)[0]==0x55);
    memset(out,0,sizeof(out));assert(ln_block_submit(LN_BLOCK_WRITE,87+2u*stride,1,out,9,now+1000,&request));
    assert(!ln_block_cache_valid(lease)&&commands==0);ln_block_cancel(9);assert(ln_block_take(9,&completion));
}
static void native_workspace_coverage(void){
    filesystem();memset(&resources,0,sizeof(resources));memset(native_workspace,0,sizeof(native_workspace));
    vc.image=native_workspace;vc.image_leased=vc.image_valid=true;native_mounted=true;
    struct ln_fat32_file file={.epoch=volume.epoch,.cluster=31,.size=106960,.directory_sector=64,.attributes=32};
    resource_opened(2,&file);assert(resource_file_matches(2,&file));assert(!resource_covered(2,file.epoch,0,file.size));
    memset(native_workspace+resource_layout[1].data,0xa5,file.size); /* DMA alone publishes nothing. */
    uint8_t out[13];memset(out,0x77,sizeof(out));assert(!resource_read(2,file.epoch,0,out,sizeof(out)));
    for(uint32_t i=0;i<sizeof(out);++i)assert(out[i]==0x77);
    resource_publish(2,file.epoch,1,file.size-1);assert(!resource_covered(2,file.epoch,0,file.size));
    resource_publish(2,file.epoch,0,1);assert(resource_covered(2,file.epoch,0,file.size));
    assert(resource_read(2,file.epoch,file.size-sizeof(out),out,sizeof(out)));assert(out[0]==0xa5);
    resource_opened(2,&file);assert(resource_covered(2,file.epoch,0,file.size)); /* Same tuple keeps bytes. */
    assert(resource_forget_span(2,file.epoch,7,1)); /* A partial chunk needs a replacement read. */
    assert(!resource_covered(2,file.epoch,0,16384));assert(resource_covered(2,file.epoch,8,1));
    assert(resource_forget_span(2,file.epoch,0,16384));
    memset(native_workspace+resource_layout[1].data,0xcc,4096); /* Failed/canceled DMA prefix. */
    resource_opened(2,&file);assert(!resource_read(2,file.epoch,8,out,sizeof(out)));
    assert(resource_covered(2,file.epoch,16384,1)); /* Outside the destination remains valid. */
    resource_publish(2,file.epoch,0,16384);assert(resource_read(2,file.epoch,8,out,sizeof(out))&&out[0]==0xcc);
    file.directory_offset+=32u;assert(!resource_file_matches(2,&file));resource_opened(2,&file);
    assert(!resource_covered(2,file.epoch,0,file.size));assert(resource_file_matches(2,&file));
    uint8_t input[13];memset(input,0x3c,sizeof(input));resource_store(2,file.epoch,3,input,sizeof(input));
    assert(resource_read(2,file.epoch,3,out,sizeof(out))&&out[12]==0x3c);assert(!resource_covered(2,file.epoch,2,sizeof(out)));
    resource_reset(true);assert(resource_file_matches(2,&file));assert(!resource_covered(2,file.epoch,3,sizeof(out)));
    ++volume.epoch;resource_publish(2,file.epoch,0,file.size);assert(!resource_file_matches(2,&file));
    ++file.epoch;resource_opened(2,&file);assert(!resource_covered(2,file.epoch,0,file.size));
    native_mounted=false;assert(!resource_binding());native_mounted=true;assert(!resource_file_matches(2,&file));
    vc.image_leased=false;assert(!resource_read(2,file.epoch,0,out,sizeof(out)));
    assert(!resource_covered(0,file.epoch,0,1)&&!resource_covered(6,file.epoch,0,1));
}
int main(void){payload_and_collision();write_revocation();lifecycle();file_proofs();bounded_fragmented_reads();selected_fat_and_directories();small_window_promotion();qualified_handle_provenance();two_way_ownership();native_workspace_coverage();
    puts("cache lease ownership, eviction, write ordering, lifecycle and FAT residency: PASS");return 0;}
