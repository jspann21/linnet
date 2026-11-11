#include "hal/api/usb.h"
#include "hal/api/power.h"
#include "hal/s5l8702/usb.h"
#include "hal/s5l8702/registers.h"
#include "boot/include/linnet_bytes.h"
#include "hal/arm926/cache.h"

extern uint32_t ln_time_us(void);
extern void ln_memory_copy(void *to,const void *from,uint32_t size);
#define ENABLE (1u<<31)
#define SNAK (1u<<27)
#define CNAK (1u<<26)
#define ACTIVE (1u<<15)
#define STALL (1u<<21)
#define DATA0 (1u<<28)
/* S5L8702 exposes IN endpoints 1/3/5 and OUT endpoints 2/4/6. */
#ifdef LINNET_SRAM_MONITOR
#define USB_MEMORY __attribute__((section(".dram.usb"),aligned(32)))
#else
#define USB_MEMORY __attribute__((aligned(32)))
#endif
static uint8_t control[256] USB_MEMORY;
static uint8_t setup_data[64] USB_MEMORY;
#ifdef LINNET_SRAM_MONITOR
static uint8_t receive[4096] USB_MEMORY;
#else
static uint8_t receive[16384] USB_MEMORY;
#endif
static uint8_t transmit[16384] USB_MEMORY;
static uint8_t line_coding[7]={0x00,0xc2,0x01,0,0,0,8};
static uint8_t serial_string[66];
static uint16_t packet_size,rx_size,rx_offset,control_size,control_offset,line_state;
static uint16_t storage_receive_size,rx_transfer_size;
static uint8_t configuration,control_action;
static uint32_t generation,failure,stop_proof,tx_started,control_started;
static bool running,tx_active,tx_zlp,rx_active;
static bool storage_mode,storage_reset;
static bool control_pending,control_read;
static bool suspended,configured_power;
static uint16_t charge_budget;
static uint32_t previous_trace[32];
/* Live diagnostics only; retention across a BootROM reset is not guaranteed. */
static volatile uint32_t *const trace=(volatile uint32_t *)(uintptr_t)0x22017f00u;
static const uint8_t device[]={18,1,0,2,2,0,0,64,9,0x12,0x4a,0x50,0,1,1,2,3,1};
static const uint8_t configuration_descriptor[]={
    9,2,67,0,2,1,0,0x80,250,
    9,4,0,0,1,2,2,1,0,
    5,0x24,0,0x10,1,5,0x24,1,0,1,4,0x24,2,2,5,0x24,6,0,1,
    7,5,0x81,3,8,0,16,9,4,1,0,2,0x0a,0,0,0,
    7,5,0x83,2,0,2,0,7,5,0x02,2,0,2,0};
static const uint8_t language[]={4,3,9,4};
static const uint8_t storage_descriptor[]={9,2,32,0,1,1,0,0x80,250,
    9,4,0,0,2,8,6,0x50,0,7,5,0x83,2,0,2,0,7,5,2,2,0,2,0};
static const uint8_t manufacturer[]={14,3,'L',0,'i',0,'n',0,'n',0,'e',0,'t',0};
static const uint8_t product[]={24,3,'L',0,'i',0,'n',0,'n',0,'e',0,'t',0,' ',0,'L',0,'i',0,'n',0,'k',0};
static const uint8_t qualifier[]={10,6,0,2,2,0,0,64,1,0};

