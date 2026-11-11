/* Read-through cache for the host-extracted, immutable native libraries.
 * All pixels, firmware bytes and coverage bits live in the leased idle music
 * decoder workspace. A music decoder generation invalidates byte coverage. */
#define RESOURCE_MAP_AT 0x90000u
#define RESOURCE_MAP_BYTES 45626u
static const struct {uint32_t bytes,data,map;} resource_layout[5]={
    {51620u,0x32000u,0u},{106960u,0x3e9c0u,6453u},
    {147232u,0x58ba0u,19823u},{52664u,0x7cac0u,38227u},
    {6528u,0x89880u,44810u}
};
_Static_assert(BCM_IMAGE_BYTES<=0x32000u,"native resource cache overlaps firmware");
_Static_assert(0x89880u+6528u<=RESOURCE_MAP_AT,"native resource data overlaps coverage");
_Static_assert(RESOURCE_MAP_AT+RESOURCE_MAP_BYTES<=0xa0000u,"native resource cache extent");
static struct {
    struct {struct ln_fat32_file file;bool ready;} file[5];
    uint32_t hits,misses,bytes,epoch;bool initialized;
} resources;
static bool resource_binding(void){
    if(!vc.image_leased)return false;
    struct ln_fat32_file root;
    bool mounted=ln_storage_media_root(&root)&&root.epoch;
    if(!mounted||resources.epoch!=root.epoch){
        resources.epoch=mounted?root.epoch:0;resources.initialized=false;vc.image_valid=false;
        for(uint32_t i=0;i<5u;++i)resources.file[i].ready=false;
    }
    if(!mounted)return false;
    if(resources.initialized)return true;
    uint32_t *words=(uint32_t *)(vc.image+RESOURCE_MAP_AT);
    for(uint32_t i=0;i<RESOURCE_MAP_BYTES/4u;++i)words[i]=0;
    for(uint32_t i=RESOURCE_MAP_BYTES&~3u;i<RESOURCE_MAP_BYTES;++i)vc.image[RESOURCE_MAP_AT+i]=0;
    resources.initialized=true;return true;
}
static void resource_reset(bool changed){
    if(changed)resources.initialized=false;
    (void)resource_binding();
}
static bool resource_span(uint32_t id,uint32_t epoch,uint32_t at,uint32_t bytes){
    return resource_binding()&&epoch==resources.epoch&&id&&id<=5u&&at<=resource_layout[id-1u].bytes&&bytes<=resource_layout[id-1u].bytes-at;
}
static bool resource_file_matches(uint32_t id,const struct ln_fat32_file *file){
    if(!file||!resource_span(id,file->epoch,0,file->size)||!resources.file[id-1u].ready)return false;
    const struct ln_fat32_file *known=&resources.file[id-1u].file;
    return known->epoch==file->epoch&&known->size==file->size&&known->cluster==file->cluster&&
        known->directory_sector==file->directory_sector&&known->directory_offset==file->directory_offset&&known->attributes==file->attributes;
}
static void resource_opened(uint32_t id,const struct ln_fat32_file *file){
    if(!file||!resource_span(id,file->epoch,0,file->size)||file->size!=resource_layout[id-1u].bytes)return;
    if(!resource_file_matches(id,file)){
        uint8_t *map=vc.image+RESOURCE_MAP_AT+resource_layout[id-1u].map;
        for(uint32_t i=0;i<(file->size+7u)/8u;++i)map[i]=0;
    }
    resources.file[id-1u].file=*file;resources.file[id-1u].ready=true;
}
static bool resource_covered(uint32_t id,uint32_t epoch,uint32_t at,uint32_t bytes){
    if(!resource_span(id,epoch,at,bytes))return false;
    const uint8_t *map=vc.image+RESOURCE_MAP_AT+resource_layout[id-1u].map;
    uint32_t end=at+bytes,cursor=at;
    while(cursor<end&&(cursor&7u)){if(!(map[cursor>>3]&(1u<<(cursor&7u))))return false;++cursor;}
    while(end-cursor>=8u){if(map[cursor>>3]!=255u)return false;cursor+=8u;}
    while(cursor<end){if(!(map[cursor>>3]&(1u<<(cursor&7u))))return false;++cursor;}
    return true;
}
static bool resource_read(uint32_t id,uint32_t epoch,uint32_t at,void *output,uint32_t bytes){
    if(!resource_covered(id,epoch,at,bytes)){++resources.misses;return false;}
    ln_memory_copy(output,vc.image+resource_layout[id-1u].data+at,bytes);
    ++resources.hits;resources.bytes+=bytes;return true;
}
static bool resource_forget_span(uint32_t id,uint32_t epoch,uint32_t at,uint32_t bytes){
    if(!resource_span(id,epoch,at,bytes))return false;
    uint8_t *map=vc.image+RESOURCE_MAP_AT+resource_layout[id-1u].map;
    uint32_t end=at+bytes;
    while(at<end&&(at&7u)){map[at>>3]&=(uint8_t)~(1u<<(at&7u));++at;}
    while(end-at>=8u){map[at>>3]=0;at+=8u;}
    while(at<end){map[at>>3]&=(uint8_t)~(1u<<(at&7u));++at;}
    return true;
}
static void resource_publish(uint32_t id,uint32_t epoch,uint32_t at,uint32_t bytes){
    if(!resource_span(id,epoch,at,bytes))return;
    uint8_t *map=vc.image+RESOURCE_MAP_AT+resource_layout[id-1u].map;
    uint32_t end=at+bytes;
    while(at<end&&(at&7u)){map[at>>3]|=1u<<(at&7u);++at;}
    while(end-at>=8u){map[at>>3]=255u;at+=8u;}
    while(at<end){map[at>>3]|=1u<<(at&7u);++at;}
}
static void resource_store(uint32_t id,uint32_t epoch,uint32_t at,const void *input,uint32_t bytes){
    if(!resource_span(id,epoch,at,bytes))return;
    ln_memory_copy(vc.image+resource_layout[id-1u].data+at,input,bytes);
    resource_publish(id,epoch,at,bytes);
}
