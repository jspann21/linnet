#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "services/video/ipvf/ima_lookup.h"
#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
static const uint16_t steps[89]={
    7,8,9,10,11,12,13,14,16,17,19,21,
    23,25,28,31,34,37,41,45,50,55,60,66,
    73,80,88,97,107,118,130,143,157,173,190,209,
    230,253,279,307,337,371,408,449,494,544,598,658,
    724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,
    2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,
    7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,
    22385,24623,27086,29794,32767
};
static int32_t sample(const uint8_t *p){
    unsigned value=(unsigned)p[0]|((unsigned)p[1]<<8);
    return value&32768u?(int)value-65536:(int)value;
}
static uint32_t nibble(int32_t target,int32_t *predictor,uint32_t *index){
    int32_t difference=target-*predictor;
    uint32_t code=0,step=steps[*index];
    if(difference<0){code=8;difference=-difference;}
    if((unsigned)difference>=step){code|=4;difference-=(int)step;}
    if((unsigned)difference>=(step>>1)){code|=2;difference-=(int)(step>>1);}
    if((unsigned)difference>=(step>>2))code|=1;
    uint32_t entry=ima_lookup[(*index<<3)|(code&7u)];
    int32_t delta=(int32_t)(entry>>8);
    int32_t value=*predictor+(code&8u?-delta:delta);
    *predictor=value>32767?32767:value< -32768?-32768:value;
    *index=entry&255u;
    return code;
}
/* PCM and output are disjoint. Caller owns both buffers for the entire call.
 * state receives next channel indices and silence/mono/stereo (0/1/2). */
EXPORT int32_t ln_ipvf_encode_audio(const uint8_t *pcm,uint32_t input_bytes,uint32_t frames,
    uint32_t left_index,uint32_t right_index,uint8_t *out,uint32_t capacity,uint32_t state[3]){
    if(!pcm||!out||!state||!frames||frames>UINT32_MAX/4u||
       input_bytes!=frames*4u||left_index>88u||right_index>88u||capacity<frames+7u)return -1;
    uint32_t nonzero=0,mono=1;
    for(uint32_t n=0;n<frames;++n){const uint8_t *p=pcm+n*4u;
        nonzero|=(unsigned)p[0]|p[1]|p[2]|p[3];
        mono&=(p[0]==p[2]&&p[1]==p[3]);}
    state[0]=left_index;state[1]=right_index;state[2]=0;
    if(!nonzero)return 0;
    int32_t left=sample(pcm),right=sample(pcm+2);
    out[0]=pcm[0];out[1]=pcm[1];out[2]=(uint8_t)left_index;out[3]=0;
    if(mono){
        for(uint32_t n=1;n<frames;++n){
            unsigned code=nibble(sample(pcm+n*4u),&left,&left_index);
            if(n&1u)out[4u+(n-1u)/2u]=(uint8_t)code;
            else out[4u+(n-1u)/2u]|=(uint8_t)(code<<4);
        }
        state[0]=state[1]=left_index;state[2]=1;
        return (int)(4u+frames/2u);
    }
    out[4]=pcm[2];out[5]=pcm[3];out[6]=(uint8_t)right_index;out[7]=0;
    for(uint32_t n=1;n<frames;++n){
        unsigned a=nibble(sample(pcm+n*4u),&left,&left_index);
        unsigned b=nibble(sample(pcm+n*4u+2u),&right,&right_index);
        out[7u+n]=(uint8_t)(a|(b<<4));
    }
    state[0]=left_index;state[1]=right_index;state[2]=2;
    return (int)(frames+7u);
}

