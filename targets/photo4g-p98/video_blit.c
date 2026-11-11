#include "targets/photo4g-p98/lcd.h"
#include "hal/api/platform.h"
#include "hal/api/display.h"
#include "hal/pp502x/display_internal.h"
#include "hal/api/core.h"
#include <stdint.h>
/* Exact 1392-byte resident worker from preserved Boot
 * 5dc70355df8d2097cd79923da3bf767af5244bd7e874d4fa315e738131cf58a0.
 * Source: out/native-boot-sha/boot.los, file [0x300,0x870).
 * SHA256:81ee702bc4f72ca399c1390ae7f302353c0f0b6dcae1b5aa7ba35dd44b06a7fa.
 * Only the signed watchdog branch and two explicit helper hooks may differ.
 * No pattern search or live-worker relocation. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define RESIDENT 0x40001000u
#define HELPER 0x40002800u
#define HELPER_LIMIT 0x40002c00u
#define COP_STACK_TOP 0x40003000u
#define HOOK 0x178u
#define OLD_BRANCH 0x0a00008eu
#define NEW_BRANCH 0x0a0005a0u
#define COMMAND_HOOK 0x168u
#define OLD_COMMAND_BRANCH 0x0a00004fu
extern const uint32_t ln_video_blit_begin[],ln_video_blit_end[],ln_video_command_begin[];
static uint32_t command_branch(void){
    uint32_t target=HELPER+(uint32_t)((uintptr_t)ln_video_command_begin-(uintptr_t)ln_video_blit_begin);
    return 0x0a000000u|((target-(RESIDENT+COMMAND_HOOK+8u))>>2);
}
static const uint32_t original[348]={
    0xe59fd0b8u,0xe59f00b8u,0xe3a01000u,0xe5801010u,0xe5801000u,0xe5801014u,
    0xe5801018u,0xe59f0080u,0xe3a01000u,0xe5801014u,0xe59f1078u,0xe5801018u,
    0xe59f1090u,0xe58010a0u,0xe59f206cu,0xeb000022u,0xe5923000u,0xe580301cu,
    0xe5904014u,0xe3540001u,0x1afffff9u,0xe5904010u,0xe0434004u,0xe59f504cu,
    0xe1540005u,0xbafffff4u,0xe59f5044u,0xe3a06002u,0xe5856000u,0xe59f503cu,
    0xe1540005u,0x3affffeeu,0xe59f5034u,0xe5907098u,0xe3570000u,0x03a070c0u,
    0x05807098u,0xe5956000u,0xe3866004u,0xe5856000u,0xeafffffeu,0x11ffe000u,
    0x4c4e434fu,0x60005010u,0x000b71b0u,0x60004018u,0x0016e360u,0x60006004u,
    0x40003000u,0x11ffd000u,0x3244434cu,0xe92d40ffu,0xe51f0014u,0xe5901078u,
    0xe580107cu,0xe5901014u,0xe3510002u,0x0a000028u,0xe3510000u,0x13a01001u,
    0xe5801018u,0x1a000110u,0xe5901010u,0xe3510001u,0x1a00010du,0xe5901000u,
    0xe59f2430u,0xe1510002u,0x1a000109u,0xe5901070u,0xe2811001u,0xe5801070u,
    0xe590101cu,0xe3510000u,0x0a000028u,0xe59f2410u,0xe5922000u,0xe5903024u,
    0xe0422003u,0xe59f3404u,0xe1520003u,0x23a01001u,0x2a0000f1u,0xe590206cu,
    0xe3520000u,0x03a01002u,0x0a0000edu,0xe2422001u,0xe580206cu,0xe3510001u,
    0x0a00004fu,0xe3510002u,0x0a000077u,0xe3510003u,0x0a00008eu,0xe3510004u,
    0x0a0000cdu,0xe3a01005u,0xea0000e1u,0xe590101cu,0xe3510000u,0x1affffd7u,
    0xe5901014u,0xe3510002u,0x1a0000e5u,0xe3a01002u,0xe5801018u,0xe59f23b8u,
    0xe3a01102u,0xe5821000u,0xe1a00000u,0xe1a00000u,0xe1a00000u,0xe3a01000u,
    0xe5801018u,0xea0000dau,0xe5901008u,0xe590200cu,0xe0513002u,0x0a0000d6u,
    0xe3530008u,0x83a01003u,0x8a0000c9u,0xe2022007u,0xe0802202u,0xe2822080u,
    0xe8920078u,0xe35300dcu,0x2a000027u,0xe35400b0u,0x2a000025u,0xe3550000u,
    0x0a000023u,0xe3560000u,0x0a000021u,0xe26370dcu,0xe1550007u,0x8a00001eu,
    0xe26470b0u,0xe1560007u,0x8a00001bu,0xe1837005u,0xe3170001u,0x1a000018u,
    0xe5907020u,0xe3170003u,0x1a000015u,0xe3570201u,0x3a000013u,0xe59f12f4u,
    0xe1570001u,0x8a000010u,0xe5803058u,0xe580405cu,0xe5805060u,0xe5806064u,
    0xe5806050u,0xe3a01000u,0xe5801040u,0xe5801044u,0xe5801048u,0xe59f12c0u,
    0xe580106cu,0xe59f12b0u,0xe5911000u,0xe5801024u,0xe3a01001u,0xe580101cu,
    0xea0000a5u,0xe3a01004u,0xea000099u,0xe59f22a0u,0xe5923000u,0xe3130102u,
    0x1a00009fu,0xe5901040u,0xe3510000u,0x03a03012u,0x0a000019u,0xe3510001u,
    0x0590305cu,0x0a000016u,0xe3510002u,0x03a03013u,0x0a000013u,0xe3510003u,
    0x05903058u,0x026330dbu,0x0a00000fu,0xe3510004u,0x03a03015u,0x0a00000cu,
    0xe3510005u,0x0590305cu,0x05904064u,0x00833004u,0x02433001u,0x0a000006u,
    0xe3510006u,0x03a03016u,0x0a000003u,0xe5903058u,0xe5904060u,0xe0833004u,
    0xe26330dcu,0xe3833102u,0xe5823000u,0xe2811001u,0xe5801040u,0xe3510008u,
    0x03a01002u,0x0580101cu,0xea000079u,0xe59f21f8u,0xe5923000u,0xe3130102u,
    0x1a000075u,0xe5902050u,0xe3520080u,0x83a02080u,0xe5802054u,0xe5903060u,
    0xe0040392u,0xe1a030a4u,0xe580304cu,0xe1a04084u,0xe2444001u,0xe59f21c4u,
    0xe59f31c8u,0xe5823000u,0xe59f31c4u,0xe1844003u,0xe5824004u,0xe3a0330du,
    0xe5823000u,0xe3a01003u,0xe580101cu,0xea000060u,0xe5905060u,0xe5906044u,
    0xe0455006u,0xe1a050a5u,0xe3550020u,0x83a05020u,0xe590704cu,0xe1550007u,
    0x81a05007u,0xe590706cu,0xe2877001u,0xe1550007u,0x81a05007u,0xe3550000u,
    0x0a000030u,0xe590105cu,0xe5907048u,0xe0811007u,0xe3a070dcu,0xe0020791u,
    0xe5907058u,0xe0822007u,0xe0822006u,0xe5901020u,0xe0811082u,0xe59f2134u,
    0xe59f3134u,0xe59f413cu,0xe3a0e000u,0xe5926000u,0xe3160401u,0x0a000008u,
    0xe4916004u,0xe1a07426u,0xe0077004u,0xe0066004u,0xe1876406u,0xe5836000u,
    0xe28ee001u,0xe2555001u,0x1afffff3u,0xe35e0000u,0x0a000014u,0xe5901044u,
    0xe081108eu,0xe5902060u,0xe1510002u,0x03a01000u,0x05902048u,0x02822001u,
    0x05802048u,0xe5801044u,0xe5901028u,0xe081100eu,0xe5801028u,0xe590106cu,
    0xe2811001u,0xe041100eu,0xe580106cu,0xe590104cu,0xe051100eu,0xe580104cu,
    0x03a01004u,0x0580101cu,0xea00001fu,0xe59f2094u,0xe5923000u,0xe3130301u,
    0x0a00001bu,0xe3a03000u,0xe5823004u,0xe5902050u,0xe5903054u,0xe0522003u,
    0xe5802050u,0x13a01002u,0x1580101cu,0x1a000012u,0xe3a01000u,0xe580101cu,
    0xe590100cu,0xe2811001u,0xe580100cu,0xe590102cu,0xe2811001u,0xe580102cu,
    0xea000009u,0xe5801034u,0xe5901030u,0xe2811001u,0xe5801030u,0xe59f102cu,
    0xe3a02000u,0xe5812004u,0xe580201cu,0xe5901008u,0xe580100cu,0xe8bd80ffu,
    0x31505344u,0x60005010u,0x0007a120u,0x000f4240u,0x107ed180u,0x70008a0cu,
    0x70008a20u,0x70008b00u,0x10000080u,0xc0010000u,0x00ff00ffu,0x60007004u,
};
_Static_assert(RESIDENT+sizeof original<=HELPER,"Resident code overlaps helper");
_Static_assert(HELPER_LIMIT+1024u<=COP_STACK_TOP,"COP stack gap");
static void barrier(void){__asm__ volatile("" ::: "memory");}
static bool fingerprint(uint32_t *branch){
    if(REG(0x11ffe0a0u)!=LINNET_DISPLAY_CAPABILITY||REG(0x11ffe018u)!=0x4c4e434fu)return false;
    for(uint32_t i=0;i<sizeof original/4u;i++){
        uint32_t word=REG(RESIDENT+i*4u);
        if(i==0x64u/4u){if(word!=0x3afffff4u&&word!=0xbafffff4u)return false;}
        else if(i==COMMAND_HOOK/4u){if(word!=OLD_COMMAND_BRANCH&&word!=command_branch())return false;}
        else if(i==HOOK/4u){if(word!=OLD_BRANCH&&word!=NEW_BRANCH)return false;*branch=word;}
        else if(word!=original[i])return false;
    }
    return true;
}
static bool helper_matches(uint32_t words){for(uint32_t i=0;i<words;i++)if(REG(HELPER+i*4u)!=ln_video_blit_begin[i])return false;return true;}
bool ln_display_upgrade_blit(void){
    if(ln_lcd_panel_type()&1u)return false;
    uintptr_t begin=(uintptr_t)ln_video_blit_begin,end=(uintptr_t)ln_video_blit_end;
    if(end<=begin||((begin|end)&3u)||end-begin>HELPER_LIMIT-HELPER)return false;
    uint32_t words=(uint32_t)(end-begin)/4u,branch=0;
    if(!fingerprint(&branch))return false;
    /* An already installed helper must match exactly; never overwrite a
     * possibly executing unknown payload, even if its hook is recognized. */
    if(branch==NEW_BRANCH)return REG(RESIDENT+COMMAND_HOOK)==command_branch()&&helper_matches(words);
    if(REG(0x60007004u)&0x80000000u)return false;
    if(!ln_core_cop_pause(true))return false;
    bool installed=false;
    if(REG(LINNET_DISPLAY_MAILBOX+LN_DISPLAY_PAUSED)==2u&&
       REG(LINNET_DISPLAY_MAILBOX+LN_DISPLAY_STATE)==0u&&
       (REG(0x60007004u)&0x80000000u)&&fingerprint(&branch)&&branch==OLD_BRANCH){
        for(uint32_t i=0;i<words;i++)REG(HELPER+i*4u)=ln_video_blit_begin[i];
        barrier();
        if(helper_matches(words)){
            /* The old worker stays parked at its untouched PC. On wake it
             * restores its original monitor LR; only later state-1/state-3 dispatches
             * enter the copied, entirely self-contained burst routine. */
            REG(RESIDENT+COMMAND_HOOK)=command_branch();REG(RESIDENT+HOOK)=NEW_BRANCH;barrier();
            installed=REG(RESIDENT+HOOK)==NEW_BRANCH&&REG(RESIDENT+COMMAND_HOOK)==command_branch();
            if(!installed){
                REG(RESIDENT+HOOK)=OLD_BRANCH;REG(RESIDENT+COMMAND_HOOK)=OLD_COMMAND_BRANCH;barrier();
                /* A failed instruction readback must never wake a worker
                 * with an unknown branch. Retain the owned park on failure. */
                if(REG(RESIDENT+HOOK)!=OLD_BRANCH||REG(RESIDENT+COMMAND_HOOK)!=OLD_COMMAND_BRANCH)return false;
            }
        }
    }
    bool resumed=ln_core_cop_pause(false);
    return installed&&resumed;
}

