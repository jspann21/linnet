#include "targets/classic6g-n25/flash.h"
#include "boot/include/linnet_bytes.h"
#include "boot/include/linnet_boot.h"
#include "boot/include/sha256.h"
#include "hal/api/usb.h"
/* Keep the archive inside the original boot region. The next NOR sector is
 * occupied on this board and is preserved by the installer. */
#define ARCHIVE 0x17000u
#define ARCHIVE_BYTES 0x11000u
static bool unpack(const uint8_t *source,uint32_t size,uint8_t *out,uint32_t capacity) {
    uint32_t input=0,used=0;
    while(input<size) {
        uint8_t token=source[input++];uint32_t count=token>>4;
        if(count==15)for(;;){if(input==size)return false;uint8_t n=source[input++];if(n>capacity-count)return false;count+=n;if(n!=255)break;}
        if(count>size-input || count>capacity-used)return false;
        for(uint32_t i=0;i<count;i++)out[used++]=source[input++];
        if(input==size)return used==capacity;
        if(size-input<2)return false;
        uint32_t offset=ln_read_le16(source+input);input+=2;count=token&15;
        if(count==15)for(;;){if(input==size)return false;uint8_t n=source[input++];if(n>capacity-count)return false;count+=n;if(n!=255)break;}
        if(!offset || offset>used || capacity-used<4 || count>capacity-used-4)return false;
        count+=4;for(uint32_t i=0;i<count;i++){out[used]=out[used-offset];used++;}
    }
    return false;
}
static void progress(void *context){(void)context;(void)ln_usb_poll();}
/* Only returns a fully bounded, hashed and LOS-validated Boot image. */
uint32_t ln_n25_persistent_load(uint8_t *out,struct ln_image_info *info,uint8_t sha[32]) {
    uint8_t header[64];
    if(!ln_n25_flash_read(ARCHIVE,header,sizeof(header)) || ln_read_le32(header)!=0x31424e4cu ||
       ln_read_le32(header+4)!=1 || ln_read_le32(header+60)!=ln_crc32(header,60))return 0;
    uint32_t packed=ln_read_le32(header+8),size=ln_read_le32(header+12);
    if(!packed || packed>ARCHIVE_BYTES-sizeof(header) || size<128 || size>0x80000)return 0;
    uint8_t *compressed=(uint8_t *)(uintptr_t)0x09100000u;
    for(uint32_t offset=0;offset<packed;) {
        uint32_t bytes=packed-offset;if(bytes>512)bytes=512;
        if(!ln_n25_flash_read(ARCHIVE+64+offset,compressed+offset,bytes))return 0;
        offset+=bytes;progress(0);
    }
    if(!unpack(compressed,packed,out,size))return 0;
    struct ln_image_policy policy={1238,7,0x08000000u,0x800000u};
    if(ln_image_validate_progress(out,size,&policy,info,progress,0)!=LINNET_IMAGE_OK)return 0;
    struct ln_sha256 hash;ln_sha256_init(&hash);
    for(uint32_t offset=0;offset<size;) {
        uint32_t bytes=size-offset;if(bytes>4096)bytes=4096;
        ln_sha256_update(&hash,out+offset,bytes);offset+=bytes;progress(0);
    }
    ln_sha256_finish(&hash,sha);
    for(unsigned i=0;i<32;i++)if(sha[i]!=header[16+i])return 0;
    return size;
}
