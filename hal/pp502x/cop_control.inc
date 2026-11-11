/* Shared resident COP rendezvous; includer supplies mailbox and state. */
static bool cop_probe(void) {
    /* The nonce exchange rejects a stale capability word left by an older
     * monitor. Old resident instructions are never replaced by this image. */
    if (REG(0x11ffe0a0) != LINNET_DISPLAY_CAPABILITY) { cop_error=1;return false; }
    uint32_t nonce = MB(LN_DISPLAY_PROBE_ACK) + 1u, start = ln_time_us();
    MB(LN_DISPLAY_PROBE) = nonce;
    for (uint32_t budget = 0; budget < 100000u; ++budget) {
        if (MB(LN_DISPLAY_PROBE_ACK) == nonce) return true;
        if (ln_time_us() - start >= 5000u) break;
    }
    cop_error=2;return false;
}

static bool pause_worker(bool pause) {
    if (!cop_capable) return true;
    MB(LN_DISPLAY_PAUSE) = pause ? 1u : 0u;
    barrier();
    uint32_t start = ln_time_us();
    for (uint32_t budget = 0; budget < 100000u; ++budget) {
        if (MB(LN_DISPLAY_PAUSED) == (pause ? 1u : 0u)) return true;
        if (ln_time_us() - start >= 5000u) break;
    }
    return false;
}

bool ln_core_cop_pause(bool pause) {
    cop_error=0;
    if (!pause) {
        if (!clock_park_owned) return true; /* Never wake an inherited sleeper. */
        /* The CPU has finished the clock transaction before this call. */
        MB(LN_DISPLAY_PAUSE)=0;
        REG(0x11ffe010)=ln_time_us();
        barrier();
        REG(0x60007004)=0;
        uint32_t start=ln_time_us();
        for (uint32_t i=0;i<100000u;++i) {
            if (MB(LN_DISPLAY_PAUSED)==0) { clock_park_owned=false;return true; }
            if (ln_time_us()-start>=5000u) break;
        }
        cop_error=6;return false;
    }
    if (clock_park_owned) { cop_error=3;return false; }
    if (REG(0x60007004)&0x80000000u) return true;
    if (!cop_probe()) return false;
    /* Boot initializes ENABLED but may inherit an arbitrary STATE
     * word before the OS calls display_init. A proven, disabled worker is
     * paused first, then its dormant state is normalized before clock park.
     * This handshake works with the resident worker instructions. */
    if (MB(LN_DISPLAY_ENABLED)!=1u) {
        MB(LN_DISPLAY_PAUSE)=1;
        uint32_t prepare_start=ln_time_us();
        bool prepared=false;
        for (uint32_t i=0;i<100000u;++i) {
            if (MB(LN_DISPLAY_PAUSED)==1u) { prepared=true;break; }
            if (ln_time_us()-prepare_start>=5000u) break;
        }
        if (!prepared) { MB(LN_DISPLAY_PAUSE)=0;cop_error=4;return false; }
        MB(LN_DISPLAY_STATE)=0;
        barrier();
    }
    MB(LN_DISPLAY_PAUSE)=2;
    uint32_t start=ln_time_us();
    for (uint32_t i=0;i<5000000u;++i) {
        if (MB(LN_DISPLAY_PAUSED)==2 && (REG(0x60007004)&0x80000000u)) {
            clock_park_owned=true;return true;
        }
        if (ln_time_us()-start>=300000u) break;
    }
    /* Withdraw a failed request. If COP crossed the park point concurrently,
     * observe its actual sleep before waking only this acknowledged worker. */
    MB(LN_DISPLAY_PAUSE)=0;
    start=ln_time_us();
    for (uint32_t i=0;i<100000u;++i) {
        if (MB(LN_DISPLAY_PAUSED)==0) { cop_error=5;return false; }
        if (MB(LN_DISPLAY_PAUSED)==2 && (REG(0x60007004)&0x80000000u)) {
            clock_park_owned=true;(void)ln_core_cop_pause(false);cop_error=5;return false;
        }
        if (ln_time_us()-start>=5000u) break;
    }
    cop_error=7;return false;
}

uint32_t ln_display_cop_error(void) { return cop_error; }
static bool monitor_signature(uint32_t *branch,uint32_t *offset)
{
    /* Boot header copied from ln_cop_begin to 0x40001000. The retail-capable
     * header adds two instructions before the stable monitor sequence. Accept
     * only these exact layouts; never scan or patch an unknown resident COP. */
    if(REG(0x11ffe0a0u)!=LINNET_DISPLAY_CAPABILITY||REG(0x11ffe018u)!=0x4c4e434fu)
        return false;
    *offset=REG(0x40001040u)==0xe5923000u?0u:
        REG(0x40001048u)==0xe5923000u?8u:UINT32_MAX;
    if(*offset==UINT32_MAX)return false;
    uint32_t base=*offset;
    if(REG(0x40001044u+base)!=0xe580301cu|| /* publish COP tick */
       REG(0x40001054u+base)!=0xe5904010u|| /* ldr r4,[r0,#16]: CPU progress */
       REG(0x40001058u+base)!=0xe0434004u|| /* sub r4,r3,r4 */
       REG(0x4000105cu+base)!=0xe59f504cu|| /* load750ms literal */
       REG(0x40001060u+base)!=0xe1540005u|| /* cmp elapsed,deadline */
       REG(0x400010b0u+base)!=750000u||REG(0x400010b8u+base)!=1500000u)return false;
    *branch=REG(0x40001064u+base);
    return *branch==0x3afffff4u||*branch==0xbafffff4u;
}
bool ln_display_upgrade_monitor(void)
{
    uint32_t branch,offset;
    if(!monitor_signature(&branch,&offset)){cop_error=8;return false;}
    if(branch==0xbafffff4u)return true;
    /* A monitor we did not park must not be modified or woken by this call. */
    if(REG(0x60007004u)&0x80000000u){cop_error=8;return false;}
    if(!ln_core_cop_pause(true))return false;
    bool patched=false;
    if(MB(LN_DISPLAY_PAUSED)==2u&&(REG(0x60007004u)&0x80000000u)&&
       monitor_signature(&branch,&offset)){
        /* Signed LT also rejects a future progress sample after timer wrap.
         * Normal watchdog deadlines are well inside the signed half-range. */
        REG(0x40001064u+offset)=(branch&0x0fffffffu)|0xb0000000u;
        barrier();
        patched=REG(0x40001064u+offset)==0xbafffff4u;
    }
    bool resumed=ln_core_cop_pause(false);
    if(!patched)cop_error=9;
    return patched&&resumed;
}
void ln_display_cop_diagnostics(uint32_t output[8]) {
    if (!output) return;
    output[0]=cop_error;output[1]=REG(0x11ffe0a0);output[2]=REG(0x60007004);
    output[3]=MB(LN_DISPLAY_ENABLED);output[4]=MB(LN_DISPLAY_STATE);
    output[5]=MB(LN_DISPLAY_PAUSE);output[6]=MB(LN_DISPLAY_PAUSED);
    output[7]=MB(LN_DISPLAY_PROBE_ACK);
}

