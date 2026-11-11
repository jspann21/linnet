#include "services/games/games.h"
#include "services/games/package.h"
#include "services/games/saves.h"
#include "services/storage/storage.h"
#include "services/display/settings.h"
#include "services/music/controller.h"
#include "services/music/player.h"
#include "hal/api/core.h"
#include "hal/api/platform.h"
#include "hal/api/audio.h"
#include "boot/include/linnet_bytes.h"
#include "boot/include/sha256.h"

enum { CAT_OPEN,CAT_OPEN_WAIT,CAT_READ,CAT_READ_WAIT,CAT_VALIDATE,CAT_COPY,CAT_NEXT,
       IDLE,LOAD_OPEN,LOAD_OPEN_WAIT,LOAD_HEADER,LOAD_HEADER_WAIT,LOAD_DATA,LOAD_DATA_WAIT,
       LOAD_VERIFY,LOAD_ZERO,LOAD_SAVE,LOAD_ENTRY,ACTIVE,STOPPING,FAILED };
static uint32_t phase,cat_copy,cat_length,cat_generation,cat_count,cat_row,cat_bytes;
static uint32_t selected,offset,chunk,crc,file_crc,io_generation=0x474d0000u,requested=UINT32_MAX;
static uint32_t revision,paused_at,paused_us,zero_at,builtin_best[LN_GAME_COUNT];
static bool pending,ready,conflict,reentrant,held,save_can_write,exit_requested;
static bool background_music;
static uint8_t catalogue[LN_GAME_CATALOGUE_BYTES] __attribute__((aligned(32)));
static uint8_t header_data[256] __attribute__((aligned(32)));
static struct ln_fat32_file source;
static struct ln_game_header header;
static struct ln_game_module active;
static struct ln_sha256 hash;
static const char *notice="Loading games...";
extern uint8_t __game_slot[];
static uint8_t *memory(void){return __game_slot;}
static const uint8_t *row(uint32_t index){return catalogue+32u+index*256u;}
static bool same(const char *a,const char *b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
static uint32_t update_crc(uint32_t value,const uint8_t *data,uint32_t bytes){
    static const uint32_t table[16]={0,0x1db71064u,0x3b6e20c8u,0x26d930acu,
        0x76dc4190u,0x6b6b51f4u,0x4db26158u,0x5005713cu,0xedb88320u,0xf00f9344u,
        0xd6d6a3e8u,0xcb61b38cu,0x9b64c2b0u,0x86d3d2d4u,0xa00ae278u,0xbdbdf21cu};
    for(uint32_t i=0;i<bytes;++i){value^=data[i];value=(value>>4)^table[value&15u];value=(value>>4)^table[value&15u];}
    return value;
}
static uint32_t game_time(void){return (held?paused_at:ln_time_us())-paused_us;}
static uint32_t best(void){uint32_t value=ln_game_save_best();
    return header.builtin<LN_GAME_COUNT&&builtin_best[header.builtin]>value?builtin_best[header.builtin]:value;}
static bool record(uint32_t value){
    bool accepted=ln_game_save_record(value);
    if(header.builtin<LN_GAME_COUNT){uint16_t score=(uint16_t)(value>65535u?65535u:value);
        if(score>builtin_best[header.builtin]){builtin_best[header.builtin]=score;ln_display_game_best_set(header.builtin,score);}
        return true; /* The existing THMS owner also persists builtin best scores. */
    }return accepted;
}
static bool title(uint32_t index,char *out,uint32_t capacity){
    if(!out||!capacity)return false;const char *text=ln_music_library_title(index);uint32_t n=0;
    if(!text){out[0]=0;return false;}while(n+1u<capacity&&text[n]){out[n]=text[n];++n;}out[n]=0;return true;
}
static uint32_t current(void){const struct ln_music_library_row *r=ln_music_current();return r?r->index:UINT32_MAX;}
static bool music_playing(void){struct ln_player_status s;ln_player_snapshot(&s);return s.phase==LN_PLAYER_PLAY;}
static struct ln_game_host host={LN_GAME_ABI,sizeof host,0,
    &ln_ui_font,&ln_ui_hint_font,&ln_ui_clock_font,game_time,best,record,
    ln_game_save_read,ln_game_save_write,ln_music_library_count,title,current,
    ln_music_loading,ln_music_quiz_begin,ln_music_quiz_select,ln_music_quiz_end,
    ln_player_pause,ln_music_toggle,music_playing};
static void fail(const char *message){ln_music_quiz_end();ln_game_save_end();notice=message;phase=FAILED;++revision;}
static int open(const char *path){
    uint32_t next=io_generation+1u;int result=ln_storage_media_open(path,next);
    if(result>0){io_generation=next;pending=true;}return result;
}
static int read_at(uint32_t at,void *buffer,uint32_t bytes){
    uint32_t next=io_generation+1u;int result=ln_storage_media_read(&source,at,buffer,bytes,next);
    if(result>0){io_generation=next;pending=true;}return result;
}
static int poll(bool opened){
    uint32_t bytes=0;int result=ln_storage_media_poll_deferred(io_generation,opened?&source:0,&bytes);
    if(result){pending=false;if(result>0&&!opened&&bytes!=chunk)return -1;}return result;
}
uint32_t ln_games_count(void){return ready&&phase>=IDLE&&!conflict?cat_count:0;}
const char *ln_games_label(uint32_t index){return index<ln_games_count()?(const char *)row(index)+64u:0;}
const char *ln_games_description(uint32_t index){return index<ln_games_count()?(const char *)row(index)+104u:notice;}
void ln_games_best_set(uint32_t index,uint16_t score){if(index<LN_GAME_COUNT&&score>builtin_best[index])builtin_best[index]=score;}
uint32_t ln_games_hud_revision(void){return revision+(phase==ACTIVE?active.revision():0);}
bool ln_games_playing(void){return phase==ACTIVE&&!held&&active.playing();}
const char *ln_games_text(const char *key){
    if(key&&same(key,"Game music"))return music_playing()?"Play: Pause music":"Play: Start/resume music";
    return ""; /* The canvas owns loading, error and gameplay text. */
}
void ln_games_music_toggle(void){ln_music_toggle();++revision;}
void ln_games_start(uint32_t index){
    if(!ready||index>=cat_count||conflict)return;
    if(phase!=IDLE){requested=index;return;}
    selected=index;phase=LOAD_OPEN;notice="Loading game...";paused_us=0;held=false;++revision;
}
void ln_games_stop(void){
    if(phase==ACTIVE){void (*stop)(void)=active.stop;active=(struct ln_game_module){0};
        phase=STOPPING;stop();ln_music_quiz_end();ln_games_audio_stop();ln_game_save_end();}
    else if(phase>=LOAD_OPEN&&phase<=LOAD_ENTRY){if(pending)ln_storage_media_cancel(io_generation);phase=STOPPING;ln_game_save_end();}
    else if(phase==FAILED){phase=IDLE;notice="";}
}
int ln_games_input(uint32_t kind,int32_t value){
    if(phase==ACTIVE){int32_t result=active.input(kind,value);
        if(result==LN_GAME_EXIT)ln_games_stop();
        return result>=LN_GAME_UNHANDLED&&result<=LN_GAME_CONSUMED?result:LN_GAME_CONSUMED;}
    if(kind==LN_GAME_INPUT_BUTTONS&&((uint32_t)value&LN_GAME_BUTTON_MENU)){ln_games_stop();return LN_GAME_EXIT;}
    return LN_GAME_CONSUMED;
}
bool ln_games_tick(uint32_t now){(void)now;return phase==ACTIVE&&!held&&active.tick(game_time());}
uint32_t ln_games_deadline(uint32_t now){
    if(pending||ln_game_save_work(save_can_write))return now+1000u;
    if(ln_storage_mounted()&&(phase<IDLE||phase==STOPPING||(phase>=LOAD_OPEN&&phase<=LOAD_ENTRY)))return now+1000u;
    if(phase!=ACTIVE||held)return UINT32_MAX;
    uint32_t due=active.deadline(game_time());return due==UINT32_MAX?due:due+paused_us;
}
static bool exports(const struct ln_game_module *module){
    uintptr_t base=(uintptr_t)memory(),p=(uintptr_t)module;
    if(header.payload<sizeof *module||p<base||(p&3u)||p-base>header.payload-sizeof *module)return false;
    if(module->abi!=LN_GAME_ABI||module->bytes<sizeof *module||module->bytes>header.payload-(p-base))return false;
    const uintptr_t pointers[]={(uintptr_t)module->start,(uintptr_t)module->input,(uintptr_t)module->tick,
        (uintptr_t)module->deadline,(uintptr_t)module->draw,(uintptr_t)module->playing,
        (uintptr_t)module->revision,(uintptr_t)module->take_cue,(uintptr_t)module->stop};
    for(uint32_t i=0;i<sizeof pointers/sizeof pointers[0];++i)
        if(pointers[i]<base||(pointers[i]&3u)||pointers[i]-base>header.text-4u)return false;
    return true;
}
void ln_games_step(uint32_t now,bool visible,bool hold,bool may_write){
    if(reentrant)return;reentrant=true;
    bool playing=music_playing();if(playing!=background_music){background_music=playing;++revision;}
    save_can_write=may_write;
    if(!visible){requested=UINT32_MAX;ln_games_stop();}
    if(phase==ACTIVE&&hold!=held){
        if(hold){paused_at=now;held=true;}else{paused_us+=now-paused_at;held=false;}
        int32_t result=active.input(LN_GAME_INPUT_HOLD,hold?1:0);
        if(result==LN_GAME_CHANGED)++revision;
        else if(result==LN_GAME_EXIT){ln_games_stop();exit_requested=true;}
    }
    ln_game_save_step(may_write);
    if(!ln_storage_mounted()){
        if(pending&&!poll(false))goto done;
        if(!ln_storage_startup_complete())goto done;
        if(phase<IDLE){ready=true;cat_count=0;phase=IDLE;notice="Game storage unavailable";++revision;}
        else if(phase==STOPPING&&ln_game_save_ready()){phase=IDLE;notice="";++revision;}
        else if(phase>=LOAD_OPEN&&phase<=LOAD_ENTRY)fail("Game storage unavailable");
        goto done;
    }
    int result;
    switch(phase){
    case CAT_OPEN:{const char *path=cat_copy?"/.LINNET/GAMES/INDEX1.BIN":"/.LINNET/GAMES/INDEX0.BIN";
        result=open(path);if(result>0)phase=CAT_OPEN_WAIT;else if(result<0)phase=CAT_NEXT;break;}
    case CAT_OPEN_WAIT:
        result=poll(true);if(!result)break;if(result<0||source.size<36u||source.size>LN_GAME_CATALOGUE_BYTES||((source.size-36u)&255u)){phase=CAT_NEXT;break;}
        cat_length=source.size;offset=0;crc=~0u;phase=CAT_READ;break;
    case CAT_READ:
        chunk=cat_length-offset;if(chunk>1024u)chunk=1024u;
        result=read_at(offset,memory()+offset,chunk);if(result>0)phase=CAT_READ_WAIT;else if(result<0)phase=CAT_NEXT;break;
    case CAT_READ_WAIT:
        result=poll(false);if(!result)break;if(result<0){phase=CAT_NEXT;break;}
        {uint32_t prefix=offset<cat_length-4u?cat_length-4u-offset:0;
        if(prefix>chunk)prefix=chunk;crc=update_crc(crc,memory()+offset,prefix);}offset+=chunk;
        if(offset<cat_length){phase=CAT_READ;break;}
        if(~crc!=ln_read_le32(memory()+cat_length-4u)||!ln_game_catalogue_header(memory(),cat_length,LINNET_TARGET_NUMBER,&cat_generation,&cat_count)){
            cat_count=ready?ln_read_le32(catalogue+16u):0;phase=CAT_NEXT;break;}
        cat_row=0;phase=CAT_VALIDATE;break;
    case CAT_VALIDATE:
        if(cat_row<cat_count){if(!ln_game_catalogue_row(memory(),cat_row++)){cat_count=ready?ln_read_le32(catalogue+16u):0;phase=CAT_NEXT;}break;}
        if(ready&&cat_generation==ln_read_le32(catalogue+8u)){
            if(cat_length!=cat_bytes||!ln_bytes_equal(memory(),catalogue,cat_length))conflict=true;
            cat_count=ln_read_le32(catalogue+16u);phase=CAT_NEXT;break;}
        if(ready&&cat_generation<ln_read_le32(catalogue+8u)){cat_count=ln_read_le32(catalogue+16u);phase=CAT_NEXT;break;}
        offset=0;phase=CAT_COPY;break;
    case CAT_COPY:
        chunk=cat_length-offset;if(chunk>1024u)chunk=1024u;
        for(uint32_t i=0;i<chunk;++i)catalogue[offset+i]=memory()[offset+i];offset+=chunk;
        if(offset==cat_length){ready=true;cat_bytes=cat_length;phase=CAT_NEXT;}break;
    case CAT_NEXT:
        if(!cat_copy){cat_copy=1;phase=CAT_OPEN;break;}
        ready=true;cat_count=cat_bytes?ln_read_le32(catalogue+16u):0;
        if(conflict){cat_count=0;notice="Game catalogue conflict";}else notice=cat_count?"":"Install games in Media Manager";
        phase=IDLE;++revision;break;
    case LOAD_OPEN:{char path[48]="/.LINNET/GAMES/";uint32_t at=15u;
        const uint8_t *p=row(selected)+184u;for(uint32_t i=0;i<13u;++i)path[at+i]=(char)p[i];
        result=open(path);if(result>0)phase=LOAD_OPEN_WAIT;else if(result<0)fail("Game file unavailable");break;}
    case LOAD_OPEN_WAIT:
        result=poll(true);if(!result)break;if(result<0||source.size!=ln_read_le32(row(selected)+200u)){fail("Game file size mismatch");break;}
        phase=LOAD_HEADER;break;
    case LOAD_HEADER:
        chunk=256u;result=read_at(0,header_data,chunk);if(result>0)phase=LOAD_HEADER_WAIT;else if(result<0)fail("Cannot read game header");break;
    case LOAD_HEADER_WAIT:
        result=poll(false);if(!result)break;
        if(result<0||!ln_game_header_decode(header_data,LINNET_TARGET_NUMBER,(uint32_t)(uintptr_t)memory(),&header)||
           source.size!=header.payload+256u||!same(header.id,(const char *)row(selected))||
           header.builtin!=ln_read_le32(row(selected)+208u)||header.capabilities!=ln_read_le32(row(selected)+212u)){
            fail("Incompatible game package");break;}
        crc=~0u;file_crc=update_crc(~0u,header_data,256u);offset=0;ln_sha256_init(&hash);phase=LOAD_DATA;break;
    case LOAD_DATA:
        chunk=header.payload-offset;if(chunk>1024u)chunk=1024u;
        result=read_at(256u+offset,memory()+offset,chunk);if(result>0)phase=LOAD_DATA_WAIT;else if(result<0)fail("Cannot load game");break;
    case LOAD_DATA_WAIT:
        result=poll(false);if(!result)break;if(result<0){fail("Game read failed");break;}
        crc=update_crc(crc,memory()+offset,chunk);file_crc=update_crc(file_crc,memory()+offset,chunk);
        ln_sha256_update(&hash,memory()+offset,chunk);offset+=chunk;phase=offset<header.payload?LOAD_DATA:LOAD_VERIFY;break;
    case LOAD_VERIFY:{uint8_t digest[32];ln_sha256_finish(&hash,digest);
        if(~crc!=header.crc||~file_crc!=ln_read_le32(row(selected)+204u)||!ln_bytes_equal(digest,header.sha256,32u)){
            fail("Game integrity check failed");break;}zero_at=0;phase=LOAD_ZERO;break;}
    case LOAD_ZERO:
        chunk=header.bss-zero_at;if(chunk>1024u)chunk=1024u;
        for(uint32_t i=0;i<chunk;++i)memory()[header.payload+zero_at+i]=0;zero_at+=chunk;
        if(zero_at==header.bss){ln_game_save_begin(header.id,ln_read_le32(row(selected)+216u),header.schema,
            header.builtin<LN_GAME_COUNT?builtin_best[header.builtin]:0);phase=LOAD_SAVE;}break;
    case LOAD_SAVE:if(ln_game_save_ready())phase=LOAD_ENTRY;break;
    case LOAD_ENTRY:{
        if(!ln_core_code_publish(memory(),header.payload+header.bss)){fail("Game cache publication failed");break;}
        host.capabilities=header.capabilities;
        ln_game_entry entry=(ln_game_entry)(uintptr_t)(header.load+header.entry);
        const struct ln_game_module *module=entry(&host);
        if(!exports(module)){fail("Invalid game exports");break;}
        active=*module;
        if(!active.start()){void (*stop)(void)=active.stop;active=(struct ln_game_module){0};stop();ln_music_quiz_end();fail("Game initialization failed");break;}
        phase=ACTIVE;notice="";++revision;break;}
    case STOPPING:
        if(pending){result=poll(false);if(!result)break;}
        if(!ln_game_save_ready())break;
        phase=IDLE;notice="";++revision;
        if(visible&&requested!=UINT32_MAX){uint32_t index=requested;requested=UINT32_MAX;ln_games_start(index);}break;
    default:break;
    }
done:reentrant=false;
}
bool ln_games_take_exit(void){bool value=exit_requested;exit_requested=false;return value;}
bool ln_games_pending(void){return pending||phase==ACTIVE||phase==STOPPING||
    (phase>=LOAD_OPEN&&phase<=LOAD_ENTRY)||ln_game_save_busy();}
bool ln_games_quiesce(bool publish){
    requested=UINT32_MAX;ln_games_stop();
    if(phase<IDLE){if(pending)ln_storage_media_cancel(io_generation);phase=STOPPING;}
    uint32_t start=ln_time_us();
    while(ln_games_pending()){
        uint32_t now=ln_time_us();if(now-start>120000000u)return false;
        ln_games_step(now,false,false,publish);
        ln_music_step();ln_audio_service(now);ln_storage_step_deferred();ln_core_watchdog_feed();
        if(!publish&&!pending&&phase==IDLE&&!ln_game_save_inflight())break;
    }return !publish||!ln_game_save_failed();
}
/* Prepared font view; status rendering performs no file reads or allocations. */
static void status(const struct ln_ui_canvas_context *c,const char *text,int32_t y){
    const struct ln_ui_font *f=&ln_ui_hint_font;int32_t x=8;
    for(;*text;++text){uint32_t ch=(uint8_t)*text;if(ch<f->first||ch-f->first>=f->count)continue;uint32_t n=ch-f->first;
        uint32_t w=f->width,h=f->height,offset=n*w*h;int32_t gx=x,gy=y;
        if(f->glyphs){const struct ln_ui_glyph *g=f->glyphs+n;w=g->w;h=g->h;offset=g->offset;gx+=g->x;gy+=g->y;}
        for(uint32_t j=0;j<h;++j)for(uint32_t i=0;i<w;++i){int32_t px=gx+(int32_t)i,py=gy+(int32_t)j;
            if(px>=c->clip_x&&px<c->clip_x+c->clip_w&&py>=c->clip_y&&py<c->clip_y+c->clip_h&&f->alpha[offset+j*w+i]>127u)
                c->pixels[py*LINNET_UI_WIDTH+px]=0xffffu;}
        x+=(int32_t)(f->advances?f->advances[n]:f->advance);
    }
}
void ln_games_draw(const struct ln_ui_canvas_context *c){
    if(phase==ACTIVE){active.draw(c);return;}
    for(int32_t y=c->clip_y;y<c->clip_y+c->clip_h;++y)for(int32_t x=c->clip_x;x<c->clip_x+c->clip_w;++x)
        c->pixels[y*LINNET_UI_WIDTH+x]=0x0841u;
    status(c,notice,LINNET_UI_HEIGHT/2u);status(c,"Menu: back",LINNET_UI_HEIGHT-24u);
}
static bool audio_owned;
static struct ln_game_cue queued_cue;
void ln_games_audio_stop(void){if(audio_owned){(void)ln_audio_stop();audio_owned=false;}queued_cue.kind=0;}
void ln_games_audio_step(void){
    if(phase==ACTIVE){struct ln_game_cue cue;if(active.take_cue(&cue))queued_cue=cue;}
    if(ln_player_busy()){audio_owned=false;queued_cue.kind=0;return;}
    if(ln_music_loading()){ln_games_audio_stop();return;}
    struct ln_audio_status s;ln_audio_snapshot(&s);
    if(audio_owned&&!s.playing&&!queued_cue.kind){ln_games_audio_stop();return;}
    if(!queued_cue.kind||(audio_owned&&s.playing))return;
    struct ln_game_cue cue=queued_cue;queued_cue.kind=0;
    if(cue.kind>9u||!ln_audio_init(22050u))return;
    audio_owned=true;int16_t pcm[512];uint32_t phase_value=0;
    uint32_t step=cue.kind>=5u&&cue.kind<=8u?0x04000000u+(cue.kind-5u)*0x01000000u:
        cue.kind==3u?0x03000000u:cue.kind==4u?0x02000000u:cue.kind==2u?0x08000000u:0x06000000u;
    for(uint32_t part=0;part<(cue.kind==9u?16u:4u);++part){
        if(cue.kind==9u)step=0x04000000u+(uint32_t)(cue.notes[part/4u]&3u)*0x01000000u;
        for(uint32_t i=0;i<256u;++i){uint32_t at=part*256u+i,wave=(phase_value>>23)&511u;
            int32_t triangle=(int32_t)(wave<256u?wave:511u-wave)-128;
            uint32_t envelope=cue.kind==9u?1024u-(at&1023u):1024u-at;
            int16_t sample=(int16_t)((triangle*(int32_t)envelope*38)>>10);
            pcm[i*2u]=pcm[i*2u+1u]=sample;phase_value+=step;}
        if(ln_audio_submit(pcm,256u)!=256u){ln_games_audio_stop();return;}
    }
    ln_audio_finish();if(!ln_audio_start())ln_games_audio_stop();
}
