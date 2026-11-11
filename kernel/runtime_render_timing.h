/* RNDR dev OS: only dirty render/present attempts, inclusive wall time.
 * No timer/accounting work on idle callbacks or animation-only publications.
 * Single CPU task context. IRQ/FIQ/COP must not write these counters.
 */
#ifndef LN_RUNTIME_RENDER_TIMING_H
#define LN_RUNTIME_RENDER_TIMING_H
static struct {uint32_t epoch,flags,depth,value[8];} rnd;
_Static_assert(sizeof(rnd)==44u,"RND static RAM budget");
struct rnd_span {uint32_t start,render_end;};
static void rnd_add(uint32_t index,uint32_t amount){
    if(amount>UINT32_MAX-rnd.value[index]){rnd.value[index]=UINT32_MAX;rnd.flags|=1u;}
    else rnd.value[index]+=amount;
}
static void rnd_init(void){
    if(rnd.depth){rnd.flags|=4u;return;}
    uint32_t epoch=rnd.epoch+1u;rnd.epoch=epoch?epoch:1u;rnd.flags=epoch?0u:8u;
    for(uint32_t i=0;i<8u;++i)rnd.value[i]=0;
}
static inline struct rnd_span rnd_enter(void){
    struct rnd_span span={ln_time_us(),0};
    if(rnd.depth)rnd_add(7u,1u);
    if(rnd.depth==UINT32_MAX)rnd.flags|=1u;else ++rnd.depth;
    return span;
}
static inline void rnd_rendered(struct rnd_span *span){span->render_end=ln_time_us();}
static void rnd_leave(const struct rnd_span *span,bool accepted){
    uint32_t end=ln_time_us(),render=span->render_end-span->start,publish=end-span->render_end;
    if(render>=0x80000000u||publish>=0x80000000u)rnd.flags|=2u;
    rnd_add(0u,1u);rnd_add(1u,render);if(render>rnd.value[2])rnd.value[2]=render;
    rnd_add(3u,1u);rnd_add(4u,publish);if(publish>rnd.value[5])rnd.value[5]=publish;
    if(accepted)rnd_add(6u,1u);
    if(rnd.depth)--rnd.depth;else rnd.flags|=16u;
}
void ln_runtime_render_timing(uint32_t words[16]){
    words[0]=0x52444e52u;words[1]=0u;words[2]=rnd.epoch;
    words[3]=rnd.flags|(rnd.depth?0x80000000u:0u);words[4]=ln_time_us();
    words[5]=rnd.depth;words[6]=words[7]=0;
    for(uint32_t i=0;i<8u;++i)words[8u+i]=rnd.value[i];
}
#endif
