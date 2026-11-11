#include "kernel/executive.h"
#define NONE UINT16_MAX
static void count(uint32_t *value) { if (*value != UINT32_MAX) ++*value; }
void ln_exec_init(ln_executive *s) {
    for (uint32_t i=0;i<LINNET_EVENT_CAPACITY;++i) s->slots[i].next=(uint16_t)(i+1u);
    s->slots[LINNET_EVENT_CAPACITY-1u].next=NONE;
    for (uint32_t i=0;i<LINNET_SERVICE_COUNT;++i) s->head[i]=s->tail[i]=NONE;
    s->free_head=0;s->used=s->high=s->accepted=s->processed=s->dropped=s->max_latency=0;
    s->deadline_used=s->deadline_high=0;
#if !defined(LINNET_PROFILE_RELEASE)
    for(uint32_t i=0;i<LINNET_SERVICE_COUNT;++i)
        s->service_accepted[i]=s->service_processed[i]=s->service_dropped[i]=s->service_max_latency[i]=0;
#endif
    for(uint32_t i=0;i<LINNET_DEADLINE_CAPACITY;++i)s->deadlines[i].active=0;
}
bool ln_exec_post(ln_executive *s,uint32_t owner,ln_event event) {
    if(owner>=LINNET_SERVICE_COUNT||s->free_head==NONE){
        count(&s->dropped);
#if !defined(LINNET_PROFILE_RELEASE)
        if(owner<LINNET_SERVICE_COUNT)count(&s->service_dropped[owner]);
#endif
        return false;
    }
    uint16_t index=s->free_head;
    s->free_head=s->slots[index].next;
    s->slots[index].event=event;
    s->slots[index].next=NONE;
    if(s->tail[owner]!=NONE){
        s->slots[s->tail[owner]].next=index;
    }else{
        s->head[owner]=index;
    }
    s->tail[owner]=index;
    ++s->used;
    if(s->used>s->high)s->high=s->used;
    count(&s->accepted);
#if !defined(LINNET_PROFILE_RELEASE)
    count(&s->service_accepted[owner]);
#endif
    return true;
}
bool ln_exec_take(ln_executive *s,uint32_t now,uint32_t *owner,ln_event *event) {
    if(!owner||!event)return false;
    for(uint32_t service=0;service<LINNET_SERVICE_COUNT;++service){
        uint16_t index=s->head[service];
        if(index==NONE)continue;
        *owner=service;
        *event=s->slots[index].event;
        s->head[service]=s->slots[index].next;
        if(s->head[service]==NONE)s->tail[service]=NONE;
        s->slots[index].next=s->free_head;
        s->free_head=index;
        --s->used;
        count(&s->processed);
        uint32_t latency=now-event->time;
        if(latency>s->max_latency)s->max_latency=latency;
#if !defined(LINNET_PROFILE_RELEASE)
        count(&s->service_processed[service]);
        if(latency>s->service_max_latency[service])s->service_max_latency[service]=latency;
#endif
        return true;
    }
    return false;
}
#if !defined(LINNET_PROFILE_RELEASE)
void ln_exec_diagnostics(const ln_executive *s,uint32_t words[24]){
    words[0]=0x56535845u;words[1]=LINNET_SERVICE_COUNT;words[2]=s->used;words[3]=s->high;
    for(uint32_t i=0;i<LINNET_SERVICE_COUNT;++i){
        words[4+i]=s->service_accepted[i];words[9+i]=s->service_processed[i];
        words[14+i]=s->service_dropped[i];words[19+i]=s->service_max_latency[i];
    }
}
#endif
bool ln_exec_schedule(ln_executive *s,uint32_t owner,uint16_t key,uint32_t due,ln_event event){
    if(owner>=LINNET_SERVICE_COUNT)return false;
    uint32_t free_slot=LINNET_DEADLINE_CAPACITY;
    for(uint32_t i=0;i<LINNET_DEADLINE_CAPACITY;++i){
        ln_deadline *d=&s->deadlines[i];
        if(d->active&&d->owner==owner&&d->key==key){
            d->due=due;
            d->event=event;
            return true;
        }
        if(!d->active&&free_slot==LINNET_DEADLINE_CAPACITY)free_slot=i;
    }
    if(free_slot==LINNET_DEADLINE_CAPACITY){
        count(&s->dropped);
#if !defined(LINNET_PROFILE_RELEASE)
        count(&s->service_dropped[owner]);
#endif
        return false;
    }
    ln_deadline *d=&s->deadlines[free_slot];
    d->due=due;
    d->event=event;
    d->key=key;
    d->owner=(uint8_t)owner;
    d->active=1;
    ++s->deadline_used;
    if(s->deadline_used>s->deadline_high)s->deadline_high=s->deadline_used;
    return true;
}
void ln_exec_cancel(ln_executive *s,uint32_t owner,uint16_t key){
    for(uint32_t i=0;i<LINNET_DEADLINE_CAPACITY;++i){
        ln_deadline*d=&s->deadlines[i];
        if(d->active&&d->owner==owner&&d->key==key){
            d->active=0;
            --s->deadline_used;
        }
    }
}
void ln_exec_due(ln_executive *s,uint32_t now){
    for(uint32_t i=0;i<LINNET_DEADLINE_CAPACITY;++i){
        ln_deadline*d=&s->deadlines[i];
        if(d->active&&(int32_t)(now-d->due)>=0){
            /* Keep a due item owned until the event pool accepts it. */
            if(s->free_head==NONE)break;
            d->event.time=d->due;
            if(ln_exec_post(s,d->owner,d->event)){
                d->active=0;
                --s->deadline_used;
            }
        }
    }
}
uint32_t ln_exec_next(const ln_executive*s,uint32_t now,uint32_t fallback){
    if(s->used)return now;
    uint32_t next=fallback;
    for(uint32_t i=0;i<LINNET_DEADLINE_CAPACITY;++i){
        if(!s->deadlines[i].active)continue;
        uint32_t due=s->deadlines[i].due;
        if((int32_t)(due-now)<=0)return now;
        if((int32_t)(due-next)<0)next=due;
    }
    return next;
}
void ln_arena_init(ln_arena*a,void*memory,uint32_t size){
    a->bytes=memory;
    a->capacity=size;
    a->used=a->high=0;
    a->generation=1;
}
void ln_arena_reset(ln_arena*a){
    a->used=0;
    ++a->generation;
    if(!a->generation)a->generation=1;
}
bool ln_arena_alloc(ln_arena*a,uint32_t size,uint32_t alignment,ln_arena_ref*r){
    if(!r||!a->bytes||!size||!alignment||(alignment&(alignment-1u))||alignment>256u)return false;
    uint32_t pad=(uint32_t)(0u-((uintptr_t)a->bytes+a->used))&(alignment-1u);
    if(pad>a->capacity-a->used||size>a->capacity-a->used-pad)return false;
    r->offset=a->used+pad;
    r->size=size;
    r->generation=a->generation;
    a->used=r->offset+size;
    if(a->used>a->high)a->high=a->used;
    return true;
}
void*ln_arena_get(ln_arena*a,ln_arena_ref r){
    if(r.generation!=a->generation||r.offset>a->used||r.size>a->used-r.offset)return 0;
    return a->bytes+r.offset;
}
