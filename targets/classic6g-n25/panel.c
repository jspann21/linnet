#include "panel.h"
#include "board.h"
#include "hal/s5l8702/lcd_bus.h"
#include "hal/s5l8702/i2c.h"
#include "hal/s5l8702/registers.h"
extern uint32_t ln_time_us(void);
static uint32_t phase,deadline,pixel,bar,bar_pixel,panel_type,setup_index;
static const uint16_t *frame;
static bool color_bars;
static bool send(uint8_t command,const uint8_t *data,unsigned count) {
    if(!ln_s5l_lcd_command(command))return false;
    for(unsigned i=0;i<count;i++)if(!ln_s5l_lcd_data(data[i]))return false;
    return true;
}
static bool pmu(uint8_t reg,uint8_t value) {
    return ln_s5l_i2c_write(0,0x73,reg,&value,1);
}
static bool reg16(uint16_t reg,uint16_t value) {
    return ln_s5l_lcd_command(reg) && ln_s5l_lcd_data(value);
}
uint32_t ln_n25_panel_type(void){return panel_type;}
static bool begin_window(uint32_t first,uint32_t end) {
    static const uint8_t columns[]={0,0,1,0x3f};
    uint8_t rows[]={(uint8_t)(first>>8),(uint8_t)first,(uint8_t)((end-1u)>>8),(uint8_t)(end-1u)};
    if(panel_type<2u){
        if(!ln_s5l_lcd_mode(0x80000c20u) || !send(0x2a,columns,4) ||
           !send(0x2b,rows,4) || !send(0x2c,0,0))return false;
    }else{
        if(!ln_s5l_lcd_mode(0x80000da8u) ||
           !reg16(0x210,0) || !reg16(0x211,319) ||
           !reg16(0x212,(uint16_t)first) || !reg16(0x213,(uint16_t)(end-1u)) ||
           !reg16(0x200,0) || !reg16(0x201,(uint16_t)first) || !ln_s5l_lcd_command(0x202))return false;
    }
    if(!ln_s5l_lcd_mode(0x80100db0u))return false;
    pixel=first*320u;phase=2;return true;
}
static bool begin_frame(void){return begin_window(0,240);}
static bool present_window(const uint16_t *pixels,uint32_t first,uint32_t end) {
    if(phase!=3 || !pixels)return false;
    frame=pixels;
    if(begin_window(first,end)){
#if defined(LINNET_IMAGE_OS)
        if(!ln_s5l_lcd_dma_begin(pixels+first*320u,320u*(end-first))){phase=0xffffffffu;return false;}
        phase=4;
#endif
        return true;
    }
    phase=0xffffffffu;return false;
}
bool ln_n25_panel_present(const uint16_t *pixels){return present_window(pixels,0,240);}
#if defined(LINNET_IMAGE_OS)
bool ln_n25_panel_present_rows(const uint16_t *pixels,uint32_t first,uint32_t end){
    if(first>=end||end>240u)return false;
    if(panel_type!=1u&&panel_type!=2u){first=0;end=240;}
    return present_window(pixels,first,end);
}
#endif
bool ln_n25_panel_start(bool diagnostic) {
    color_bars=diagnostic;
    phase=0;pixel=0;bar=0;bar_pixel=0;frame=0;setup_index=0;
    panel_type=(LN_REG(0x3cf000c4u)>>4)&3u;
    /* A warm restart can inherit an enabled backlight and stale panel RAM.
     * Hide reset/configuration until phase 2 finishes the full-screen clear.
     * A verified Boot-to-OS handoff uses adopt() and preserves its artwork. */
    if(!pmu(0x29,0) || !ln_n25_lcd_power())return false;
    LN_REG(0x38300020u)=0x33;
    if(panel_type>=2u){
        if(!ln_s5l_lcd_mode(0x80000da8u))return false;
        deadline=ln_time_us();phase=6;return true;
    }
    if(!ln_s5l_lcd_mode(0x80000c20u))return false;
    /* Establish the same controller state after ROM, OS and RAM handoffs.
     * A repeated Sleep Out alone does not reset an already awake panel. */
    if(!send(0x01,0,0))return false;
    deadline=ln_time_us()+120000u;phase=5;return true;
}
bool ln_n25_panel_adopt(void) {
    if((LN_CLOCK_GATE0&2u) || LN_REG(0x38300000u)!=0x80100db0u ||
       !(LN_REG(0x3830001cu)&2u))return false;
    panel_type=(LN_REG(0x3cf000c4u)>>4)&3u;
    phase=3;pixel=bar=bar_pixel=0;frame=0;color_bars=false;return true;
}
static bool configure(void) {
#include "third_party/rockbox_hw/classic_panel8.inc"
    deadline=ln_time_us()+60000u;phase=1;return true;
}
static bool configure16(void) {
#include "third_party/rockbox_hw/classic_panel16.inc"
    for(unsigned n=0;n<8 && setup_index<sizeof(setup)/sizeof(setup[0]);++n,++setup_index)
        if(!reg16(setup[setup_index][0],setup[setup_index][1]))return false;
    if(setup_index==sizeof(setup)/sizeof(setup[0])){deadline=ln_time_us()+30000u;phase=8;}
    return true;
}
uint32_t ln_n25_panel_service(uint32_t pixels) {
    if(pixels>512)pixels=512;
    if(phase==5){
        if((int32_t)(ln_time_us()-deadline)<0)return phase;
        if(panel_type==0u){
            if(!send(0x11,0,0))goto fail;
            deadline=ln_time_us()+30000u;phase=1;
        }else if(!configure())goto fail;
    }
    if(phase>=6 && phase<=10){
        if((int32_t)(ln_time_us()-deadline)<0)return phase;
        if(phase==6){
            /* Six spaced bus strobes also release an inherited standby. */
            if(!ln_s5l_lcd_command(0))goto fail;
            deadline=ln_time_us()+1280u;
            if(++setup_index==6u){setup_index=0;phase=7;}
        }else if(phase==7){if(!configure16())goto fail;}
        else if(phase==8){
            if(!reg16(0x030,0x03ff))goto fail;
            deadline=ln_time_us()+60000u;phase=9;
        }else if(phase==9){
            if(!reg16(0x007,0x0072))goto fail;
            deadline=ln_time_us()+150000u;phase=10;
        }else if(!reg16(0x007,0x0173) || !begin_frame())goto fail;
        return phase;
    }
#if defined(LINNET_IMAGE_OS)
    if(phase==4){int result=ln_s5l_lcd_dma_poll();if(result<0)goto fail;if(result>0)phase=3;return phase;}
#endif
    if(phase==1) {
        if((int32_t)(ln_time_us()-deadline)<0)return phase;
        /* Memory Access Control is only reliable after Sleep Out has settled.
         * Program it explicitly so cold NOR boot and warm RAM boot do not
         * inherit different scan directions from the previous firmware. */
        uint8_t orientation=0;
        if(panel_type==0u){
            uint8_t format=6;
            if(!send(0x35,&orientation,1) || !send(0x3a,&format,1))goto fail;
        }
        if(!send(0x36,&orientation,1) || !send(0x13,0,0) ||
           !send(0x29,0,0) || !begin_frame())goto fail;
    }
    if(phase==2) {
        static const uint16_t colors[]={0xffff,0xffe0,0x07ff,0x07e0,0xf81f,0xf800,0x001f,0};
        for(unsigned i=0;i<pixels && pixel<320u*240u;i++,pixel++) {
            if(!ln_s5l_lcd_data(frame?frame[pixel]:color_bars?colors[bar]:0x0841))goto fail;
            if(++bar_pixel==40){bar_pixel=0;bar=(bar+1)&7;}
        }
        if(pixel==320u*240u) {
            if(!pmu(0x2a,5) || !pmu(0x2b,0x14) || !pmu(0x28,0x20) || !pmu(0x29,1))goto fail;
            phase=3;
        }
    }
    return phase;
fail:phase=0xffffffffu;return phase;
}
uint32_t ln_n25_panel_poll(void){return ln_n25_panel_service(256);}