static void copy(uint8_t *to,const uint8_t *from,uint32_t size){
    if(size>=32u&&!(((uintptr_t)to|(uintptr_t)from)&3u))ln_memory_copy(to,from,size);
    else while(size--)*to++=*from++;
}
static void barrier(void){uint32_t zero=0;__asm__ volatile("mcr p15,0,%0,c7,c10,4"::"r"(zero):"memory");}
static void charge_limit(uint16_t budget){
    if(charge_budget!=budget&&ln_power_charge_limit(budget))charge_budget=budget;
}
static bool wait_bits(volatile uint32_t *reg,uint32_t mask,uint32_t value){
    uint32_t start=ln_time_us();
    for(uint32_t budget=2000000u;budget;--budget){
        if((*reg&mask)==value)return true;
        if(ln_time_us()-start>=100000u)break;
    }
    return false;
}
static void delay(uint32_t us){uint32_t start=ln_time_us();for(uint32_t n=0;n<4000000u&&ln_time_us()-start<us;n++)__asm__ volatile("nop");}
static void arm_control_mode(bool accept_data){
    ln_arm926_data_clean_range(setup_data,sizeof(setup_data));
    (void)ln_arm926_data_invalidate_range(setup_data,sizeof(setup_data));
    LN_DWC_OUT(0,0x14)=(uint32_t)(uintptr_t)setup_data;
    LN_DWC_OUT(0,0x10)=(1u<<29)|(1u<<19)|64u;
    barrier();LN_DWC_OUT(0,0)|=ENABLE|(accept_data?CNAK:SNAK);
}
static void arm_control(void){arm_control_mode(false);}
static void send_control_packet(void){
    uint32_t count=control_size-control_offset;if(count>64)count=64;
    ln_arm926_data_clean_range(control+control_offset,count);
    LN_DWC_IN(0,0x14)=(uint32_t)(uintptr_t)(control+control_offset);
    LN_DWC_IN(0,0x10)=(1u<<19)|count;
    control_offset+=(uint16_t)count;
    barrier();LN_DWC_IN(0,0)|=ENABLE|CNAK;
}
static void reply_control(const uint8_t *bytes,uint16_t count,uint16_t requested){
    if(count>requested)count=requested;
    if(bytes!=control&&count)copy(control,bytes,count);
    control_size=count;control_offset=0;send_control_packet();
}
static void arm_rx(void){
    if(!configuration||rx_active||rx_size||storage_reset||(storage_mode&&!storage_receive_size))return;
    rx_transfer_size=storage_mode?storage_receive_size:packet_size;
    ln_arm926_data_clean_range(receive,rx_transfer_size);
    (void)ln_arm926_data_invalidate_range(receive,rx_transfer_size);
    LN_DWC_OUT(2,0x14)=(uint32_t)(uintptr_t)receive;
    LN_DWC_OUT(2,0x10)=((uint32_t)(rx_transfer_size>>(packet_size==512?9:6))<<19)|rx_transfer_size;
    barrier();LN_DWC_OUT(2,0)|=ENABLE|CNAK;rx_active=true;
}
static void configure(void){
    LN_DWC_IN(1,0)=8u|ACTIVE|(3u<<18)|(1u<<22)|DATA0|SNAK;
    LN_DWC_IN(3,0)=packet_size|ACTIVE|(2u<<18)|(2u<<22)|DATA0|SNAK;
    LN_DWC_OUT(2,0)=packet_size|ACTIVE|(2u<<18)|DATA0|SNAK;
    rx_size=rx_offset=storage_receive_size=0;rx_active=false;arm_rx();
}
static void reset(void){
    ++generation;configuration=0;line_state=0;rx_active=tx_active=tx_zlp=false;rx_size=rx_offset=storage_receive_size=0;
    configured_power=suspended=false;charge_limit(100);
    control_action=0;control_size=control_offset=0;
    control_pending=false;
    storage_reset=false;
    LN_DWC(0x800)&=~0x7f0u;
    for(uint32_t ep=0;ep<7;ep++){LN_DWC_IN(ep,8)=UINT32_MAX;LN_DWC_OUT(ep,8)=UINT32_MAX;}
    LN_DWC_IN(0,0)=ACTIVE|SNAK;LN_DWC_OUT(0,0)=ACTIVE|SNAK;
    arm_control();
}
static void setup(void){
    suspended=false;
    uint8_t type=setup_data[0],request=setup_data[1];
    uint16_t value=ln_read_le16(setup_data+2),index=ln_read_le16(setup_data+4),size=ln_read_le16(setup_data+6);
    control_started=ln_time_us();control_pending=true;control_read=(type&0x80)!=0;
    /* SETUP cancels an older control transfer. Do not reuse its IN DMA/FIFO
     * while the controller still owns it. All waits are bounded. */
    if(LN_DWC_IN(0,0)&ENABLE) {
        LN_DWC_IN(0,0)|=SNAK;
        if(!wait_bits(&LN_DWC_IN(0,0),1u<<17,1u<<17)){failure=4;return;}
        LN_DWC_IN(0,0)|=1u<<30;
        if(!wait_bits(&LN_DWC_IN(0,0),ENABLE,0)){failure=4;return;}
        LN_DWC(0x10)=1u<<5;
        if(!wait_bits(&LN_DWC(0x10),1u<<5,0)){failure=4;return;}
    }
    trace[2]++;trace[3]=ln_read_le32(setup_data);trace[4]=ln_read_le32(setup_data+4);
    control_action=0;LN_DWC_IN(0,0)&=~STALL;LN_DWC_OUT(0,0)&=~STALL;
    arm_control();
    if(type==0x80&&request==6){
        const uint8_t *bytes=0;uint16_t count=0;
        switch(value>>8){
        case 1:
            bytes=device;count=sizeof(device);
            if(storage_mode){copy(control,device,count);control[4]=0;control[10]=0x4b;bytes=control;}
            break;
        case 2:case 7:
            if(storage_mode) {
                copy(control,storage_descriptor,sizeof(storage_descriptor));control[1]=(uint8_t)(value>>8);
                uint16_t p=(value>>8)==7?(packet_size==512?64:512):packet_size;
                ln_write_le16(control+22,p);ln_write_le16(control+29,p);
                bytes=control;count=sizeof(storage_descriptor);break;
            }
            copy(control,configuration_descriptor,sizeof(configuration_descriptor));
            control[1]=(uint8_t)(value>>8);
            {uint16_t p=(value>>8)==7?(packet_size==512?64:512):packet_size;
            ln_write_le16(control+57,p);ln_write_le16(control+64,p);}
            bytes=control;count=sizeof(configuration_descriptor);break;
        case 3:
            switch(value&255){case 0:bytes=language;break;case 1:bytes=manufacturer;break;case 2:bytes=product;break;case 3:bytes=serial_string;break;}
            if(bytes)count=bytes[0];break;
        case 6:bytes=qualifier;count=sizeof(qualifier);
            if(storage_mode){copy(control,qualifier,count);control[4]=0;bytes=control;}break;
        }
        if(bytes){reply_control(bytes,count,size);return;}
    }else if(type==0&&request==5&&value<128&&!size){
        LN_DWC(0x800)=(LN_DWC(0x800)&~0x7f0u)|((uint32_t)value<<4);
        reply_control(0,0,0);return;
    }else if(type==0&&request==9&&value<=1&&!size){
        if(!value){configured_power=false;charge_limit(100);}
        configuration=(uint8_t)value;control_action=2;reply_control(0,0,0);return;
    }else if(type==0x80&&request==8){reply_control(&configuration,1,size);return;
    }else if((type==0x80||type==0x81||type==0x82)&&request==0){
        control[0]=control[1]=0;
        if(type==0x82 && (index==0x83 || index==2))control[0]=((index==0x83?LN_DWC_IN(3,0):LN_DWC_OUT(2,0))&STALL)?1:0;
        reply_control(control,2,size);return;
    }else if(storage_mode&&type==2&&request==1&&!value&&!size&&(index==0x83||index==2)) {
        if(index==0x83)LN_DWC_IN(3,0)=(LN_DWC_IN(3,0)&~STALL)|DATA0;
        else LN_DWC_OUT(2,0)=(LN_DWC_OUT(2,0)&~STALL)|DATA0;
        reply_control(0,0,0);return;
    }else if(storage_mode&&type==0xa1&&request==0xfe&&!value&&!index&&size==1) {
        control[0]=0;reply_control(control,1,1);return;
    }else if(storage_mode&&type==0x21&&request==0xff&&!value&&!index&&!size) {
        /* The disk owner drains and flushes ATA before completing status. */
        storage_reset=true;return;
    }else if(!storage_mode&&type==0x21&&request==0x22&&index==0&&!size){
        if((line_state&1)&&!(value&1)){++generation;rx_size=rx_offset=0;}
        line_state=value;reply_control(0,0,0);return;
    }else if(!storage_mode&&type==0xa1&&request==0x21&&index==0){reply_control(line_coding,7,size);return;
    }else if(!storage_mode&&type==0x21&&request==0x20&&index==0&&size==7){control_action=3;arm_control_mode(true);return;
    }else if(type==1&&request==11&&value==0&&!size){reply_control(0,0,0);return;
    }else if(type==0x81&&request==10){control[0]=0;reply_control(control,1,size);return;
    }
    control_pending=false;LN_DWC_IN(0,0)|=STALL;LN_DWC_OUT(0,0)|=STALL;arm_control();
}
static bool initialize(const char *serial){
    charge_budget=UINT16_MAX;charge_limit(100);
    for(uint32_t n=0;n<32;n++)trace[n]=0;
    trace[0]=0x30425355u;trace[1]=1;
    uint32_t length=0;while(serial[length]&&length<32)length++;
    if(!length||serial[length])return false;
    serial_string[0]=(uint8_t)(length*2+2);serial_string[1]=3;
    for(uint32_t n=0;n<length;n++){serial_string[2+n*2]=(uint8_t)serial[n];serial_string[3+n*2]=0;}
    LN_CLOCK_GATE0&=~4u;LN_CLOCK_GATE1&=~8u;
    LN_PHY(0)=0;delay(10);LN_PHY(0x1c)=1;LN_PHY(0x44)=0xe3f;
    LN_PHY(8)=1;delay(10);LN_PHY(8)=0;delay(10);LN_PHY(0x18)=0x600;LN_PHY(4)=0;delay(400);
    LN_DWC(0xe00)=0;LN_DWC(0x804)=2;LN_DWC(0xc)=8u|(5u<<10);
    delay(100);
    if(!wait_bits(&LN_DWC(0x10),1u<<31,1u<<31))return false;
    LN_DWC(0x10)=1;if(!wait_bits(&LN_DWC(0x10),1,0))return false;
    trace[1]=2;
    if(!wait_bits(&LN_DWC(0x10),1u<<31,1u<<31))return false;
    LN_DWC(0x24)=0x360;LN_DWC(0x28)=(0x40u<<16)|0x360;
    for(uint32_t n=0;n<3;n++)LN_DWC(0x104+n*4)=(0x180u<<16)|(0x3a0+n*0x180);
    LN_DWC(0x10)=(1u<<4)|(1u<<5)|(16u<<6);
    if(!wait_bits(&LN_DWC(0x10),0x30,0))return false;
    LN_DWC(0xc)=8u|(5u<<10);
    LN_DWC(8)=0x2bu;LN_DWC(0x800)=4;LN_DWC(0x810)=9;LN_DWC(0x814)=9;
    LN_DWC(0x830)=(1u<<27)|(8u<<17)|(1u<<16);
    LN_DWC(0x81c)=(1u<<0)|(1u<<1)|(1u<<3)|(1u<<16)|(1u<<18);
    LN_DWC(0x18)=(1u<<12)|(1u<<13)|(1u<<18)|(1u<<19);
    LN_DWC(0x14)=UINT32_MAX;packet_size=512;failure=0;running=true;reset();
    LN_DWC(0x804)=2;delay(10000);LN_DWC(0x804)=(1u<<8)|(1u<<10);trace[1]=3;return true;
}
bool ln_usb_init(const char *serial){storage_mode=false;return initialize(serial);}
bool ln_s5l_usb_storage_init(const char *serial){storage_mode=true;return initialize(serial);}
bool ln_usb_poll(void){
    if(!running||failure)return false;
    uint32_t status=LN_DWC(0x14);
    trace[5]=status;trace[6]=LN_DWC(0x808);trace[7]=LN_DWC(0x804);
    trace[8]=LN_DWC_IN(0,0);trace[9]=LN_DWC_IN(0,8);trace[10]=LN_DWC_IN(0,0x10);
    trace[11]=LN_DWC_OUT(0,0);trace[12]=LN_DWC_OUT(0,8);trace[13]=LN_DWC_OUT(0,0x10);
    trace[14]=ln_time_us();trace[15]=generation;trace[16]=LN_DWC(0x48);trace[17]=LN_DWC(0x50);
    trace[18]=control_action;trace[19]=control_pending;trace[20]=control_read;trace[21]=control_started;
    trace[22]=line_state;trace[23]=configuration;trace[24]=failure;
    trace[25]=rx_active;trace[26]=rx_size;trace[27]=tx_active;trace[28]=tx_started;
    trace[29]=LN_DWC_OUT(2,0);trace[30]=LN_DWC_OUT(2,8);trace[31]=LN_DWC_IN(3,8);
    if(status&(1u<<12)){LN_DWC(0x14)=1u<<12;reset();}
    if(status&(1u<<31)){LN_DWC(0x14)=1u<<31;suspended=false;}
    if(status&((1u<<11)|(1u<<29))){
        LN_DWC(0x14)=status&((1u<<11)|(1u<<29));suspended=true;
    }
    if(status&(1u<<13)){packet_size=((LN_DWC(0x808)>>1)&3u)==0?512:64;LN_DWC(0x14)=1u<<13;arm_control();}
    /* Retire the previous IN completion before SETUP replaces its state.
     * Reading it after setup() can mistake the preceding status ACK for
     * completion of the new CDC request and discard its pending OUT data. */
    uint32_t in=LN_DWC_IN(0,8);LN_DWC_IN(0,8)=in;
    uint32_t out=LN_DWC_OUT(0,8);LN_DWC_OUT(0,8)=out;
    if(out&9u)(void)ln_arm926_data_invalidate_range(setup_data,sizeof(setup_data));
    if(out&8u)setup();
    if((out&1u) && (out&(1u<<5))) {
        /* StatusRecvd is a controller-generated completion, not another
         * CDC data packet. It still disables EP0 OUT, which must be armed. */
        arm_control_mode(control_action==3);
    } else if((out&1u) && !(out&((1u<<15)|8u))){
        if(control_action==3){
            if((LN_DWC_OUT(0,0x10)&0x7fu)!=57u) {
                control_pending=false;control_action=0;LN_DWC_IN(0,0)|=STALL;arm_control();
            } else {
                copy(line_coding,setup_data,7);control_action=0;reply_control(0,0,0);
            }
        }else {if(control_read)control_pending=false;arm_control();}
    }
    if((in&1u) && !(out&8u)){
        if(control_offset<control_size)send_control_packet();
        else{
            if(control_action==2){configured_power=configuration!=0;if(configuration)configure();}
            if(!control_read)control_pending=false;
            control_action=0;arm_control_mode(true);
        }
    }
    out=LN_DWC_OUT(2,8);LN_DWC_OUT(2,8)=out;
    if(out&1u){
        (void)ln_arm926_data_invalidate_range(receive,rx_transfer_size);
        rx_active=false;rx_size=(uint16_t)(rx_transfer_size-(LN_DWC_OUT(2,0x10)&0x7ffffu));rx_offset=0;
        storage_receive_size=0;
    }
    in=LN_DWC_IN(3,8);LN_DWC_IN(3,8)=in;
    if(in&1u){
        if(tx_zlp){tx_zlp=false;LN_DWC_IN(3,0x10)=1u<<19;barrier();LN_DWC_IN(3,0)|=ENABLE|CNAK;}
        else tx_active=false;
    }
    if(tx_active&&ln_time_us()-tx_started>2000000u)failure=2;
    if(control_pending&&ln_time_us()-control_started>2000000u)failure=3;
    charge_limit(suspended||failure?0:configured_power?500:100);
    arm_rx();return !failure;
}
bool ln_usb_connected(void){return running&&configuration&&(storage_mode||(line_state&1))&&!failure;}
bool ln_usb_rx_pending(void){return rx_size>rx_offset;}
void ln_usb_rx_hint(uint32_t remaining){(void)remaining;} /* Already uses negotiated 512-byte packets. */
uint32_t ln_usb_generation(void){return generation;}
void ln_usb_irq_enable(void){} /* The executive services the bounded USB poller. */
size_t ln_usb_read(uint8_t *data,size_t size){
    if(!ln_usb_connected())return 0;
    size_t available=rx_size-rx_offset;if(size>available)size=available;
    copy(data,receive+rx_offset,(uint32_t)size);rx_offset+=(uint16_t)size;
    if(rx_offset==rx_size){rx_size=rx_offset=0;arm_rx();}return size;
}
size_t ln_s5l_usb_storage_read(uint8_t *data,size_t size){
    if(!storage_mode||storage_reset||!size||!ln_usb_connected())return 0;
    if(!rx_active&&!rx_size){
        size_t limit=packet_size==512u?sizeof(receive):4096u;
        if(size>limit)size=limit;
        storage_receive_size=(uint16_t)((size+packet_size-1u)&~(size_t)(packet_size-1u));
        arm_rx();
    }
    return ln_usb_read(data,size);
}
size_t ln_usb_write(const uint8_t *data,size_t size){
    if(!ln_usb_connected()||tx_active||!size)return 0;if(size>packet_size)size=packet_size;
    copy(transmit,data,(uint32_t)size);LN_DWC_IN(3,0x14)=(uint32_t)(uintptr_t)transmit;
    ln_arm926_data_clean_range(transmit,(uint32_t)size);
    LN_DWC_IN(3,0x10)=(1u<<19)|(uint32_t)size;barrier();
    tx_active=true;tx_zlp=size==packet_size;tx_started=ln_time_us();LN_DWC_IN(3,0)|=ENABLE|CNAK;return size;
}
bool ln_s5l_usb_storage_reset_pending(void){return storage_reset;}
bool ln_s5l_usb_storage_reset_complete(void) {
    if(!storage_reset)return false;
    if(LN_DWC_IN(3,0)&ENABLE) {
        LN_DWC_IN(3,0)|=SNAK;
        if(!wait_bits(&LN_DWC_IN(3,0),1u<<17,1u<<17))return false;
        LN_DWC_IN(3,0)|=1u<<30;
        if(!wait_bits(&LN_DWC_IN(3,0),ENABLE,0))return false;
    }
    if(LN_DWC_OUT(2,0)&ENABLE) {
        LN_DWC(0x804)|=1u<<9;
        if(!wait_bits(&LN_DWC(0x14),1u<<7,1u<<7))return false;
        LN_DWC_OUT(2,0)|=(1u<<30)|SNAK;
        if(!wait_bits(&LN_DWC_OUT(2,0),ENABLE,0))return false;
        LN_DWC(0x804)|=1u<<10;
    }
    LN_DWC(0x10)=(2u<<6)|(1u<<5);
    if(!wait_bits(&LN_DWC(0x10),1u<<5,0))return false;
    LN_DWC(0x10)=1u<<4;
    if(!wait_bits(&LN_DWC(0x10),1u<<4,0))return false;
    LN_DWC_IN(3,8)=UINT32_MAX;LN_DWC_OUT(2,8)=UINT32_MAX;
    rx_active=tx_active=tx_zlp=false;rx_size=rx_offset=storage_receive_size=0;
    /* BOT reset preserves halt/toggle state. CLEAR_FEATURE resets DATA0. */
    storage_reset=false;reply_control(0,0,0);arm_rx();return true;
}
bool ln_s5l_usb_storage_write(const uint8_t *data,size_t size) {
    if(!storage_mode || storage_reset || !ln_usb_connected() || tx_active || size>sizeof(transmit) || (size&&!data))return false;
    if(size)copy(transmit,data,(uint32_t)size);
    ln_arm926_data_clean_range(transmit,(uint32_t)size);
    LN_DWC_IN(3,0x14)=(uint32_t)(uintptr_t)transmit;
    uint32_t packets=size?(uint32_t)((size+packet_size-1u)>>(packet_size==512?9:6)):1u;
    LN_DWC_IN(3,0x10)=(packets<<19)|(uint32_t)size;barrier();
    tx_active=true;tx_zlp=false;tx_started=ln_time_us();LN_DWC_IN(3,0)|=ENABLE|CNAK;return true;
}
void ln_s5l_usb_stall(bool input) {
    if(input)LN_DWC_IN(3,0)|=STALL;else LN_DWC_OUT(2,0)|=STALL;
}
bool ln_usb_flush(uint32_t deadline){
    for(uint32_t budget=20000000u;budget;--budget){
        if(!ln_usb_poll())return false;if(!tx_active)return true;
        if((int32_t)(ln_time_us()-deadline)>=0)return false;
    }return false;
}
bool ln_usb_stop(void){
    charge_limit(100);
    if(trace[0]==0x30425355u)for(unsigned i=0;i<32;i++)previous_trace[i]=trace[i];
    previous_trace[24]=failure;
    LN_DWC(0x804)|=2;running=false;stop_proof=0;
    if(!wait_bits(&LN_DWC(0x10),1u<<31,1u<<31))return false;
    LN_DWC(0x10)=1;if(!wait_bits(&LN_DWC(0x10),1,0))return false;
    /* Core reset clears DCTL. Keep the pull-up disconnected long enough for
     * the host to retire its old CDC instance before a RAM image reconnects. */
    LN_DWC(0x804)|=2;delay(100000);
    stop_proof=0x2f;return true;
}
uint32_t ln_usb_stop_diagnostics(void){return stop_proof;}
void ln_usb_disconnect(void){(void)ln_usb_stop();}
void ln_usb_diagnostics(uint32_t out[8]){
    out[0]=LN_DWC_IN(3,0);out[1]=LN_DWC_IN(3,0x10);out[2]=LN_DWC_OUT(2,0);
    out[3]=LN_DWC(0x14);out[4]=LN_DWC(0x808);out[5]=LN_DWC(0x818);out[6]=failure;out[7]=generation;
}
void ln_s5l_usb_trace(uint32_t output[64]){
    for(unsigned i=0;i<32;i++){output[i]=trace[i];output[32+i]=previous_trace[i];}
}