struct output { uint8_t *p; uint32_t used,cap; };
static int byte(struct output *o,uint32_t v){if(o->used==o->cap)return 0;o->p[o->used++]=(uint8_t)v;return 1;}
static int copy(struct output *o,const uint8_t *p,uint32_t n){if(n>o->cap-o->used)return 0;memcpy(o->p+o->used,p,n);o->used+=n;return 1;}
static int length(struct output *o,uint32_t n){if(n<15)return 1;n-=15;while(n>=255){if(!byte(o,255))return 0;n-=255;}return byte(o,n);}
EXPORT int32_t ln_ipvf_coalesce(const uint8_t *packed,uint32_t packed_size,const uint8_t *raw,uint32_t raw_size,uint32_t minimum,uint8_t *out,uint32_t capacity){
 if(!packed||!raw||!out||!packed_size||!raw_size||raw_size>262144||packed_size>524288||minimum<4||minimum>262145||capacity>2147483647)return -1;
 uint32_t cursor=0,position=0,anchor=0;struct output o={out,0,capacity};
 while(cursor<packed_size){
  uint32_t token=packed[cursor++],literals=token>>4,match=(token&15)+4;
  if(literals>raw_size-position)return -1;
  if(literals==15){uint32_t ext;do{if(cursor==packed_size)return -1;ext=packed[cursor++];if(ext>raw_size-position-literals)return -1;literals+=ext;}while(ext==255);}
  if(literals>packed_size-cursor||literals>raw_size-position)return -1;
  cursor+=literals;position+=literals;if(cursor==packed_size)break;
  if(packed_size-cursor<2)return -1;uint32_t match_at=cursor,offset=packed[cursor]|((uint32_t)packed[cursor+1]<<8);cursor+=2;if(!offset||offset>position)return -1;
  if(match>raw_size-position)return -1;
  if((token&15)==15){uint32_t ext;do{if(cursor==packed_size)return -1;ext=packed[cursor++];if(ext>raw_size-position-match)return -1;match+=ext;}while(ext==255);}
  if(match>raw_size-position)return -1;
  if(match>=minimum){literals=position-anchor;if(!byte(&o,((literals<15?literals:15)<<4)|(token&15))||!length(&o,literals)||!copy(&o,raw+anchor,literals)||!copy(&o,packed+match_at,cursor-match_at))return -1;anchor=position+match;}
  position+=match;
 }
 if(position!=raw_size)return -1;
 uint32_t literals=position-anchor;if(!byte(&o,(literals<15?literals:15)<<4)||!length(&o,literals)||!copy(&o,raw+anchor,literals))return -1;return (int32_t)o.used;
}


struct score {uint32_t total,count;};
static struct score score(const uint8_t *prev,const uint8_t *cur,int w,int h,int dx,int dy,int step,const struct score *ceiling){
 int x0=dx>0?dx:0,x1=dx<0?w+dx:w,y0=dy>0?dy:0,y1=dy<0?h+dy:h;
 struct score s={0,(uint32_t)(((x1-x0-1)/step+1)*((y1-y0-1)/step+1))};
 for(int y=y0;y<y1;y+=step){
  for(int x=x0;x<x1;x+=step){
   uint32_t a=(uint32_t)(y*w+x)*2,b=(uint32_t)((y-dy)*w+x-dx)*2;
   int lo=(int)cur[a]-prev[b],hi=(int)cur[a+1]-prev[b+1];s.total+=(uint32_t)((lo<0?-lo:lo)+(hi<0?-hi:hi));
  }
  if(ceiling&&(uint64_t)s.total*ceiling->count>(uint64_t)ceiling->total*s.count)break;
 }
 return s;
}
static int consider(const uint8_t *prev,const uint8_t *cur,int w,int h,int dx,int dy,int step,int *bx,int *by,struct score *best){
 struct score s=score(prev,cur,w,h,dx,dy,step,best);uint64_t left=(uint64_t)s.total*best->count,right=(uint64_t)best->total*s.count;
 int distance=(dx<0?-dx:dx)+(dy<0?-dy:dy),old=(*bx<0?-*bx:*bx)+(*by<0?-*by:*by);
 if(left<right||(left==right&&(distance<old||(distance==old&&(dx<*bx||(dx==*bx&&dy<*by)))))){*best=s;*bx=dx;*by=dy;return 1;}return 0;
}
EXPORT int32_t ln_ipvf_estimate_translation(const uint8_t *prev,const uint8_t *cur,uint32_t bytes,uint32_t w,uint32_t h,uint32_t shift,uint32_t step,int32_t state[2]){
 if(!prev||!cur||!state||!w||!h||w>1024||h>1024||w*h>131072||bytes!=w*h*2||shift>32||shift>=w||shift>=h||!step||step>1024)return -1;
 int bx=0,by=0;struct score best=score(prev,cur,(int)w,(int)h,0,0,(int)step,0);
 for(int dy=-(int)shift;dy<=(int)shift;dy+=2)for(int dx=-(int)shift;dx<=(int)shift;dx+=2)consider(prev,cur,(int)w,(int)h,dx,dy,(int)step,&bx,&by,&best);
 int x0=bx-2,x1=bx+2,y0=by-2,y1=by+2;if(x0<-(int)shift)x0=-(int)shift;if(y0<-(int)shift)y0=-(int)shift;if(x1>(int)shift)x1=(int)shift;if(y1>(int)shift)y1=(int)shift;
 for(int dy=y0;dy<=y1;++dy)for(int dx=x0;dx<=x1;++dx)consider(prev,cur,(int)w,(int)h,dx,dy,(int)step,&bx,&by,&best);
 state[0]=bx;state[1]=by;return 0;
}



