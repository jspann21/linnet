#include "boot/include/linnet_bytes.h"
#include "services/update/update.h"
#include "boot/include/sha256.h"
#if defined(LINNET_IMAGE_BOOT) && !defined(LINNET_PROFILE_RELEASE)
#include "hal/api/platform.h"
#include "hal/api/recovery.h"
#define LN_VALIDATE_START(name) uint32_t name=ln_time_us()
#define LN_VALIDATE_TIME(index,name) ln_recovery_timeline_add(index,ln_time_us()-name)
#define LN_VALIDATE_COUNT(index,value) ln_recovery_timeline_add(index,value)
#else
#define LN_VALIDATE_START(name) ((void)0)
#define LN_VALIDATE_TIME(index,name) ((void)0)
#define LN_VALIDATE_COUNT(index,value) ((void)0)
#endif
static void clear_image(struct ln_update_image*i){i->length=i->profile=0;for(unsigned k=0;k<32;++k)i->build_id[k]=i->sha256[k]=0;}
static int usable(struct ln_update*u){return u&&u->ready&&u->store->ready&&!u->store->poisoned;}
int ln_update_metadata_encode(const struct ln_update*u,uint8_t p[184]){
    if(!u||!p)return 0;
    for(unsigned i=0;i<184;++i)p[i]=0;
    ln_write_le32(p,0x54445055u);ln_write_le32(p+4,0);
    if(!ln_boot_state_encode(&u->boot,p+8,32))return 0;
    for(unsigned s=0;s<2;++s){uint8_t*r=p+40+s*72;const struct ln_update_image*i=&u->image[s];
        ln_write_le32(r,i->length);ln_write_le32(r+4,i->profile);
        for(unsigned k=0;k<32;++k){r[8+k]=i->build_id[k];r[40+k]=i->sha256[k];}}
    return 1;
}
static int publish(struct ln_update*u){uint8_t p[184];
    if(!ln_update_metadata_encode(u,p)||!ln_state_publish(u->store,p,184)){u->ready=0;return 0;}return 1;}
