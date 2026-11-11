#include "boot/include/linnet_bytes.h"
#include "boot/include/linnet_boot.h"
#include "kernel/storage_link.h"
#include "hal/api/power_policy.h"
#include "services/storage/storage.h"
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
#include "services/music/history_pages.h"
#endif
#include "services/storage/block.h"
#if defined(LINNET_FILES)
#include "services/files/files.h"
#endif
#include "hal/api/recovery.h"
#include "hal/api/platform.h"
#include "hal/api/power.h"
#if defined(LINNET_IMAGE_OS)
#include "kernel/runtime.h"
#include "services/display/settings.h"
#include "services/music/coverflow.h"
#include "hal/api/display.h"
#endif
#if defined(LINNET_AUDIO)
#include "services/music/player.h"
#include "services/music/controller.h"
#endif
#if defined(LINNET_VIDEO)
#include "services/video/video.h"
#if defined(LINNET_VIDEO_MP4)
#include "services/video/mp4.h"
#endif
#endif
static uint8_t ln_storage_restart_pending;
int ln_storage_link_dispatch(const ln_link_frame*q,ln_link_frame*r,uint8_t *ln_storage_reply){
    if(q->command<80||q->command>98||q->flags||!q->sequence||q->length>1024||(q->length&&!q->payload))return 0;
    uint32_t generation=ln_recovery_generation();
    r->command=q->command;r->sequence=q->sequence;r->flags=3;r->payload=ln_storage_reply;r->length=8;
    ln_write_le32(ln_storage_reply,generation);ln_write_le32(ln_storage_reply+4,0);
#if defined(LINNET_VIDEO)
    /* Package validation uses STAGE, also owned by retained video records. */
    if(q->command==87u&&(ln_video_active()||ln_video_playback_pending()))return 1;
#endif
#if defined(LINNET_AUDIO)
    if(q->command==87u&&ln_player_busy())return 1;
#endif
    switch(q->command){
    case 80:{
        if(q->length)return 1;uint32_t words[24];ln_storage_state(words);
        for(unsigned i=0;i<24;++i)ln_write_le32(ln_storage_reply+i*4,words[i]);r->length=96;break;}
    case 94:{
        uint32_t words[32],count=24u;
        if(!q->length)ln_ata_diagnostics(words);
#if !defined(LINNET_PROFILE_RELEASE)
        else if(q->length==4u&&ln_read_le32(q->payload)==1u){ln_block_performance(words);count=32u;}
        else if(q->length==4u&&ln_read_le32(q->payload)==2u){ln_storage_media_performance(words);count=16u;}
        else if(q->length==4u&&ln_read_le32(q->payload)==3u){ln_ata_capabilities(words);count=16u;}
        else if(q->length==4u&&ln_read_le32(q->payload)==13u){ln_block_workload_diagnostics(words);count=8u;}
#if defined(LINNET_IMAGE_OS)
        else if(q->length==4u&&ln_read_le32(q->payload)==4u){ln_runtime_render_timing(words);count=16u;}
#if defined(LINNET_TARGET_VIDEO5G_M25)
        else if(q->length==4u&&ln_read_le32(q->payload)==22u){ln_power_sampler_diagnostics(words);count=16u;}
        else if(q->length==4u&&ln_read_le32(q->payload)==23u){ln_power_sampler_trace(words);count=16u;}
        else if(q->length==4u&&ln_read_le32(q->payload)==24u){ln_power_sampler_idle_trace(words);count=16u;}
#endif
#if defined(LINNET_FILES)
        else if(q->length==4u&&ln_read_le32(q->payload)==19u){ln_files_diagnostics(words);count=16u;}
#endif
        else if(q->length==4u&&ln_read_le32(q->payload)==14u){ln_runtime_exec_diagnostics(words);count=24u;}
        else if(q->length==4u&&ln_read_le32(q->payload)==15u){ln_display_settings_diagnostics(words);count=8u;}
#if defined(LINNET_HARDWARE_CLICKER)
        else if(q->length==4u&&ln_read_le32(q->payload)==18u){
            words[0]=0x4b494c43u;
            ln_platform_click_diagnostics(words+1);
            words[5]=ln_display_click_tone();count=6u;
        }
#endif
#if defined(LINNET_AUDIO)
        else if(q->length==4u&&ln_read_le32(q->payload)==17u){ln_music_budget_diagnostics(words);count=12u;}
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
        else if(q->length==4u&&ln_read_le32(q->payload)==26u){ln_player_cache_diagnostics(words);count=32u;}
#endif
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
        else if(q->length==4u&&ln_read_le32(q->payload)==20u){ln_paged_history_window_diagnostics(words);count=8u;}
#if defined(LINNET_OS_CRC_IRAM)
        else if(q->length==4u&&ln_read_le32(q->payload)==21u){ln_crc32_os_iram_diagnostics(words);count=8u;}
#endif
#endif
#endif
#if defined(LINNET_VIDEO_MP4)
        else if(q->length==4u&&ln_read_le32(q->payload)==16u){ln_mp4_table_diagnostics(words);count=12u;}
#endif
        else if(q->length==4u&&ln_read_le32(q->payload)==7u){ln_display_sync_diagnostics(words);count=16u;}
#if defined(LINNET_COVERFLOW)
        else if(q->length==4u&&ln_read_le32(q->payload)==11u){ln_coverflow_diagnostics(words);count=16u;}
        else if(q->length==4u&&ln_read_le32(q->payload)==12u){ln_coverflow_render_diagnostics(words);count=16u;}
#endif
        else if(q->length==4u&&ln_read_le32(q->payload)==8u){ln_display_blit_diagnostics(words);count=16u;}
#if LINNET_IDLE_CLOCK_HZ
        else if(q->length==4u&&ln_read_le32(q->payload)==10u){
            extern void ln_runtime_idle_clock_diagnostics(uint32_t output[4]);
            ln_runtime_idle_clock_diagnostics(words);count=4u;
        }

#endif
        else if(q->length==4u&&(count=ln_display_target_diagnostics(ln_read_le32(q->payload),words))!=0u){}

#endif
        else if(q->length==4u&&ln_read_le32(q->payload)==5u){ln_storage_schedule_performance(words);count=24u;}
        else if(q->length==4u&&ln_read_le32(q->payload)==6u){ln_ata_clock_diagnostics(words);count=8u;}
#if LINNET_RUNTIME_POWER_POLICY && defined(LINNET_IMAGE_OS)
        else if(q->length==4u&&ln_read_le32(q->payload)==27u){ln_power_policy_diagnostics(words);count=32u;}
#endif
#endif
        else return 1;
        for(uint32_t i=0;i<count;++i)ln_write_le32(ln_storage_reply+i*4,words[i]);r->length=(uint16_t)(count*4u);break;}
    case 95:{
        if(q->length)return 1;struct ln_ata_info a;struct ln_block_info b;
        ln_ata_snapshot(&a);ln_block_snapshot(&b);
        for(uint32_t i=0;i<192u;++i)ln_storage_reply[i]=0;
        uint32_t state=(!a.powered?0u:a.sleeping?2u:1u);
        if(a.initializing||b.power_transition)state=3u;
        uint32_t words[16]={0x4944454du,0,(uint32_t)a.sectors,(uint32_t)(a.sectors>>32),
            a.media_kind,a.rotation_rpm,a.logical_sector_bytes,a.physical_sector_bytes,
            a.capabilities,a.quirks,state,a.idle_timeout_us,a.wake_timeout_us,a.poweroffs,a.spinups,b.idle_failures};
        for(uint32_t i=0;i<16u;++i)ln_write_le32(ln_storage_reply+i*4,words[i]);
        for(uint32_t i=0;i<40u;++i)ln_storage_reply[64+i]=(uint8_t)a.model[i];
        for(uint32_t i=0;i<20u;++i)ln_storage_reply[104+i]=(uint8_t)a.serial[i];
        for(uint32_t i=0;i<8u;++i)ln_storage_reply[124+i]=(uint8_t)a.firmware[i];
        r->length=192;break;}
    case 81:case 97:{
        if(q->length!=8)return 1;uint32_t offset=ln_read_le32(q->payload),n=ln_read_le32(q->payload+4);
        if(!n||n>1008||!ln_storage_read(q->command==81?LN_STORAGE_SCRATCH:LN_STORAGE_WORK,offset,ln_storage_reply+4,n))return 1;
        ln_write_le32(ln_storage_reply,offset);r->length=4+n;break;}
    case 82:case 98:{
        if(q->length<=8||q->length>1016||ln_read_le32(q->payload)!=generation)return 1;
        uint32_t offset=ln_read_le32(q->payload+4),n=q->length-8;
        if(offset>UINT32_MAX-n||!ln_storage_write(q->command==82?LN_STORAGE_SCRATCH:LN_STORAGE_WORK,offset,q->payload+8,n)||!ln_storage_flush())return 1;
        ln_write_le32(ln_storage_reply+4,offset+n);break;}
    case 83:{
        if(q->length)return 1;uint32_t n=480;
        if(!ln_storage_settings(false,ln_storage_reply+4,&n)||n>480)return 1;
        r->length=4+n;break;}
    case 84:{
        if(q->length<4||q->length>484||ln_read_le32(q->payload)!=generation)return 1;
        uint32_t n=q->length-4;
        for(uint32_t i=0;i<n;++i)ln_storage_reply[4+i]=q->payload[4+i];
        if(!ln_storage_settings(true,ln_storage_reply+4,&n))return 1;
        ln_write_le32(ln_storage_reply+4,n);break;}
    case 85:{
        if(q->length!=40||ln_read_le32(q->payload)!=generation)return 1;
        if(!ln_storage_update(1,q->payload+8,ln_read_le32(q->payload+4),0))return 1;
        break;}
    case 86:{
        if(q->length<=8||q->length>1016||ln_read_le32(q->payload)!=generation)return 1;
        uint32_t offset=ln_read_le32(q->payload+4),n=q->length-8;
        if(offset>UINT32_MAX-n||!ln_storage_update(2,q->payload+8,n,offset))return 1;
        ln_write_le32(ln_storage_reply+4,offset+n);break;}
    case 87:
        if(q->length!=4||ln_read_le32(q->payload)!=generation||!ln_storage_update(3,0,0,0))return 1;
        break;
    case 88:{
        if(q->length)return 1;uint32_t words[16];ln_storage_update_state(words);
        for(unsigned i=0;i<16;++i)ln_write_le32(ln_storage_reply+i*4,words[i]);r->length=64;break;}
    case 89:{
        if(q->length!=8||ln_read_le32(q->payload)!=generation)return 1;
        uint32_t mode=ln_read_le32(q->payload+4);if(mode<1||mode>5||!ln_block_inject(mode))return 1;
        ln_write_le32(ln_storage_reply+4,mode);break;}
    case 90:
        /* Read-only FAT ownership proof. This never publishes filesystem data. */
        if(q->length!=4||ln_read_le32(q->payload)!=generation||ln_storage_prepare_writes()<0)return 1;
        break;
    case 91:
        if(q->length!=4||ln_read_le32(q->payload)!=generation||!ln_storage_recover())return 1;
        break;
    case 92:
        if(q->length!=4||ln_read_le32(q->payload)!=generation||ln_storage_restart_pending)return 1;
#if !defined(LINNET_IMAGE_OS)
        if(ln_storage_busy()||!ln_storage_prepared())return 1;
#endif
        ln_storage_restart_pending=1;break;
    case 93:{
        if(q->length!=8||ln_read_le32(q->payload)!=generation)return 1;
        uint32_t stage=ln_read_le32(q->payload+4);if(stage<1||stage>4||!ln_storage_publication_fault(stage))return 1;
        ln_write_le32(ln_storage_reply+4,stage);break;}
    case 96:{
        if(q->length!=12||ln_read_le32(q->payload)!=generation)return 1;
        uint32_t action=ln_read_le32(q->payload+4),size=ln_read_le32(q->payload+8);
        if(action<1||action>2||!size||size>0x100000u||!ln_storage_file_resize(action,size))return 1;
        ln_write_le32(ln_storage_reply+4,size);break;}
    default:return 1;
    }
    r->flags=1;return 1;
}
void ln_storage_link_ack_sent(uint16_t command){
    if(command!=92||!ln_storage_restart_pending)return;
#if defined(LINNET_IMAGE_OS)
    ln_runtime_restart();
#else
    ln_storage_restart_pending=0;(void)ln_storage_restart();ln_panic(35,0,0);
#endif
}
