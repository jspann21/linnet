/* BCM2722 tag-2 surface protocol, checked against the device's RetailOS.
 * This channel has its own sequence and reply storage. Each service turn
 * performs at most one bounded RPC transfer or one 4 KiB pixel upload. */
static struct {
    uint32_t slot,resource,address,element,sequence,step,mode,at,deadline;
    uint32_t front_resource,front_address;
    uint32_t reply[8];bool pending,enabled,visible;
} nd;
static int native_rpc(uint32_t opcode,const uint32_t *payload,uint32_t bytes){
    if(nd.slot>=8u)return -1;
    uint32_t *c=vc.channel[nd.slot];
    uint32_t offset=halfword(vc.directory,nd.slot*2u);
    if(!nd.pending){
        if(bytes>32u||(bytes&15u)||!ln_m25_bcm_read(vc.header[3]+offset+16u,c+4,4u))return -1;
        uint32_t lo=halfword(c,6),hi=halfword(c,8),rd=halfword(c,16),wr=halfword(c,32);
        if(!ring_valid(lo,hi,rd)||!ring_valid(lo,hi,wr))return -1;
        if(rd!=wr)return 0;
        nd.sequence=(nd.sequence+1u)&0x7fffffffu;
        uint32_t message[12]={0xf1a55a1fu,nd.sequence,opcode,bytes};
        for(uint32_t i=0;i<bytes/4u;++i)message[4u+i]=payload[i];
        if(!ring_transfer(true,lo,hi,&wr,message,16u+bytes))return -1;
        c[8]=(c[8]&0xffff0000u)|wr;
        if(!ln_m25_bcm_write(vc.header[3]+offset+32u,c+8,4u)||!notify_channel(nd.slot))return -1;
        nd.pending=true;return 0;
    }
    if(!ln_m25_bcm_read(vc.header[3]+offset+64u,c+16,4u))return -1;
    uint32_t lo=halfword(c,10),hi=halfword(c,12),rd=halfword(c,48),wr=halfword(c,64);
    if(!ring_valid(lo,hi,rd)||!ring_valid(lo,hi,wr))return -1;
    uint32_t available=wr>=rd?wr-rd:hi-rd+wr-lo;
    if(available<32u)return 0;
    if(available!=32u||!ring_transfer(false,lo,hi,&rd,nd.reply,32u))return -1;
    uint32_t length=nd.reply[3]&0xffffu;
    if(nd.reply[0]!=0xf1a55a1fu||nd.reply[1]!=nd.sequence||nd.reply[2]||
       length<(opcode==8u?8u:4u)||length>16u||(length&3u))return -1;
    if(nd.reply[4]==0xffffffffu)return -1;
    c[12]=(c[12]&0xffff0000u)|rd;
    if(!ln_m25_bcm_write(vc.header[3]+offset+48u,c+12,4u)||!notify_channel(nd.slot))return -1;
    nd.pending=false;return 1;
}
static bool native_display_begin(uint32_t mode){
    if(nd.mode||mode<1u||mode>4u)return false;
    nd.mode=mode;nd.step=mode==1u?1u:3u;nd.at=0;nd.deadline=ln_time_us()+2000000u;return true;
}
static int native_display_service(void){
    if(!nd.mode)return 1;
    if((int32_t)(ln_time_us()-nd.deadline)>=0)return -1;
    uint32_t payload[8]={0};int result=0;
    if(nd.step==1u||nd.step==8u){
        payload[0]=(uint32_t)(uintptr_t)&nd.resource;payload[1]=1u;
        payload[2]=320u;payload[3]=240u;payload[4]=640u;payload[7]=1u;
        result=native_rpc(8u,payload,32u);
        if(result>0){
            nd.resource=nd.reply[4];nd.address=nd.reply[5];
            /* The measured allocation is FE08E4C0. Retail's uploader at
             * 0x287BE8 sends this address unchanged through the host port.
             * Admit only the first 1 MiB of that qualified memory window;
             * native stream addresses use a different, low address range. */
            if(!nd.resource||(nd.address&3u)||nd.address<0xfe000000u||nd.address>0xfe100000u-153600u)return -1;
            if(nd.step==8u&&(nd.resource==nd.front_resource||
               (nd.address<nd.front_address+153600u&&nd.front_address<nd.address+153600u)))return -1;
            nd.step=nd.step==1u?2u:9u;
        }
    }else if(nd.step==2u||nd.step==9u){
        const uint16_t *pixels=ln_display_framebuffer();
        uint32_t bytes=153600u-nd.at;if(bytes>4096u)bytes=4096u;
        if(!pixels||!ln_m25_bcm_write(nd.address+nd.at,(const uint32_t *)((const uint8_t *)pixels+nd.at),bytes/4u))return -1;
        nd.at+=bytes;
        if(nd.at==153600u){
            if(nd.step==2u){
                nd.front_resource=nd.resource;nd.front_address=nd.address;
                nd.at=0;nd.step=8u;
            }else nd.step=3u;
        }
    }else if(nd.step==3u){
        result=native_rpc(1u,0,0);if(result>0)nd.step=nd.element?4u:nd.mode==2u?7u:5u;
    }else if(nd.step==4u){
        payload[0]=nd.element;result=native_rpc(4u,payload,16u);
        if(result>0){nd.element=0;nd.step=nd.mode==2u?7u:5u;}
    }else if(nd.step==5u){
        if(nd.mode==3u){nd.step=6u;return 0;}
        payload[1]=50u;payload[3]=nd.mode==4u?nd.front_resource:nd.resource;
        payload[6]=320u|(240u<<16);
        result=native_rpc(3u,payload,32u);
        if(result>0){nd.element=nd.reply[4];if(!nd.element)return -1;++nd.step;}
    }else if(nd.step==6u){
        result=native_rpc(2u,0,0);
        if(result>0){
            /* Only the acknowledged update makes the uploaded back surface
             * the front. Hide/show preserve the last presented surface. */
            if(nd.mode==1u||nd.mode==2u){
                uint32_t resource=nd.front_resource,address=nd.front_address;
                nd.front_resource=nd.resource;nd.front_address=nd.address;
                nd.resource=resource;nd.address=address;
            }
            nd.visible=nd.mode!=3u;nd.enabled=true;nd.mode=nd.step=0;return 1;
        }
    }else if(nd.step==7u)return 0;
    else return -1;
    return result<0?-1:0;
}