static int rect_geometry(uint32_t bytes,uint32_t w,uint32_t h){return w&&h&&w<=1024&&h<=1024&&w*h<=131072&&bytes==w*h*2;}
static int rect_bounds(const uint8_t *prev,const uint8_t *cur,uint32_t w,uint32_t h,uint32_t out[4]){
 uint32_t minx=w,miny=h,maxx=0,maxy=0;
 for(uint32_t y=0;y<h;++y){
  const uint8_t *a=prev+y*w*2,*b=cur+y*w*2;if(!memcmp(a,b,w*2))continue;
  uint32_t first=0,last=w-1;
  while(first<w&&a[first*2]==b[first*2]&&a[first*2+1]==b[first*2+1])++first;
  while(last>first&&a[last*2]==b[last*2]&&a[last*2+1]==b[last*2+1])--last;
  if(first<minx)minx=first;if(last>maxx)maxx=last;if(y<miny)miny=y;maxy=y;
 }
 if(miny==h)return 0;minx&=~1u;maxx|=1u;if(maxx>=w)maxx=w-1;
 out[0]=minx;out[1]=miny;out[2]=maxx-minx+1;out[3]=maxy-miny+1;return 1;
}
EXPORT int32_t ln_ipvf_frame_bounds(const uint8_t *prev,const uint8_t *cur,uint32_t bytes,uint32_t w,uint32_t h,uint32_t out[4]){
 if(!prev||!cur||!out||!rect_geometry(bytes,w,h))return -1;return rect_bounds(prev,cur,w,h,out);
}
struct rect {uint32_t x,y,w,h;};
static int pair_equal(const uint8_t *a,const uint8_t *b,uint32_t offset){return !memcmp(a+offset,b+offset,4);}
static struct rect unite(struct rect a,struct rect b){
 uint32_t x=a.x<b.x?a.x:b.x,y=a.y<b.y?a.y:b.y,right=a.x+a.w>b.x+b.w?a.x+a.w:b.x+b.w,bottom=a.y+a.h>b.y+b.h?a.y+a.h:b.y+b.h;
 return (struct rect){x,y,right-x,bottom-y};
}
static int32_t cost(struct rect a){return (int32_t)(8+a.w*a.h*2);}
EXPORT int32_t ln_ipvf_rectangles(const uint8_t *prev,const uint8_t *cur,uint32_t bytes,uint32_t w,uint32_t h,uint32_t maximum,uint32_t pairs,uint32_t rows,uint32_t *output,uint32_t capacity){
 if(!prev||!cur||!output||!rect_geometry(bytes,w,h)||!maximum||maximum>255||!pairs||pairs>1024||!rows||rows>1024||capacity<128)return -1;
 uint32_t columns=w/2;if(!columns)return 0;
 uint32_t nx=(columns+pairs-1)/pairs,ny=(h+rows-1)/rows,cells=nx*ny;
 uint8_t *grid=calloc(cells,1);uint32_t *stack=malloc(cells*sizeof(*stack));int32_t result=-2;
 if(!grid||!stack)goto done;
 uint32_t changed=0,min_tx=nx,max_tx=0,min_ty=ny,max_ty=0;
 for(uint32_t ty=0;ty<ny;++ty)for(uint32_t tx=0;tx<nx;++tx){
  uint32_t first=tx*pairs,end=first+pairs,y=ty*rows,yend=y+rows;if(end>columns)end=columns;if(yend>h)yend=h;
  for(;y<yend;++y){uint32_t offset=(y*w+first*2)*2;if(memcmp(prev+offset,cur+offset,(end-first)*4)){grid[ty*nx+tx]=1;++changed;if(tx<min_tx)min_tx=tx;if(tx>max_tx)max_tx=tx;if(ty<min_ty)min_ty=ty;if(ty>max_ty)max_ty=ty;break;}}
 }
 result=0;if(!changed)goto done;
 if(!(w&1u)&&changed==(max_tx-min_tx+1)*(max_ty-min_ty+1)){result=rect_bounds(prev,cur,w,h,output);goto done;}
 struct rect rects[128];uint32_t count=0;
 for(uint32_t seed_x=0;seed_x<nx;++seed_x)for(uint32_t seed_y=0;seed_y<ny;++seed_y){
  uint32_t seed=seed_y*nx+seed_x;if(!grid[seed])continue;
  if(count==128){result=rect_bounds(prev,cur,w,h,output);goto done;}
  uint32_t top=0,x0=seed_x,x1=seed_x,y0=seed_y,y1=seed_y;stack[top++]=seed;grid[seed]=0;
  while(top){
   uint32_t at=stack[--top],x=at%nx,y=at/nx;if(x<x0)x0=x;if(x>x1)x1=x;if(y<y0)y0=y;if(y>y1)y1=y;
   if(x&&grid[at-1]){grid[at-1]=0;stack[top++]=at-1;}
   if(x+1<nx&&grid[at+1]){grid[at+1]=0;stack[top++]=at+1;}
   if(y&&grid[at-nx]){grid[at-nx]=0;stack[top++]=at-nx;}
   if(y+1<ny&&grid[at+nx]){grid[at+nx]=0;stack[top++]=at+nx;}
  }
  uint32_t min_pair=columns,max_pair=0,min_y=h,max_y=0;
  uint32_t start_pair=x0*pairs,end_pair=(x1+1)*pairs,end_y=(y1+1)*rows;if(end_pair>columns)end_pair=columns;if(end_y>h)end_y=h;
  for(uint32_t y=y0*rows;y<end_y;++y){
   uint32_t first=start_pair,last=end_pair-1,offset=y*w*2;
   while(first<end_pair&&pair_equal(prev,cur,offset+first*4))++first;
   if(first==end_pair)continue;
   while(last>first&&pair_equal(prev,cur,offset+last*4))--last;
   if(first<min_pair)min_pair=first;if(last>max_pair)max_pair=last;if(y<min_y)min_y=y;max_y=y;
  }
  if(min_y<h)rects[count++]=(struct rect){min_pair*2,min_y,(max_pair-min_pair+1)*2,max_y-min_y+1};
 }
 while(count>1){
  int32_t best=INT32_MAX;uint32_t bi=0,bj=1;struct rect chosen={0};
  for(uint32_t i=0;i+1<count;++i)for(uint32_t j=i+1;j<count;++j){struct rect merged=unite(rects[i],rects[j]);int32_t extra=cost(merged)-cost(rects[i])-cost(rects[j]);if(extra<best){best=extra;bi=i;bj=j;chosen=merged;}}
  if(count<=maximum&&best>0)break;
  rects[bi]=chosen;memmove(rects+bj,rects+bj+1,(count-bj-1)*sizeof(*rects));--count;
 }
 for(uint32_t i=1;i<count;++i){struct rect value=rects[i];uint32_t j=i;while(j&&(rects[j-1].y>value.y||(rects[j-1].y==value.y&&rects[j-1].x>value.x))){rects[j]=rects[j-1];--j;}rects[j]=value;}
 for(uint32_t i=0;i<count;++i){output[i*4]=rects[i].x;output[i*4+1]=rects[i].y;output[i*4+2]=rects[i].w;output[i*4+3]=rects[i].h;}
 result=(int32_t)count;
done:free(stack);free(grid);return result;
}
