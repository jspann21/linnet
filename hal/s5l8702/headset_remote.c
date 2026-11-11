#include "hal/api/input.h"
#if defined(LINNET_INPUT_REMOTE)
#include "headset_remote.h"
#include "i2c.h"

/* Original Linnet driver. Register facts and provenance: docs/headset-remote.md.
 * The optional controller is fitted to the 2008 and 2009 Classic boards.
 * Never infer its presence from disk size or fail clickwheel initialization
 * when it is absent. All bus work runs in the foreground, outside the wheel IRQ. */
/* Literature uses the on-wire write address 0x72. This HAL takes seven bits. */
#define JACK_ADDRESS (0x72u >> 1)
#define INSERT_US 150000u
#define RETRY_US 2000000u
#define CLICK_GAP_US 450000u
#define RELEASE_US 40000u
#define REPEAT_DELAY_US 500000u
#define REPEAT_US 120000u
#define VOLUME_LIMIT_US 5000000u
static uint32_t next_probe,settle_at,mode_at,release_at,click_at,repeat_at,volume_at;
static uint32_t samples,failures,center_presses,raw;
static uint16_t up_presses,down_presses;
static uint8_t volume_keys,clicks,last_action;
static bool connected,present,armed,center,release_tracking,volume_limited,fault;

static bool due(uint32_t now,uint32_t at){return (int32_t)(now-at)>=0;}
static bool read_register(uint8_t reg,uint8_t *value){
    return ln_s5l_i2c_read(0,JACK_ADDRESS,reg,value,1);
}
static void clear_keys(void){
    volume_keys=clicks=0;center=false;release_tracking=false;volume_limited=false;
}
void ln_s5l_remote_init(void){
    connected=present=armed=fault=false;clear_keys();
    last_action=0;
    samples=failures=center_presses=raw=0;up_presses=down_presses=0;
    next_probe=settle_at=mode_at=release_at=click_at=repeat_at=volume_at=0;
}
static void failed(uint32_t now){
    ++failures;fault=true;armed=false;clear_keys();next_probe=now+RETRY_US;
}
void ln_s5l_remote_poll(uint32_t now,bool headphones,uint8_t *actions,int8_t *volume){
    *actions=0;*volume=0;
    if(headphones!=connected){
        connected=headphones;clear_keys();armed=false;raw=0;
        next_probe=now+INSERT_US;
        if(!connected&&present){
            const uint8_t off=0;
            if(!ln_s5l_i2c_write(0,JACK_ADDRESS,0,&off,1))++failures;
        }
    }
    if(!connected)return;
    if(!armed){
        if(!due(now,next_probe))return;
        uint8_t mode;
        if(!read_register(0,&mode)){present=false;failed(now);return;}
        present=true;
        const uint8_t enabled=0x2fu;
        if(!ln_s5l_i2c_write(0,JACK_ADDRESS,0,&enabled,1)){failed(now);return;}
        armed=true;fault=false;clear_keys();
        /* Start disarmed for center: jack insertion must first show a stable
         * released sample. Discard accessory-ID transients while bias settles. */
        center=true;settle_at=now+INSERT_US;mode_at=now;return;
    }
    if(!due(now,settle_at))return;
    uint8_t mode=(uint8_t)raw,r4,r5,r6;
    if(due(now,mode_at)){
        if(!read_register(0,&mode)){failed(now);return;}
        mode_at=now+100000u;
        /* Tolerate mode readback with reserved bits omitted. A reset loses it. */
        if((mode&0x77u)!=0x27u){armed=false;clear_keys();next_probe=now+INSERT_US;return;}
    }
    if(!read_register(4,&r4)||!read_register(5,&r5)||!read_register(6,&r6)){
        failed(now);return;
    }
    ++samples;raw=mode|((uint32_t)r4<<8)|((uint32_t)r5<<16)|((uint32_t)r6<<24);
    /* Bit 0 is the center pressed level. Bit 2 can
     * reappear alone (0x84) after release: counting it creates phantom clicks
     * and can hide the released interval before the next real press. */
    bool pressed=(r4&1u)!=0;
    if(pressed){
        release_tracking=false;
        if(!center){
            center=true;++center_presses;
            if(clicks<3u)++clicks;
        }
        if(clicks)click_at=now+CLICK_GAP_US;
    }else if(center){
        if(!release_tracking){release_tracking=true;release_at=now+RELEASE_US;}
        else if(due(now,release_at)){center=false;release_tracking=false;}
    }
    /* Group by the released gap, not press-to-press time: a longer press
     * must not consume the user's entire double/triple-click window. Never
     * publish a shorter gesture while another click is still down. */
    if(clicks&&!center&&due(now,click_at)){
        *actions=clicks==1u?LINNET_REMOTE_PLAY_PAUSE:
            clicks==2u?LINNET_REMOTE_NEXT:LINNET_REMOTE_PREVIOUS;
        last_action=*actions;clicks=0;
    }
    uint8_t before=volume_keys;
    /* Accessory-ID bits 4..5 are deliberately excluded from button decoding.
     * Release wins if both edges accumulated between two foreground samples. */
    if(r5&4u)volume_keys|=1u;
    if(r5&1u)volume_keys|=2u;
    if(r5&8u)volume_keys&=~1u;
    if(r5&2u)volume_keys&=~2u;
    if(volume_keys!=before){
        if((volume_keys&1u)&&!(before&1u)&&up_presses<UINT16_MAX)++up_presses;
        if((volume_keys&2u)&&!(before&2u)&&down_presses<UINT16_MAX)++down_presses;
        volume_limited=false;volume_at=now+VOLUME_LIMIT_US;repeat_at=now+REPEAT_DELAY_US;
        if(volume_keys==1u)*volume=1;else if(volume_keys==2u)*volume=-1;
    }else if(volume_keys){
        /* A missing release must not ramp gain forever. Require an observed
         * release/new press after this bounded repeat interval. */
        if(due(now,volume_at))volume_limited=true;
        if(!volume_limited&&due(now,repeat_at)){
            if(volume_keys==1u)*volume=1;else if(volume_keys==2u)*volume=-1;
            repeat_at=now+REPEAT_US;
        }
    }
}
void ln_s5l_remote_diagnostics(uint32_t words[6]){
    words[0]=0x524d0000u|(present?1u:0u)|(armed?2u:0u)|(connected?4u:0u)|
        (fault?8u:0u)|((uint32_t)volume_keys<<4)|(center?64u:0u)|(volume_limited?128u:0u)|
        ((uint32_t)last_action<<8)|((uint32_t)clicks<<11);
    words[1]=raw;words[2]=samples;words[3]=failures;words[4]=center_presses;
    words[5]=up_presses|((uint32_t)down_presses<<16);
}
#endif
