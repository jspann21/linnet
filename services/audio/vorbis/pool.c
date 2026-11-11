#include "services/audio/vorbis/vorbis.h"
struct cell{uint32_t bytes,used;};
static struct ln_vorbis *current;
/* Context selection is scoped to one synchronous codec operation. No callback
 * performs I/O; the main scheduler owns calls, so this binding never survives
 * a pending source read. */
void ln_vorbis_pool_bind(struct ln_vorbis*s){current=s;}
#if defined(__arm__)
extern void ln_vorbis_abort(uintptr_t*) __attribute__((noreturn));
__asm__(".text\n.align 2\n.global ln_vorbis_guard\nln_vorbis_guard:\n"
        "stmdb sp!,{r4-r12,lr}\nstr sp,[r0]\nmov r3,r1\nmov r0,r2\nmov lr,pc\nbx r3\n"
        "ldmia sp!,{r4-r12,pc}\n.global ln_vorbis_abort\nln_vorbis_abort:\n"
        "ldr sp,[r0]\nmvn r0,#2\nldmia sp!,{r4-r12,pc}\n");
#else
static void ln_vorbis_abort(jmp_buf jump){longjmp(jump,1);}
#endif
static void exhausted(void) __attribute__((noreturn));
static void exhausted(void){current->error=LN_PCM_UNSUPPORTED;ln_vorbis_abort(current->jump);for(;;){}}
void *ogg_malloc(size_t n)
{
    if(n>LINNET_VORBIS_POOL-8u)exhausted();if(!n)n=1;n=(n+7u)&~(size_t)7u;
    uint32_t at=0;
    while(at<LINNET_VORBIS_POOL){struct cell*c=(struct cell*)(void*)(current->pool+at);
        if(!c->bytes||c->bytes>LINNET_VORBIS_POOL-at-8u)exhausted();
        if(!c->used&&c->bytes>=n){if(c->bytes>=n+16u){struct cell*next=(struct cell*)(void*)(current->pool+at+8u+n);*next=(struct cell){c->bytes-(uint32_t)n-8u,0};c->bytes=(uint32_t)n;}
            c->used=1;current->pool_used+=c->bytes;if(current->pool_used>current->pool_high)current->pool_high=current->pool_used;return c+1;}
        at+=8u+c->bytes;
    }exhausted();
}
void ogg_free(void*p)
{
    if(!p)return;uintptr_t offset=(uintptr_t)p-(uintptr_t)current->pool;
    if(offset<8u||offset>=LINNET_VORBIS_POOL||(offset&7u))exhausted();
    uint32_t at=0;struct cell*previous=0;
    while(at<offset-8u){struct cell*c=(struct cell*)(void*)(current->pool+at);previous=c;at+=8u+c->bytes;}
    if(at!=offset-8u)exhausted();struct cell*c=(struct cell*)(void*)(current->pool+at);
    if(!c->used)exhausted();c->used=0;current->pool_used-=c->bytes;
    uint32_t next=at+8u+c->bytes;if(next<LINNET_VORBIS_POOL){struct cell*n=(struct cell*)(void*)(current->pool+next);if(!n->used)c->bytes+=8u+n->bytes;}
    if(previous&&!previous->used)previous->bytes+=8u+c->bytes;
}
void *ogg_calloc(size_t n,size_t size)
{if(n>LINNET_VORBIS_POOL||size>LINNET_VORBIS_POOL||(uint64_t)n*size>LINNET_VORBIS_POOL)exhausted();size_t bytes=n*size;uint8_t*p=ogg_malloc(bytes);for(size_t i=0;i<bytes;++i)p[i]=0;return p;}
void *ogg_realloc(void*p,size_t n)
{
    if(!p)return ogg_malloc(n);if(!n){ogg_free(p);return 0;}
    struct cell*c=(struct cell*)p-1;if(c->bytes>=n)return p;
    void*next=ogg_malloc(n);ln_tremor_copy(next,p,c->bytes);ogg_free(p);return next;
}
void *ln_tremor_stack(size_t n){if(current->scratch_count>=256u)exhausted();void*p=ogg_malloc(n);current->scratch[current->scratch_count++]=p;return p;}
void ln_vorbis_scratch_clear(struct ln_vorbis*s){while(s->scratch_count)ogg_free(s->scratch[--s->scratch_count]);}
void ln_vorbis_pool_reset(struct ln_vorbis*s){*(struct cell*)(void*)s->pool=(struct cell){LINNET_VORBIS_POOL-8u,0};s->pool_used=0;s->scratch_count=0;}
static void swap(uint8_t*a,uint8_t*b,size_t n){for(size_t i=0;i<n;++i){uint8_t t=a[i];a[i]=b[i];b[i]=t;}}
static void sift(uint8_t*p,size_t root,size_t n,size_t width,int(*cmp)(const void*,const void*))
{while(root<n/2u){size_t child=root*2u+1u;if(child+1u<n&&cmp(p+child*width,p+(child+1u)*width)<0)++child;if(cmp(p+root*width,p+child*width)>=0)return;swap(p+root*width,p+child*width,width);root=child;}}
void ln_tremor_sort(void*p,size_t n,size_t width,int(*cmp)(const void*,const void*))
{if(!width||n>LINNET_VORBIS_POOL||width>LINNET_VORBIS_POOL||(uint64_t)n*width>LINNET_VORBIS_POOL)exhausted();for(size_t i=n/2u;i;i--)sift(p,i-1u,n,width,cmp);for(size_t i=n;i>1u;--i){swap(p,(uint8_t*)p+(i-1u)*width,width);sift(p,0,i-1u,width,cmp);}}