#if !defined(LINNET_PROFILE_RELEASE)
void ln_display_blit_diagnostics(uint32_t out[16]){
    out[0]=0x54494c42u;out[1]=0u;
    out[2]=(uint32_t)((uintptr_t)ln_video_blit_end-(uintptr_t)ln_video_blit_begin);
    out[3]=REG(RESIDENT+HOOK);out[4]=REG(RESIDENT+COMMAND_HOOK);
    out[5]=command_branch();out[6]=0xffffffffu;out[7]=out[8]=0;
    for(uint32_t i=0;i<sizeof original/4u;++i){
        uint32_t v=REG(RESIDENT+i*4u);
        if(i==0x64u/4u||i==HOOK/4u||i==COMMAND_HOOK/4u)continue;
        if(v!=original[i]){out[6]=i*4u;out[7]=original[i];out[8]=v;break;}
    }
    out[9]=ln_display_column_mismatches();
    out[10]=REG(LINNET_DISPLAY_MAILBOX+LN_DISPLAY_HEAD);
    out[11]=REG(LINNET_DISPLAY_MAILBOX+LN_DISPLAY_TAIL);
    out[12]=REG(LINNET_DISPLAY_MAILBOX+LN_DISPLAY_FAILURES);
    out[13]=out[14]=out[15]=0;
}
#endif
