#include "boot/include/linnet_bytes.h"
#include "jpeg.h"
#include "idct.h"
#include "linnet_target.h"
static const uint8_t zigzag[64]={0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
 12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,35,42,49,56,
 57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};
static uint32_t divide(uint32_t value,uint32_t divisor){
    uint32_t q=0,r=0;for(uint32_t i=32;i;i--){r=(r<<1)|((value>>(i-1u))&1u);if(r>=divisor){r-=divisor;q|=1u<<(i-1u);}}return q;
}
static int fail(struct ln_photo_jpeg*j,int error){j->error=error;return error;}
static uint32_t smaller(uint32_t a,uint32_t b){return a<b?a:b;}
static int tables(struct ln_photo_jpeg*j,const uint8_t*p,uint32_t n,bool huffman){
    /* At most four quant/eight Huffman definitions in a bounded marker step.
     * Repeated table definitions are outside this limited fallback contract. */
    if(n>(huffman?2184u:260u))return fail(j,LN_PHOTO_JPEG_UNSUPPORTED);
    while(n){
        uint32_t spec=*p++,id=spec&15u,kind=spec>>4;--n;
        if(id>=4)return fail(j,LN_PHOTO_JPEG_UNSUPPORTED);
        if(!huffman){
            if(kind||(j->quant_present&(1u<<id)))return fail(j,LN_PHOTO_JPEG_UNSUPPORTED);
            if(n<64)return fail(j,LN_PHOTO_JPEG_CORRUPT);
            for(uint32_t i=0;i<64;++i){if(!p[i])return fail(j,LN_PHOTO_JPEG_CORRUPT);j->quant[id][zigzag[i]]=p[i];}
            j->quant_present|=(uint8_t)(1u<<id);p+=64;n-=64;
        }else{
            if(kind>1||n<16)return fail(j,LN_PHOTO_JPEG_CORRUPT);
            struct ln_photo_jpeg_huffman*t=&j->huffman[kind][id];
            if(t->present)return fail(j,LN_PHOTO_JPEG_UNSUPPORTED);
            uint32_t code=0,total=0;
            for(uint32_t i=1;i<=16;++i){uint32_t count=p[i-1];
                t->first[i]=code;t->offset[i]=total;code+=count;t->end[i]=code;
                if(code>(1u<<i)||(count&&code==(1u<<i)))return fail(j,LN_PHOTO_JPEG_CORRUPT);
                total+=count;code<<=1;
            }
            p+=16;n-=16;if(!total||total>256||total>n)return fail(j,LN_PHOTO_JPEG_CORRUPT);
            for(uint32_t i=0;i<total;++i){uint32_t value=p[i];
                if((!kind&&value>11)||(kind&&((value&15u)>10||(!(value&15u)&&value!=0&&value!=240))))return fail(j,LN_PHOTO_JPEG_CORRUPT);
                t->symbols[i]=(uint8_t)value;
            }
            t->present=true;p+=total;n-=total;
        }
    }return 0;
}
static int layout(struct ln_photo_jpeg*j){
    uint32_t w=j->orientation>=5?j->height:j->width,h=j->orientation>=5?j->width:j->height;
    j->image_width=smaller(w,j->output_width);j->image_height=divide(h*j->image_width,w);
    if(j->image_height>j->output_height){j->image_height=j->output_height;j->image_width=divide(w*j->image_height,h);}
    if(!j->image_width)j->image_width=1;if(!j->image_height)j->image_height=1;
    j->left=(j->output_width-j->image_width)>>1;j->top=(j->output_height-j->image_height)>>1;
    j->mcu_columns=divide(j->width+j->max_h*8u-1u,j->max_h*8u);
    j->mcu_rows=divide(j->height+j->max_v*8u-1u,j->max_v*8u);
    j->phase=2;return 0;
}
static int marker_step(struct ln_photo_jpeg*j){
    if(++j->markers>4096)return fail(j,LN_PHOTO_JPEG_CORRUPT);
    uint32_t at=j->cursor;
    if(at>=j->bytes||j->source[at++]!=255)return fail(j,LN_PHOTO_JPEG_CORRUPT);
    /* One fill byte per call bounds even malicious runs of marker padding. */
    if(at<j->bytes&&j->source[at]==255){j->cursor=at;return 0;}
    if(at>=j->bytes)return fail(j,LN_PHOTO_JPEG_CORRUPT);
    uint32_t marker=j->source[at++];
    if(marker==0xc2||(marker>=0xc0&&marker<=0xcf&&marker!=0xc0&&marker!=0xc4))return fail(j,LN_PHOTO_JPEG_UNSUPPORTED);
    if(j->bytes-at<2)return fail(j,LN_PHOTO_JPEG_CORRUPT);
    uint32_t length=ln_read_be16(j->source+at);if(length<2||length>j->bytes-at)return fail(j,LN_PHOTO_JPEG_CORRUPT);
    const uint8_t*p=j->source+at+2;uint32_t n=length-2;j->cursor=at+length;
    if(marker==0xdb)return tables(j,p,n,false);
    if(marker==0xc4)return tables(j,p,n,true);
    if(marker==0xdd){if(n!=2)return fail(j,LN_PHOTO_JPEG_CORRUPT);j->restart_interval=ln_read_be16(p);return 0;}
    if(marker==0xc0){
        if(j->frame_seen||n<6)return fail(j,LN_PHOTO_JPEG_CORRUPT);
        if(p[0]!=8)return fail(j,LN_PHOTO_JPEG_UNSUPPORTED);
        j->height=ln_read_be16(p+1);j->width=ln_read_be16(p+3);j->components=p[5];
        if(!j->width||!j->height)return fail(j,LN_PHOTO_JPEG_CORRUPT);
        if(j->width>2048||j->height>2048||(j->components!=1&&j->components!=3))return fail(j,LN_PHOTO_JPEG_UNSUPPORTED);
        if(n!=6+j->components*3)return fail(j,LN_PHOTO_JPEG_CORRUPT);
        for(uint32_t i=0;i<j->components;++i){struct ln_photo_jpeg_component*c=&j->channels[i];
            c->id=p[6+i*3];c->h=p[7+i*3]>>4;c->v=p[7+i*3]&15u;c->quant=p[8+i*3];
            for(uint32_t k=0;k<i;++k)if(j->channels[k].id==c->id)return fail(j,LN_PHOTO_JPEG_CORRUPT);
            if(!c->h||c->h>2||!c->v||c->v>2||c->quant>=4||(i&&(c->h!=1||c->v!=1)))return fail(j,LN_PHOTO_JPEG_UNSUPPORTED);
        }
        if(j->components==1&&(j->channels[0].h!=1||j->channels[0].v!=1))return fail(j,LN_PHOTO_JPEG_UNSUPPORTED);
        if(j->components==3&&(j->channels[0].id!=1||j->channels[1].id!=2||j->channels[2].id!=3))return fail(j,LN_PHOTO_JPEG_UNSUPPORTED);
        j->max_h=j->channels[0].h;j->max_v=j->channels[0].v;j->frame_seen=true;return 0;
    }
    if(marker==0xda){
        if(!j->frame_seen||j->scan_seen||n!=4+2*j->components||p[0]!=j->components)return fail(j,LN_PHOTO_JPEG_CORRUPT);
        /* Restrict to one interleaved scan in frame order. Other scan layouts
         * are explicit unsupported cases, never decoded with guessed order. */
        for(uint32_t i=0;i<j->components;++i){struct ln_photo_jpeg_component*c=&j->channels[i];
            if(p[1+i*2]!=c->id)return fail(j,LN_PHOTO_JPEG_UNSUPPORTED);
            c->dc=p[2+i*2]>>4;c->ac=p[2+i*2]&15u;
            if(c->dc>=4||c->ac>=4||!(j->quant_present&(1u<<c->quant))||!j->huffman[0][c->dc].present||!j->huffman[1][c->ac].present)return fail(j,LN_PHOTO_JPEG_CORRUPT);
        }
        if(p[n-3]||p[n-2]!=63||p[n-1])return fail(j,LN_PHOTO_JPEG_UNSUPPORTED);
        if(j->components==3&&j->adobe_seen&&j->adobe_transform!=1)return fail(j,LN_PHOTO_JPEG_UNSUPPORTED);
        j->scan_seen=true;return layout(j);
    }
    if(marker==0xee&&n>=12&&p[0]=='A'&&p[1]=='d'&&p[2]=='o'&&p[3]=='b'&&p[4]=='e'){
        j->adobe_seen=true;j->adobe_transform=p[11];return 0;
    }
    if((marker>=0xe0&&marker<=0xef)||marker==0xfe)return 0;
    return fail(j,LN_PHOTO_JPEG_UNSUPPORTED);
}
static int bits(struct ln_photo_jpeg*j,uint32_t count){
    while(j->bit_count<count){
        if(j->cursor>=j->bytes)return fail(j,LN_PHOTO_JPEG_CORRUPT);
        uint32_t b=j->source[j->cursor++];
        if(b==255){if(j->cursor>=j->bytes||j->source[j->cursor++]!=0)return fail(j,LN_PHOTO_JPEG_CORRUPT);}
        j->bits=(j->bits<<8)|b;j->bit_count+=8;
    }
    j->bit_count-=count;return (int)((j->bits>>j->bit_count)&((1u<<count)-1u));
}
static int symbol(struct ln_photo_jpeg*j,const struct ln_photo_jpeg_huffman*t){
    uint32_t code=0;
    for(uint32_t i=1;i<=16;++i){int bit=bits(j,1);if(bit<0)return bit;code=(code<<1)|(uint32_t)bit;
        if(code>=t->first[i]&&code<t->end[i])return t->symbols[t->offset[i]+code-t->first[i]];
    }return fail(j,LN_PHOTO_JPEG_CORRUPT);
}
static int amplitude(struct ln_photo_jpeg*j,uint32_t n){
    int v=bits(j,n);if(j->error||!n)return v;
    return (uint32_t)v<(1u<<(n-1))?v-(int)((1u<<n)-1u):v;
}
static int block_step(struct ln_photo_jpeg*j){
    struct ln_photo_jpeg_component*c=&j->channels[j->component];
    int32_t *b=j->coefficients;for(uint32_t i=0;i<64;++i)b[i]=0;
    int n=symbol(j,&j->huffman[0][c->dc]);if(n<0)return n;
    int dc=amplitude(j,(uint32_t)n);if(j->error)return j->error;
    c->predictor+=dc;if(c->predictor < -2048||c->predictor>2047)return fail(j,LN_PHOTO_JPEG_CORRUPT);b[0]=c->predictor;
    for(uint32_t k=1;k<64;){
        int s=symbol(j,&j->huffman[1][c->ac]);if(s<0)return s;
        uint32_t run=(uint32_t)s>>4,size=(uint32_t)s&15u;
        if(!size){if(!run)break;k+=16;if(k>64)return fail(j,LN_PHOTO_JPEG_CORRUPT);continue;}
        k+=run;if(k>=64)return fail(j,LN_PHOTO_JPEG_CORRUPT);
        b[zigzag[k++]]=amplitude(j,size);if(j->error)return j->error;
    }
    ln_jpeg_idct8(j->block_pixels,b,j->quant[c->quant],8);
    uint32_t bx=(j->block&(c->h-1u))*8u,by=(j->block>>(c->h-1u))*8u,stride=c->h*8u;
    for(uint32_t y=0;y<8;++y)for(uint32_t x=0;x<8;++x)j->planes[j->component][(by+y)*stride+bx+x]=j->block_pixels[y*8+x];
    if(++j->block==c->h*c->v){j->block=0;if(++j->component==j->components){j->component=0;j->phase=4;}}
    return 0;
}
static void oriented_bounds(struct ln_photo_jpeg*j,uint32_t *x0,uint32_t *x1,uint32_t *y0,uint32_t *y1){
    uint32_t a=*x0,b=*x1,c=*y0,d=*y1,w=j->width,h=j->height;
    switch(j->orientation){
    case 2:*x0=w-b;*x1=w-a;break;
    case 3:*x0=w-b;*x1=w-a;*y0=h-d;*y1=h-c;break;
    case 4:*y0=h-d;*y1=h-c;break;
    case 5:*x0=c;*x1=d;*y0=a;*y1=b;break;
    case 6:*x0=h-d;*x1=h-c;*y0=a;*y1=b;break;
    case 7:*x0=h-d;*x1=h-c;*y0=w-b;*y1=w-a;break;
    case 8:*x0=c;*x1=d;*y0=w-b;*y1=w-a;break;
    default:break;
    }
}
static uint32_t clamp(int32_t x){return x<0?0:x>255?255:(uint32_t)x;}
static int render_row(struct ln_photo_jpeg*j){
    uint32_t w=j->orientation>=5?j->height:j->width,h=j->orientation>=5?j->width:j->height;
    uint32_t y=divide(j->render_y*h,j->image_height);
    /* Exact nearest-neighbor coordinates, with two divisions per row instead
     * of one per pixel. Sampling factors are restricted to one or two. */
    uint32_t x=divide(j->render_x0*w,j->image_width),step=divide(w,j->image_width);
    uint32_t remainder=j->render_x0*w-x*j->image_width,advance=w-step*j->image_width;
    for(uint32_t ox=j->render_x0;ox<j->render_x1;++ox){
        uint32_t sx=x,sy=y;
        switch(j->orientation){case 2:sx=j->width-1-x;break;case 3:sx=j->width-1-x;sy=j->height-1-y;break;
        case 4:sy=j->height-1-y;break;case 5:sx=y;sy=x;break;case 6:sx=y;sy=j->height-1-x;break;
        case 7:sx=j->width-1-y;sy=j->height-1-x;break;case 8:sx=j->width-1-y;sy=x;break;default:break;}
        sx-=j->mcu_x*j->max_h*8u;sy-=j->mcu_y*j->max_v*8u;
        if(sx>=j->max_h*8u||sy>=j->max_v*8u)return fail(j,LN_PHOTO_JPEG_CORRUPT);
        int32_t yy=j->planes[0][sy*j->max_h*8u+sx],u=0,v=0;
        if(j->components==3){uint32_t index=(sy>>(j->max_v-1u))*8u+(sx>>(j->max_h-1u));u=(int32_t)j->planes[1][index]-128;v=(int32_t)j->planes[2][index]-128;}
        uint32_t r=clamp(yy+((91881*v+32768)>>16)),g=clamp(yy-((22554*u+46802*v+32768)>>16)),b=clamp(yy+((116130*u+32768)>>16));
        j->surface[(j->top+j->render_y)*j->stride+j->left+ox]=(uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
        x+=step;remainder+=advance;if(remainder>=j->image_width){remainder-=j->image_width;++x;}
    }
    if(++j->render_y==j->render_y1)j->phase=6;return 0;
}
int ln_photo_jpeg_init(struct ln_photo_jpeg*j,const uint8_t*source,uint32_t bytes,uint16_t*surface,
    uint32_t width,uint32_t height,uint32_t stride,uint32_t orientation){
    if(!j)return LN_PHOTO_JPEG_ARGUMENT;
    uint8_t*p=(uint8_t*)j;for(uint32_t i=0;i<sizeof(*j);++i)p[i]=0;
    if(!source||!surface||!width||width>LINNET_PHOTO_WIDTH||!height||height>LINNET_PHOTO_HEIGHT||stride<width||stride>LINNET_PHOTO_WIDTH||orientation<1||orientation>8)return fail(j,LN_PHOTO_JPEG_ARGUMENT);
    if(bytes>LN_PHOTO_JPEG_MAX_BYTES)return fail(j,LN_PHOTO_JPEG_UNSUPPORTED);
    if(bytes<4||source[0]!=255||source[1]!=0xd8)return fail(j,LN_PHOTO_JPEG_CORRUPT);
    j->source=source;j->bytes=bytes;j->surface=surface;j->output_width=width;j->output_height=height;j->stride=stride;j->orientation=orientation;j->cursor=2;j->phase=1;return 0;
}
int ln_photo_jpeg_step(struct ln_photo_jpeg*j){
    if(!j)return LN_PHOTO_JPEG_ARGUMENT;if(j->error)return j->error;
    if(j->phase==1)return marker_step(j);
    if(j->phase==2){for(uint32_t x=0;x<j->output_width;++x)j->surface[j->clear_row*j->stride+x]=0;if(++j->clear_row==j->output_height)j->phase=3;return 0;}
    if(j->phase==3)return block_step(j);
    if(j->phase==4){
        uint32_t x0=j->mcu_x*j->max_h*8u,y0=j->mcu_y*j->max_v*8u,x1=smaller(j->width,x0+j->max_h*8u),y1=smaller(j->height,y0+j->max_v*8u);
        oriented_bounds(j,&x0,&x1,&y0,&y1);
        uint32_t w=j->orientation>=5?j->height:j->width,h=j->orientation>=5?j->width:j->height;
        j->render_x0=divide(x0*j->image_width+w-1u,w);j->render_x1=divide(x1*j->image_width+w-1u,w);
        j->render_y=divide(y0*j->image_height+h-1u,h);j->render_y1=divide(y1*j->image_height+h-1u,h);
        j->phase=j->render_y==j->render_y1||j->render_x0==j->render_x1?6:5;return 0;
    }
    if(j->phase==5)return render_row(j);
    if(j->phase==6){
        if(++j->mcu_x==j->mcu_columns){j->mcu_x=0;++j->mcu_y;}
        if(j->mcu_y==j->mcu_rows){j->phase=7;return 0;}
        if(j->restart_interval&&++j->restart_count==j->restart_interval){
            uint32_t pad=(1u<<j->bit_count)-1u;
            if((j->bits&pad)!=pad)return fail(j,LN_PHOTO_JPEG_CORRUPT);
            j->restart_count=0;j->bits=j->bit_count=0;
            if(j->bytes-j->cursor<2||j->source[j->cursor]!=255||j->source[j->cursor+1]!=(uint8_t)(0xd0u+j->restart_marker))return fail(j,LN_PHOTO_JPEG_CORRUPT);
            j->cursor+=2;j->restart_marker=(j->restart_marker+1u)&7u;for(uint32_t i=0;i<3;++i)j->channels[i].predictor=0;
        }j->phase=3;return 0;
    }
    if(j->phase==7){
        uint32_t pad=(1u<<j->bit_count)-1u;
        if((j->bits&pad)!=pad)return fail(j,LN_PHOTO_JPEG_CORRUPT);
        if(j->bytes-j->cursor<2||j->source[j->cursor]!=255||j->source[j->cursor+1]!=0xd9)return fail(j,LN_PHOTO_JPEG_CORRUPT);
        j->phase=8;return 1;
    }
    return j->phase==8?1:fail(j,LN_PHOTO_JPEG_ARGUMENT);
}
