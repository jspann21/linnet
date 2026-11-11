#include "boot/include/linnet_bytes.h"
#include "linnet_scene_ids.h"
#include "services/notes/notes.h"
#include "services/storage/storage.h"
#define NOTES_MAX 256u
#define NOTES_RECORD 8196u
#define CACHE_ROWS 16u
static struct ln_fat32_file file;
/* One bounded read-ahead window, reused for titles and body lines. Never retain the
 * complete catalog or document in the firmware's fixed image arena. */
static uint8_t header[16],cache[CACHE_ROWS*64u],selected_title[64];
static uint32_t phase,generation=0x4e000001u,count,lines,selected,revision,drawn;
static uint32_t cache_first,cache_count,request_first,request_count;
static bool active,pending,reading,discard;
static const char *status="Prepare Notes on your computer";
static bool scene(const struct ln_ui_state*s){return s&&s->scene<ln_ui_scene_count&&s->scene==LN_UI_SCENE_NOTES;}
static bool text_valid(const uint8_t*p,uint32_t n){bool zero=false;for(uint32_t i=0;i<n;++i){if(!p[i])zero=true;else if(p[i]<32u||p[i]>126u)return false;}return zero;}
static void fail(void){phase=0;count=lines=cache_count=0;reading=false;status="Notes unavailable - prepare again";++revision;}
void ln_notes_close(void){active=false;if(pending)ln_storage_media_cancel(generation);else phase=0;}
bool ln_notes_pending(void){return pending||phase!=0;}
uint32_t ln_notes_count(void){return reading?lines:count?count:1u;}
const char *ln_notes_title(void){return reading?(const char*)selected_title:"Notes";}
const char *ln_notes_label(uint32_t row){
    if(!count)return status;
    if(row<cache_first||row-cache_first>=cache_count)return "Loading...";
    return (const char*)(cache+(row-cache_first)*(reading?32u:64u));
}
void ln_notes_step(struct ln_ui_state*s){
    if(!scene(s)&&active)ln_notes_close();
    if(scene(s)&&!active&&!pending){active=true;reading=discard=false;count=lines=cache_count=0;phase=1;++generation;status="Loading Notes...";++revision;}
    if(pending){
        uint32_t transferred=0;int result=ln_storage_media_poll_deferred(generation,&file,&transferred);
        if(!result)return;pending=false;
        if(!active){phase=0;return;}
        if(discard){discard=false;phase=0;}
        else if(result<0){fail();}
        else if(phase==1){phase=2;}
        else if(phase==2){
            count=ln_read_le32(header+8);
            if(transferred!=16u||ln_read_le32(header)!=0x45544f4eu||ln_read_le32(header+4)||count>NOTES_MAX||ln_read_le32(header+12)!=NOTES_RECORD||file.size!=16u+count*(64u+NOTES_RECORD))fail();
            else{phase=0;status="No Notes - add text files";++revision;}
        }else if(phase==3||phase==5){
            uint32_t stride=phase==3?64u:32u;bool valid=transferred==request_count*stride;
            for(uint32_t i=0;i<request_count&&valid;++i)valid=text_valid(cache+i*stride,stride);
            if(!valid)fail();else{phase=0;cache_first=request_first;cache_count=request_count;++revision;}
        }else if(phase==4){
            lines=ln_read_le32(header);
            if(transferred!=4u||!lines||lines>256u)fail();
            else{phase=0;reading=true;cache_count=0;s->selected=0;++revision;}
        }
    }
    if(active&&!phase&&count&&scene(s)){
        uint32_t capacity=ln_ui_visible_capacity(s);if(!capacity)capacity=1;
        if(capacity>CACHE_ROWS){fail();return;}
        uint32_t total=reading?lines:count,first=ln_ui_first_visible(s);
        if(first>=total)first=0;
        uint32_t wanted=total-first;if(wanted>capacity)wanted=capacity;
        if(first<cache_first||cache_count<wanted||first-cache_first>cache_count-wanted){
            request_count=total<CACHE_ROWS?total:CACHE_ROWS;
            uint32_t margin=(CACHE_ROWS-capacity)/2u;
            request_first=first>margin?first-margin:0;
            if(request_first>total-request_count)request_first=total-request_count;
            cache_count=0;phase=reading?5u:3u;
        }
    }
    if(active&&phase&&!pending){
        uint32_t body_offset=16u+count*64u+selected*NOTES_RECORD;
        int result=phase==1?ln_storage_media_open("/.LINNET/NOTES/LIBRARY.NNT",generation):
            phase==2?ln_storage_media_read(&file,0,header,16,generation):
            phase==3?ln_storage_media_read(&file,16u+request_first*64u,cache,request_count*64u,generation):
            phase==4?ln_storage_media_read(&file,body_offset,header,4u,generation):
            ln_storage_media_read(&file,body_offset+4u+request_first*32u,cache,request_count*32u,generation);
        if(result>0)pending=true;else if(result<0)fail();
    }
    if(scene(s)&&drawn!=revision){drawn=revision;ln_ui_refresh(s);}
}
bool ln_notes_input(struct ln_ui_state*s,uint32_t kind,int32_t value){
    if(!scene(s)||s->hold||kind!=2u)return false;
    uint32_t pressed=(uint32_t)value&~s->buttons;
    if(!(pressed&(1u|4u|8u|16u)))return false;
    s->buttons=(uint32_t)value;
    if(pressed&1u){
        if(reading){
            if(pending){ln_storage_media_cancel(generation);discard=true;}else phase=0;
            reading=false;lines=cache_count=0;s->selected=selected;++revision;ln_ui_refresh(s);
        }else{ln_notes_close();(void)ln_ui_show(s,"linnet.system.home");}
    }else if(pressed&(4u|8u)){
        uint32_t total=ln_notes_count();
        if((pressed&4u)&&s->selected+1u<total)++s->selected;
        if((pressed&8u)&&s->selected)--s->selected;
        ln_ui_refresh(s);
    }else if(!reading&&!phase&&count&&s->selected>=cache_first&&s->selected-cache_first<cache_count){
        selected=s->selected;for(uint32_t i=0;i<64u;++i)selected_title[i]=cache[(selected-cache_first)*64u+i];
        phase=4;status="Loading note...";
    }
    return true;
}
