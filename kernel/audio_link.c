#include "hal/api/platform.h"
#include "boot/include/linnet_bytes.h"
#include "kernel/audio_link.h"
#include "kernel/runtime.h"
#include "hal/api/audio.h"
#include "hal/api/core.h"
#include "hal/api/recovery.h"
#include "services/storage/storage.h"
#include "services/music/player.h"
#include "services/music/library.h"
#include "services/music/search.h"
#include "services/music/controller.h"
#include "services/audio/mp3/timing.h"
#if defined(LINNET_PHOTOS)
#include "services/photos/photos.h"
#endif
#if defined(LINNET_VIDEO)
#include "services/video/video.h"
#endif
static int16_t pcm[508];
int ln_audio_link_dispatch(const ln_link_frame *q,ln_link_frame *r,uint8_t *reply){
    if(q->command<112||q->command>124)return 0;
#if !defined(LINNET_PHOTOS)
    if(q->command==121)return 0;
#endif
#if !defined(LINNET_VIDEO)
    if(q->command==122||q->command==123||q->command==124)return 0;
#endif
    if(q->flags||!q->sequence||q->length>1024||(q->length&&!q->payload))return 0;
    uint32_t generation=ln_recovery_generation();
    r->command=q->command;r->sequence=q->sequence;r->flags=LINNET_LINK_ACK|LINNET_LINK_ERROR;
    r->payload=reply;r->length=8;ln_write_le32(reply,generation);ln_write_le32(reply+4,0);
#if defined(LINNET_VIDEO)
    if((ln_video_active()||ln_video_playback_pending())&&q->command>=113&&q->command<=118&&q->command!=116)return 1;
#endif
    struct ln_audio_status a;ln_audio_snapshot(&a);
    if(q->command==124){
#if defined(LINNET_VIDEO)
        if(q->length<6u||q->length>260u||ln_read_le32(q->payload)!=generation||q->payload[q->length-1u])return 1;
        for(uint32_t i=4u;i+1u<q->length;++i)if(!q->payload[i])return 1;
        if(!ln_runtime_video_open((const char *)q->payload+4u))return 1;
#else
        return 0;
#endif
    }else if(q->command==123){
#if defined(LINNET_VIDEO) && !defined(LINNET_PROFILE_RELEASE)
        if(q->length!=8||ln_read_le32(q->payload)!=generation||
           !ln_video_measurement_arm(ln_read_le32(q->payload+4)))return 1;
        ln_write_le32(reply+4,ln_read_le32(q->payload+4));
#else
        return 0;
#endif
    }else if(q->command==122){
#if defined(LINNET_VIDEO)
#if !defined(LINNET_PROFILE_RELEASE)
        if(q->length==4u&&ln_read_le32(q->payload)<9u){
            ln_video_metadata(ln_read_le32(q->payload),reply);r->length=128;r->flags=LINNET_LINK_ACK;return 1;
        }
#endif
        if(q->length)return 1;uint32_t words[32];ln_video_diagnostics(words);
        for(uint32_t n=0;n<32;++n)ln_write_le32(reply+n*4,words[n]);r->length=128;
#endif
    }else if(q->command==121){
#if defined(LINNET_PHOTOS)
        if(q->length)return 1;uint32_t words[32];ln_photos_diagnostics(words);
        for(uint32_t n=0;n<32;++n)ln_write_le32(reply+n*4,words[n]);r->length=128;
#endif
    }else if(q->command==120){
        if(q->length)return 1;struct ln_player_timing t;ln_player_timing_snapshot(&t);
        struct ln_mp3_timing m;ln_mp3_timing_snapshot(&m);
        struct ln_core_status c;ln_core_snapshot(&c);
        uint32_t words[31]={0x454d4954u,0,t.decode_calls,t.decode_us,t.decode_max_us,t.dsp_calls,t.dsp_frames,t.dsp_us,t.dsp_max_us,t.runtime_calls,t.runtime_us,t.runtime_max_us,t.music_us,t.storage_us,t.link_us,
            c.clock_hz,c.cache_control,ln_platform_clock_divider(),c.idle_us,c.clock_source,ln_player_decoder_kind(),
            m.frame_calls,m.frame_us,m.frame_max_us,m.synth_calls,m.synth_us,m.synth_max_us,
            m.pcm_calls,m.pcm_frames,m.pcm_us,m.pcm_max_us};
        for(uint32_t n=0;n<31;n++)ln_write_le32(reply+n*4,words[n]);r->length=124;
    }else if(q->command==119){
#if !defined(LINNET_PROFILE_RELEASE)
        if(q->length==4u&&ln_read_le32(q->payload)==1u){
            uint32_t words[16];ln_music_search_diagnostics(words);
            for(uint32_t i=0;i<16u;i++)ln_write_le32(reply+i*4u,words[i]);
            r->length=64;r->flags=LINNET_LINK_ACK;return 1;
        }
#if defined(LINNET_TARGET_VIDEO5G_M25)
        if(q->length==4u&&ln_read_le32(q->payload)==2u){
            uint32_t words[8];ln_music_library_verification_diagnostics(words);
            for(uint32_t i=0;i<8u;i++)ln_write_le32(reply+i*4u,words[i]);
            r->length=32;r->flags=LINNET_LINK_ACK;return 1;
        }
#endif
#endif
        if(q->length)return 1;
        uint32_t words[8]={0x5342494cu,0,ln_music_library_status(),(uint32_t)ln_music_library_error(),
            ln_music_library_count(),ln_music_library_group_count(),ln_music_library_verified_bytes(),ln_music_library_revision()};
        for(uint32_t i=0;i<8;++i)ln_write_le32(reply+i*4,words[i]);
        for(uint32_t i=32;i<64;++i)reply[i]=0;
        (void)ln_music_library_identity(reply+32);r->length=64;
    }else if(q->command==116){
        if(q->length)return 1;struct ln_player_status p;ln_player_snapshot(&p);
        uint64_t position=ln_player_position();
        uint32_t words[16]={0x52594c50u,p.phase,p.error,p.generation,p.rate,p.decoded,p.rendered,p.underruns,(uint32_t)p.frames,(uint32_t)(p.frames>>32),
            p.next_phase,p.next_error,p.gapless_joins,p.missed_joins,(uint32_t)position,(uint32_t)(position>>32)};
        for(uint32_t i=0;i<16;++i)ln_write_le32(reply+i*4,words[i]);r->length=64;
    }else if(q->command==117){
        if(q->length<6||q->length>260||ln_read_le32(q->payload)!=generation||q->payload[q->length-1]||a.initialized)return 1;
        if(!ln_player_open((const char*)q->payload+4))return 1;
    }else if(q->command==118){
        if((q->length!=8&&q->length!=16)||ln_read_le32(q->payload)!=generation)return 1;
        uint32_t action=ln_read_le32(q->payload+4);
        if((action==4)!=(q->length==16))return 1;
        if(action==1){if(!ln_player_pause(true))return 1;}
        else if(action==2){if(!ln_player_pause(false))return 1;}
        else if(action==3)ln_music_stop();
        else if(action==4){uint64_t frame=(uint64_t)ln_read_le32(q->payload+8)|((uint64_t)ln_read_le32(q->payload+12)<<32);
            if(!ln_music_seek(frame))return 1;}else return 1;
        ln_write_le32(reply+4,action);
    }else if(q->command==112){
#if defined(LINNET_SOC_S5L8702) && !defined(LINNET_PROFILE_RELEASE) && defined(LINNET_IMAGE_OS)
        if(q->length==4u && ln_read_le32(q->payload)==1u){
            extern void ln_s5l_audio_diagnostics(uint32_t out[16]);
            uint32_t words[16];ln_s5l_audio_diagnostics(words);
            for(uint32_t n=0;n<16u;++n)ln_write_le32(reply+n*4u,words[n]);
            r->length=64;r->flags=LINNET_LINK_ACK;return 1;
        }
#endif
        if(q->length)return 1;
        uint32_t words[20]={0x53445541u,0,a.rate,a.queued,a.free_frames,a.consumed,a.submitted,
            a.interrupts,a.underruns,a.silence_frames,a.fault,a.playing,a.paused,a.initialized,
            a.last_irq_us,a.max_irq_us,(uint32_t)a.volume_db_tenths,
            a.fifo_empty_irqs,a.max_completion_interval_us,a.max_completion_late_us};
        for(uint32_t i=0;i<20;++i)ln_write_le32(reply+i*4,words[i]);r->length=80;
    }else{
        if(q->length<8||ln_read_le32(q->payload)!=generation||ln_storage_busy()||ln_player_busy())return 1;
        uint32_t value=ln_read_le32(q->payload+4);
        if(q->command==113){
            if(q->length!=8||a.initialized||!ln_audio_init(value))return 1;
            ln_write_le32(reply+4,value);
        }else if(q->command==114){
            if(q->length<12||(q->length-8u)%4u||value!=a.submitted)return 1;
            uint32_t frames=(q->length-8u)/4u;if(frames>a.free_frames)return 1;
            for(uint32_t i=0;i<frames*2u;++i)pcm[i]=(int16_t)((uint16_t)q->payload[8+i*2]|(uint16_t)q->payload[9+i*2]<<8);
            if(ln_audio_submit(pcm,frames)!=frames)return 1;ln_write_le32(reply+4,value+frames);
        }else{
            if(q->length!=12)return 1;uint32_t arg=ln_read_le32(q->payload+8);
            if(value==1){if(arg||!ln_audio_start())return 1;}
            else if(value==2){if(arg)return 1;ln_audio_pause();}
            else if(value==3){if(arg||!ln_audio_stop())return 1;}
            else if(value==4){if(!a.initialized||!ln_audio_volume((int32_t)arg))return 1;}
            else if(value==5){if(arg||!a.initialized)return 1;ln_audio_finish();}
            else return 1;ln_write_le32(reply+4,value);
        }
    }
    r->flags=LINNET_LINK_ACK;return 1;
}
