#include "i2c.h"
#include "registers.h"
#define I2C(bus, offset) LN_REG(0x3c600000u + (bus)*0x300000u + (offset))
extern uint32_t ln_time_us(void);
static bool initialized;
typedef struct { unsigned bus; uint32_t deadline, budget; } transfer;
static bool alive(transfer *t) {
    if(!t->budget)return false;
    --t->budget;
    return (int32_t)(ln_time_us()-t->deadline)<0;
}
static bool ready(transfer *t) {
    while(I2C(t->bus,0x10)) if(!alive(t))return false;
    return alive(t);
}
static bool complete(transfer *t) {
    do {
        if(!ready(t))return false;
        if(!(I2C(t->bus,4)&0x20) || (I2C(t->bus,0x20)&0x2100)) {
            I2C(t->bus,0x20)|=0x2100;
            return true;
        }
    } while(alive(t));
    return false;
}
static bool start(transfer *t,uint8_t slave,bool read) {
    uint32_t mode=read?0x80:0xc0;
    if(!ready(t))return false;
    /* 12 MHz ECLK / 32 / 4 stays below the codec's 100 kHz limit. */
    I2C(t->bus,0)=0x83;
    if(!ready(t))return false;
    I2C(t->bus,4)=mode;
    if(!ready(t))return false;
    I2C(t->bus,12)=(uint32_t)slave*2u+read;
    if(!ready(t))return false;
    I2C(t->bus,4)=mode|0x30;
    return complete(t) && !(I2C(t->bus,4)&1);
}
static bool stop(transfer *t) {
    if(!ready(t))return false;
    I2C(t->bus,4)&=~0x20u;
    if(!ready(t))return false;
    I2C(t->bus,0)=0x10;
    return complete(t);
}
static bool send(transfer *t,uint8_t byte) {
    if(!ready(t))return false;
    I2C(t->bus,12)=byte;
    uint32_t until=ln_time_us()+5;
    while((int32_t)(ln_time_us()-until)<0)if(!alive(t))return false;
    if(!ready(t))return false;
    I2C(t->bus,0)=I2C(t->bus,0);
    return complete(t) && !(I2C(t->bus,4)&1);
}
static bool transact(unsigned bus,uint8_t slave,uint8_t reg,uint8_t *read_data,
                     const uint8_t *write_data,size_t length) {
    /* Bus 1 pin routing is not established for this board yet. */
    if(bus!=0 || slave>0x7f || !length || length>32 || (!read_data && !write_data))return false;
    LN_CLOCK_GATE1&=~((1u<<4)|(1u<<12));
    LN_REG(0x3cf00060u)=(LN_REG(0x3cf00060u)&~0xff0u)|0x220u;
    transfer t={bus,ln_time_us()+20000u,1000000u};
    if(!initialized) {
        if(!ready(&t))return false;
        I2C(bus,8)=0x40;
        if(!ready(&t))return false;
        I2C(bus,0x14)=1;
        if(!ready(&t))return false;
        I2C(bus,0x18)=0;
        if(!ready(&t))return false;
        I2C(bus,4)=0x80;
        if(!ready(&t))return false;
        I2C(bus,0)=0;
        if(!ready(&t))return false;
        I2C(bus,0x20)=0x3f00;initialized=true;
    }
    bool ok=start(&t,slave,false) && send(&t,reg);
    if(read_data) {
        ok=stop(&t)&&ok;
        if(ok)ok=start(&t,slave,true);
        for(size_t n=0;ok && n<length;n++) {
            ok=ready(&t);
            if(!ok)break;
            uint32_t control=I2C(bus,0);
            I2C(bus,0)=n+1<length?control|0x80u:control&~0x80u;
            ok=complete(&t);
            if(ok)read_data[n]=(uint8_t)I2C(bus,12);
        }
    } else for(size_t n=0;ok && n<length;n++)ok=send(&t,write_data[n]);
    /* Give cleanup its own short deadline even after the transfer expired. */
    t.deadline=ln_time_us()+1000u;t.budget=50000;
    bool stopped=stop(&t);
    if(ready(&t))I2C(bus,4)=0;
    LN_CLOCK_GATE1|=1u<<4;
    return ok && stopped;
}
bool ln_s5l_i2c_read(unsigned bus,uint8_t slave,uint8_t reg,uint8_t *data,size_t length) {
    if(!data)return false;
    return transact(bus,slave,reg,data,0,length);
}
bool ln_s5l_i2c_write(unsigned bus,uint8_t slave,uint8_t reg,const uint8_t *data,size_t length) {
    if(!data)return false;
    return transact(bus,slave,reg,0,data,length);
}
