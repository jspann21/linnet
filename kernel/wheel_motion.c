#include "wheel_motion.h"
#include <limits.h>
#define IDLE_US 200000u
#define FAST_DETENT_US 80000u
void ln_wheel_motion_reset(struct ln_wheel_motion *motion)
{
    if(motion)*motion=(struct ln_wheel_motion){0};
}
int32_t ln_wheel_motion_step(struct ln_wheel_motion *motion,int32_t delta,
    bool touched,bool hold,uint32_t list_count,bool cover_flow,uint32_t now_us)
{
    if(!motion)return 0;
    if(hold||!touched){ln_wheel_motion_reset(motion);return 0;}
    if(motion->list_count!=list_count||
       (motion->direction&&(uint32_t)(now_us-motion->last_motion_us)>=IDLE_US))
        ln_wheel_motion_reset(motion);
    motion->list_count=list_count;
    if(!delta)return 0;
    int32_t direction=delta>0?1:-1;
    if(motion->direction&&motion->direction!=direction){
        ln_wheel_motion_reset(motion);motion->list_count=list_count;
    }
    motion->direction=direction;motion->last_motion_us=now_us;
    /* Widen before adding so even malformed caller deltas cannot overflow. */
    int64_t accumulated=(int64_t)motion->remainder+delta;
    int32_t steps=(int32_t)(accumulated/4);
    motion->remainder=(int32_t)(accumulated-(int64_t)steps*4);
    if(!steps)return 0;
    if(!motion->streak||(uint32_t)(now_us-motion->last_detent_us)>FAST_DETENT_US)
        motion->streak=1;
    else if(motion->streak<13u)++motion->streak;
    motion->last_detent_us=now_us;
    /* A batched packet is one observation, not evidence of several fast
     * detents. The first two observations remain exact at any list length. */
    /* Covers stay exact through the first six fast observations, then use two
     * and finally four albums per detent for a sustained turn. */
    uint32_t gain=list_count<64u?1u:cover_flow?(motion->streak>=13u?4u:motion->streak>=7u?2u:1u):
        motion->streak>=7u?4u:motion->streak>=3u?2u:1u;
    int64_t result=(int64_t)steps*gain;
    if(result>INT32_MAX)return INT32_MAX;
    if(result<INT32_MIN)return INT32_MIN;
    return (int32_t)result;
}
