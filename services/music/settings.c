#include "boot/include/linnet_bytes.h"
#include "settings.h"
#include "boot/include/linnet_boot.h"
/* MST3 stores the stable track ID in place of the profile's fixed header. */
uint32_t ln_music_settings_encode(const struct ln_queue*q,const struct ln_music_profile*p,uint8_t*out,uint32_t cap){
    if(!q||!p||!out||cap<44)return 0;
    uint32_t n=ln_queue_encode(q,out+8,cap-44);if(!n)return 0;
    uint8_t profile[32];if(!ln_music_profile_encode(p,profile))return 0;
    for(uint32_t i=0;i<24;i++)out[8+n+i]=profile[8+i];
    ln_write_le32(out,0x3354534du);ln_write_le32(out+4,n);ln_write_le64(out+32+n,q->last_track_id);
    ln_write_le32(out+40+n,ln_crc32(out,40+n));return 44+n;
}
bool ln_music_settings_decode(struct ln_queue*q,struct ln_music_profile*p,const uint8_t*in,uint32_t bytes){
    if(!q||!p||!in)return false;struct ln_music_profile profile;
    if(bytes<44)return false;
    if(ln_read_le32(in)!=0x3354534du)return false;
    uint32_t n=ln_read_le32(in+4);if(n!=bytes-44||ln_read_le32(in+40+n)!=ln_crc32(in,40+n))return false;
    uint8_t record[32];ln_write_le32(record,0x464f5250u);ln_write_le32(record+4,0x00200002u);
    for(uint32_t i=0;i<24;i++)record[8+i]=in[8+n+i];
    if(!ln_music_profile_decode(&profile,record,32)||!ln_queue_decode(q,in+8,n))return false;
    q->last_track_id=ln_read_le64(in+32+n);
    *p=profile;return true;
}