int ln_update_open(struct ln_update*u,struct ln_state_store*store,const struct ln_update_io*io,const struct ln_image_policy*policy){
    if(u)u->rejected_slot=255;
    if(!u||!store||!io||!policy||!io->read||!io->write||!io->flush||!io->capacity||!store->ready||store->poisoned)return 0;
    u->ready=u->upload_active=0;u->store=store;u->io=*io;u->policy=*policy;
    clear_image(&u->image[0]);clear_image(&u->image[1]);ln_boot_state_init(&u->boot,255);
    if(!store->generation){u->ready=1;return 1;}
    const uint8_t*p=store->payload;
    if(store->length!=184||ln_read_le32(p)!=0x54445055u||ln_read_le32(p+4)||!ln_boot_state_decode(&u->boot,p+8,32))return 0;
    for(unsigned s=0;s<2;++s){const uint8_t*r=p+40+s*72;struct ln_update_image*i=&u->image[s];
        i->length=ln_read_le32(r);i->profile=ln_read_le32(r+4);
        if(!i->length){if(!ln_bytes_zero(r,72))return 0;continue;}
        if(i->length<132||i->length>LN_UPDATE_IMAGE_MAX||i->length>io->capacity(io->context,s)||
           i->profile>2)return 0;
        for(unsigned k=0;k<32;++k){i->build_id[k]=r[8+k];i->sha256[k]=r[40+k];}}
    if(u->boot.confirmed_slot>1||!u->image[u->boot.confirmed_slot].length||
       (u->boot.candidate_slot<=1&&!u->image[u->boot.candidate_slot].length))return 0;
    u->ready=1;return 1;
}
static int read_image(struct ln_update*u,uint32_t slot,uint32_t n,const uint8_t expected[32],
                      uint8_t*scratch,uint32_t capacity,struct ln_image_info*info,int published){
    if(!scratch||!info||slot>1||n<132||n>LN_UPDATE_IMAGE_MAX||n>capacity||n>u->io.capacity(u->io.context,slot))return 0;
    struct ln_sha256 hash;ln_sha256_init(&hash);
    for(uint32_t offset=0;offset<n;){uint32_t step=n-offset;if(step>4096)step=4096;
        LN_VALIDATE_START(read_started);
        int result=u->io.read(u->io.context,slot,offset,scratch+offset,step);
        LN_VALIDATE_TIME(0,read_started);
        LN_VALIDATE_COUNT(4,1u);
        if(result!=1)return 0;
        LN_VALIDATE_COUNT(3,step);
        LN_VALIDATE_START(hash_started);
        ln_sha256_update(&hash,scratch+offset,step);offset+=step;
        LN_VALIDATE_TIME(1,hash_started);
        if(u->io.progress)u->io.progress(u->io.context);}
    LN_VALIDATE_START(finish_started);
    uint8_t actual[32];ln_sha256_finish(&hash,actual);
    LN_VALIDATE_TIME(1,finish_started);
    if(!ln_bytes_equal(actual,expected,32))return 0;
    LN_VALIDATE_START(crc_started);
    int valid=(published
        ? ln_image_validate_sha256_matched(scratch,n,&u->policy,info)
        : ln_image_validate_progress(scratch,n,&u->policy,info,u->io.progress,u->io.context))==LINNET_IMAGE_OK;
    LN_VALIDATE_TIME(2,crc_started);
    return valid;
}
int ln_update_initialize(struct ln_update*u,uint32_t slot,uint32_t n,const uint8_t expected[32],uint8_t*scratch,uint32_t capacity){
    if(!usable(u)||u->store->generation||slot>1||!expected)return 0;
    struct ln_image_info info;if(!read_image(u,slot,n,expected,scratch,capacity,&info,0))return 0;
    struct ln_update_image*i=&u->image[slot];i->length=n;i->profile=info.profile;
    for(unsigned k=0;k<32;++k){i->build_id[k]=info.build_id[k];i->sha256[k]=expected[k];}
    ln_boot_state_init(&u->boot,(uint8_t)slot);return publish(u);
}
int ln_update_begin(struct ln_update*u,uint32_t n,const uint8_t expected[32]){
    if(!usable(u)||!u->store->generation||u->upload_active||u->boot.attempt_pending||u->boot.confirmed_slot>1||
       u->boot.generation==UINT32_MAX||!expected||n<132||n>LN_UPDATE_IMAGE_MAX)return 0;
    uint32_t slot=u->boot.confirmed_slot^1u;if(n>u->io.capacity(u->io.context,slot))return 0;
    /* Durably withdraw any former candidate before overwriting its bytes. */
    clear_image(&u->image[slot]);u->boot.candidate_slot=255;++u->boot.generation;
    if(!publish(u))return 0;
    u->upload_slot=slot;u->upload_size=n;u->upload_offset=0;
    for(unsigned k=0;k<32;++k)u->upload_sha[k]=expected[k];u->upload_active=1;return 1;
}
int ln_update_write(struct ln_update*u,uint32_t offset,const uint8_t*p,uint32_t n){
    if(!usable(u)||!u->upload_active||!p||!n||n>4096||offset!=u->upload_offset||n>u->upload_size-offset)return 0;
    if(u->io.write(u->io.context,u->upload_slot,offset,p,n)!=1){u->upload_active=0;u->ready=0;return 0;}
    u->upload_offset+=n;if(u->io.progress)u->io.progress(u->io.context);return 1;
}
int ln_update_finish(struct ln_update*u,uint8_t*scratch,uint32_t capacity){
    if(!usable(u)||!u->upload_active||u->upload_offset!=u->upload_size)return 0;
    u->upload_active=0;
    if(u->io.flush(u->io.context)!=1){u->ready=0;return 0;}
    struct ln_image_info info;
    if(!read_image(u,u->upload_slot,u->upload_size,u->upload_sha,scratch,capacity,&info,0))return 0;
    if(u->boot.generation==UINT32_MAX)return 0;
    struct ln_update_image*i=&u->image[u->upload_slot];i->length=u->upload_size;i->profile=info.profile;
    for(unsigned k=0;k<32;++k){i->build_id[k]=info.build_id[k];i->sha256[k]=u->upload_sha[k];}
    if(!ln_boot_offer_candidate(&u->boot,(uint8_t)u->upload_slot))return 0;
    return publish(u);
}
int ln_update_validate(struct ln_update*u,uint32_t slot,uint8_t*scratch,uint32_t capacity,struct ln_image_info*info){
    if(!usable(u)||slot>1||!u->image[slot].length)return 0;
    struct ln_update_image*i=&u->image[slot];
    return read_image(u,slot,i->length,i->sha256,scratch,capacity,info,1)&&
        info->profile==i->profile&&ln_bytes_equal(info->build_id,i->build_id,32);
}
int ln_update_after_reset(struct ln_update*u){
    if(!usable(u)||u->upload_active)return 0;
    if(!u->boot.attempt_pending)return 1;
    if(u->boot.generation==UINT32_MAX||!ln_boot_after_reset(&u->boot))return 0;
    return publish(u);
}
uint8_t ln_update_prepare_boot(struct ln_update*u,uint8_t*scratch,uint32_t capacity,struct ln_image_info*info){
    if(u)u->rejected_slot=255;
    if(!usable(u)||u->upload_active||u->boot.attempt_pending||u->boot.generation==UINT32_MAX)return 255;
    /* Validate in selection order so the successful image remains in scratch.
     * An invalid candidate falls back to confirmed; unreferenced slots cannot
     * be selected and need no read. Keep all policy/hash checks before the
     * durable attempt publication. */
    uint8_t eligible=3,slot;
    for(;;){
        slot=ln_boot_select(&u->boot,eligible);
        if(slot>1)return 255;
        if(ln_update_validate(u,slot,scratch,capacity,info))break;
        if(slot==u->boot.candidate_slot)u->rejected_slot=slot;
        eligible&=(uint8_t)~(1u<<slot);
    }
    if(!ln_boot_begin_attempt(&u->boot,slot)||!publish(u))return 255;
    return slot;
}
int ln_update_mark_healthy(struct ln_update*u,uint32_t slot,const uint8_t sha[32],uint32_t readiness){
    if(!usable(u)||u->upload_active||slot>1||!sha||!ln_bytes_equal(sha,u->image[slot].sha256,32)||
       (readiness&31)!=31||u->boot.generation==UINT32_MAX)return 0;
    struct ln_update proposed=*u;
    if(!ln_boot_mark_healthy(&proposed.boot,(uint8_t)slot,readiness))return 0;
    if(u->boot.generation==proposed.boot.generation)return 1;
    if(!publish(&proposed)){u->ready=0;return 0;}
    u->boot=proposed.boot;return 1;
}
