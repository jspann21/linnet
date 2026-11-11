#include "services/video/video.h"
#include "services/video/catalog.h"
#include "targets/video5g-m25/coprocessor.h"
#include "hal/api/display.h"
#include "hal/api/platform.h"
#include "hal/api/ata.h"
#include "services/storage/storage.h"
static struct {char path[256],title[128];uint32_t identity,position,revision,backend_revision,resume[2];uint8_t options,output_once;bool active,completed,resume_pending,headphone_known,headphones;} v;
void ln_video_init(void){v.active=v.completed=v.resume_pending=false;v.position=0;v.options=8u;v.output_once=0;v.headphone_known=false;}
bool ln_video_open(const char *path){
    if(v.active||ln_m25_coprocessor_active()||ln_video_catalog_pending()||ln_display_busy()||!path)return false;
    uint32_t n=0,identity=2166136261u;while(path[n]&&n<255u){identity=(identity^(uint8_t)path[n])*16777619u;++n;}if(!n||n==255u)return false;
    bool television=(v.options&3u)==2u||((v.options&3u)==1u&&v.output_once==2u);
    uint8_t resolved=(uint8_t)(v.options|(television?32u:0u));v.output_once=0;
    bool opened=ln_m25_coprocessor_open(path,resolved);
    for(uint32_t i=0;i<=n;++i)v.path[i]=path[i];const char *name=path;
    for(uint32_t i=0;i<n;++i)if(path[i]=='/')name=path+i+1u;
    uint32_t i=0;while(name[i]&&i<127u){v.title[i]=name[i];++i;}v.title[i]=0;
    v.identity=identity;v.position=0;v.completed=false;v.resume_pending=v.resume[0]==identity&&v.resume[1]>0u;v.active=opened;++v.revision;return true;
}
bool ln_video_open_file(const struct ln_fat32_file*file,const char*title){
    if(!file||!title||!ln_video_open(title))return false;
    if(v.active)ln_m25_coprocessor_file(file);return true;
}
void ln_video_step(void){
    struct ln_ata_state disk;ln_ata_state_snapshot(&disk);
    /* A cold HDD wake can exceed the PCM reserve. Keep it ready even when
     * extended RAM temporarily supplies every packet and index read. */
    ln_storage_media_realtime(v.active&&disk.media_kind==LN_ATA_MEDIA_ROTATING&&ln_m25_coprocessor_disk_required());
    ln_video_catalog_step(ln_m25_coprocessor_active());
    if(v.active&&ln_m25_coprocessor_active())v.position=ln_m25_coprocessor_position();
    /* Repaint current UI state after restoration before enabling its light;
     * the retained preparation frame can still contain the startup cue. */
    if(ln_m25_coprocessor_step()){
        ln_display_damage(0,0,LINNET_UI_WIDTH,LINNET_UI_HEIGHT);
        if(v.active)++v.revision;else (void)ln_display_present();
    }
    if(v.active){
        if(v.resume_pending&&ln_m25_coprocessor_duration()){
            if(ln_m25_coprocessor_seek(v.resume[1]))v.resume_pending=false;
        }
        if(!ln_m25_coprocessor_active()){
            v.completed=ln_m25_coprocessor_completed();
            if(v.completed)v.position=0;
            v.resume[0]=v.identity;v.resume[1]=v.position;v.active=false;++v.revision;
        }
    }
    uint32_t revision=ln_m25_coprocessor_revision();if(revision!=v.backend_revision){v.backend_revision=revision;++v.revision;}
}
void ln_video_stop(void){v.completed=false;if(v.active){v.resume[0]=v.identity;v.resume[1]=v.position;v.resume_pending=false;ln_m25_coprocessor_stop();}}
bool ln_video_active(void){return v.active;}
bool ln_video_completed(void){return v.completed&&!v.active;}
bool ln_video_pending(void){return ln_video_catalog_pending();}
bool ln_video_playback_pending(void){return false;}
void ln_video_pause(bool paused){if(v.active)ln_m25_coprocessor_pause(paused);}
bool ln_video_controls(bool show){(void)show;ln_video_pause(!ln_video_paused());return false;}
bool ln_video_paused(void){return v.active&&ln_m25_coprocessor_paused();}
bool ln_video_seek(int32_t seconds){
    if(!v.active)return false;int64_t position=(int64_t)v.position+(int64_t)seconds*1000;
    if(position<0)position=0;if(position>0xffffffffu)position=0xffffffffu;v.resume_pending=false;
    return ln_m25_coprocessor_seek((uint32_t)position);
}
uint8_t ln_video_options(void){return v.options;}
void ln_video_options_set(uint8_t options){options&=31u;if(options!=v.options){v.options=options;++v.revision;}}
const char *ln_video_option_label(uint32_t row){
    if(row==0)return (v.options&3u)==2u?"TV Out: On":(v.options&3u)==1u?"TV Out: Ask":"TV Out: Off";
    if(row==1)return v.options&4u?"TV Signal: PAL":"TV Signal: NTSC";
    if(row==2)return v.options&8u?"Widescreen: On":"Widescreen: Off";
    if(row==3)return v.options&16u?"TV Connection: S-Video":"TV Connection: Composite";
    return "";
}
bool ln_video_option_adjust(uint32_t row,int32_t direction){
    if(v.active||!direction||row>3u)return false;
    if(row==0){uint8_t mode=v.options&3u;mode=(uint8_t)(direction<0?(mode?mode-1u:2u):(mode==2u?0u:mode+1u));v.options=(uint8_t)((v.options&~3u)|mode);}
    else v.options^=(uint8_t)(1u<<(row+1u));
    ++v.revision;return true;
}
bool ln_video_output_prompt(void){return (v.options&3u)==1u;}
void ln_video_output_once(bool television){v.output_once=television?2u:1u;}
void ln_video_headphones(bool connected){
    if(v.active&&v.headphone_known&&v.headphones&&!connected)ln_video_pause(true);
    v.headphones=connected;v.headphone_known=true;
}
uint32_t ln_video_progress(void){uint32_t duration=ln_m25_coprocessor_duration(),position=v.position>duration?duration:v.position;if(!duration)return 0;while(duration>0x3fffffu){duration>>=1;position>>=1;}return position*1000u/duration;}
const char *ln_video_title(void){return v.title;}
/* Acknowledge selection before BCM takes the display for its short handoff. */
const char *ln_video_status(void){return ln_m25_coprocessor_error()?"Video unavailable":ln_m25_coprocessor_active()&&!ln_m25_coprocessor_owned()?"Starting video...":"";}
const struct ln_ui_asset *ln_video_asset(void){return 0;}
uint32_t ln_video_revision(void){return v.revision;}
void ln_video_presented(void){}
void ln_video_resume_get(uint32_t out[2]){out[0]=v.active?v.identity:v.resume[0];out[1]=v.active?v.position:v.resume[1];}
void ln_video_resume_set(const uint32_t in[2]){v.resume[0]=in[0];v.resume[1]=in[1];}
uint32_t ln_video_deadline(uint32_t now){return ln_m25_coprocessor_deadline(now);}
#if !defined(LINNET_PROFILE_RELEASE)
void ln_video_diagnostics(uint32_t out[32]){uint32_t command[32];ln_m25_coprocessor_snapshot(out);ln_m25_coprocessor_channel(10u,command);out[24]=v.position;out[25]=ln_m25_coprocessor_duration();out[26]=v.active;out[27]=command[7];out[28]=v.options;out[29]=ln_video_paused();}
bool ln_video_measurement_arm(uint32_t mode){return mode==0u&&!v.active;}
void ln_video_metadata(uint32_t page,uint8_t out[128]){for(uint32_t i=0;i<128u;++i)out[i]=page<2u?(uint8_t)v.path[page*128u+i]:page==2u?(uint8_t)v.title[i]:0u;}
#endif
