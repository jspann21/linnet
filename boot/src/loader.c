#include "boot/include/linnet_bytes.h"
#include "loader.h"
static void count(uint32_t *value) { if(*value!=UINT32_MAX)++*value; }
static int begin(void *context,uint32_t size) {
    struct ln_loader *s=context;
    s->verified=0;s->size=0;s->offset=0;
    if(!(s->config.capabilities&LINNET_LOADER_CAP_UPLOAD)||
        size<LINNET_IMAGE_HEADER_SIZE+4u||size>s->config.capacity)return 0;
    s->size=size;count(&s->metrics[0]);return 1;
}
static int write_chunk(void *context,uint32_t offset,const uint8_t *data,uint32_t size) {
    struct ln_loader *s=context;
    s->verified=0;
    if(!data||!s->size||offset!=s->offset||size>s->size-offset)return 0;
    for(uint32_t i=0;i<size;++i)s->config.stage[offset+i]=data[i];
    s->offset+=size;
    if(size>UINT32_MAX-s->metrics[1])s->metrics[1]=UINT32_MAX;else s->metrics[1]+=size;
    return 1;
}
static int verify(void *context,uint32_t size,const uint8_t expected[32]) {
    struct ln_loader *s=context;
    struct ln_sha256 hash;
    uint8_t digest[32],difference=0;
    s->verified=0;
    if(!expected||size!=s->size||s->offset!=size||size>s->config.capacity)goto fail;
    ln_sha256_init(&hash);
    for(uint32_t offset=0;offset<size;) {
        uint32_t chunk=size-offset;
        if(chunk>4096u)chunk=4096u;
        ln_sha256_update(&hash,s->config.stage+offset,chunk);
        offset+=chunk;
        if(s->config.actions.progress)s->config.actions.progress(s->config.actions.context);
    }
    ln_sha256_finish(&hash,digest);
    for(unsigned i=0;i<32;++i)difference|=(uint8_t)(digest[i]^expected[i]);
    if(difference||ln_image_validate_progress(s->config.stage,size,&s->config.policy,&s->candidate,
        s->config.actions.progress,s->config.actions.context)!=LINNET_IMAGE_OK)goto fail;
    for(unsigned i=0;i<32;++i)s->candidate_sha256[i]=digest[i];
    s->verified=1;count(&s->metrics[2]);return 1;
fail:
    count(&s->metrics[3]);return 0;
}
static int read_response(void *context,uint16_t command,uint8_t *output,uint32_t *size) {
    struct ln_loader *s=context;
    if(!output||!size)return 0;
    count(&s->metrics[6]);
    if(command==1||command==2||command==3) {
        if(*size<LINNET_LOADER_IDENTITY_SIZE)return 0;
        output[0]='L';output[1]='N';output[2]='I';output[3]='D';
        ln_write_le32(output+4,s->config.policy.target);ln_write_le32(output+8,0);
        ln_write_le32(output+12,s->config.profile);ln_write_le32(output+16,s->config.image_kind);
        ln_write_le32(output+20,s->config.health_flags);ln_write_le32(output+24,s->config.generation);
        ln_write_le32(output+28,s->config.capabilities);
        for(unsigned i=0;i<32;++i){output[32+i]=s->config.build_id[i];output[64+i]=s->config.image_sha256[i];}
        *size=LINNET_LOADER_IDENTITY_SIZE;return 1;
    }
    if(command==49) {
        if(*size<32)return 0;
        for(unsigned i=0;i<8;++i)ln_write_le32(output+4u*i,s->metrics[i]);
        *size=32;return 1;
    }
    if(command==48){*size=0;return 1;} /* No log records emitted by this bounded backend. */
    if(command==50) {
        struct ln_crash_record record;
        if(!s->config.crash_mailbox){*size=0;return 1;}
        if(*size<LINNET_CRASH_MAILBOX_SIZE)return 0;
        for(unsigned i=0;i<LINNET_CRASH_MAILBOX_SIZE;++i)output[i]=s->config.crash_mailbox[i];
        *size=ln_crash_decode(&record,output,LINNET_CRASH_MAILBOX_SIZE)?LINNET_CRASH_MAILBOX_SIZE:0;
        return 1;
    }
    return 0;
}
static int clear_crash(void *context) {
    struct ln_loader *s=context;
    if(!s->config.crash_mailbox)return 0;
    for(unsigned i=0;i<LINNET_CRASH_MAILBOX_SIZE;++i)s->config.crash_mailbox[i]=0;
    count(&s->metrics[5]);return 1;
}
static int quiesce(void *context) {
    struct ln_loader *s=context;
    return s->config.actions.quiesce&&s->config.actions.quiesce(s->config.actions.context);
}
static int chainload(void *context) {
    struct ln_loader *s=context;
    if(!(s->config.capabilities&LINNET_LOADER_CAP_RAM_BOOT)||!s->verified||!s->config.actions.chainload)return 0;
    /* Revalidate staged bytes after transport quiescence, before copying/executing. */
    if(!verify(s,s->size,s->candidate_sha256))return 0;
    s->verified=0;count(&s->metrics[4]);
    return s->config.actions.chainload(s->config.actions.context,
        s->config.stage+LINNET_IMAGE_HEADER_SIZE,s->candidate.payload_size,
        s->candidate.load_address,s->candidate.entry_address,s->candidate.build_id,
        s->candidate_sha256,s->config.generation+1u);
}
static int reboot(void *context,uint16_t command) {
    struct ln_loader *s=context;
    uint32_t capability=command==32?LINNET_LOADER_CAP_REBOOT_OS:
        command==33?LINNET_LOADER_CAP_REBOOT_BOOT:command==34?LINNET_LOADER_CAP_REBOOT_DISK:0;
    return capability&&(s->config.capabilities&capability)&&s->config.actions.reboot&&
        s->config.actions.reboot(s->config.actions.context,command);
}
static int can_action(void *context,uint16_t command) {
    struct ln_loader *s=context;
    if(!s->config.actions.quiesce)return 0;
    if(command>=32u&&command<=34u&&(!s->config.actions.can_reboot||
        !s->config.actions.can_reboot(s->config.actions.context,command)))return 0;
    switch(command) {
    case 20:return (s->config.capabilities&LINNET_LOADER_CAP_RAM_BOOT)&&s->verified&&s->config.actions.chainload;
    case 32:return (s->config.capabilities&LINNET_LOADER_CAP_REBOOT_OS)&&s->config.actions.reboot;
    case 33:return (s->config.capabilities&LINNET_LOADER_CAP_REBOOT_BOOT)&&s->config.actions.reboot;
    case 34:return (s->config.capabilities&LINNET_LOADER_CAP_REBOOT_DISK)&&s->config.actions.reboot;
    case 52:return (s->config.capabilities&LINNET_LOADER_CAP_CRASH)&&s->config.actions.crash;
    default:return 0;
    }
}
static int crash_action(void *context) {
    struct ln_loader *s=context;
    return can_action(s,52)&&s->config.actions.crash(s->config.actions.context);
}
int ln_loader_init(struct ln_loader *s,const struct ln_loader_config *config,ln_link_backend *backend) {
    if(!s||!config||!backend||!config->stage||config->capacity<LINNET_IMAGE_HEADER_SIZE+4u||
        config->capacity>LINNET_LINK_MAX_IMAGE||config->profile>2||config->image_kind>1)return 0;
    s->config=*config;s->size=0;s->offset=0;s->verified=0;
    for(unsigned i=0;i<8;++i)s->metrics[i]=0;
    backend->context=s;backend->begin=begin;backend->write=write_chunk;backend->verify=verify;
    backend->read=read_response;backend->clear_crash=clear_crash;
    backend->quiesce=quiesce;backend->chainload=chainload;backend->reboot=reboot;
    backend->can_action=can_action;backend->crash=crash_action;
    return 1;
}
void ln_loader_set_health(struct ln_loader *s,uint32_t flags) {
    if(s)s->config.health_flags=flags&LINNET_BOOT_HEALTH_ALL;
}
