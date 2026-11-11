#ifndef LINNET_WHEEL_MOTION_H
#define LINNET_WHEEL_MOTION_H
#include <stdbool.h>
#include <stdint.h>
struct ln_wheel_motion {
    uint32_t last_motion_us,last_detent_us,list_count;
    int32_t remainder,direction;
    uint32_t streak;
};
/* Reset on scene/navigation changes, even when the finger remains down. */
void ln_wheel_motion_reset(struct ln_wheel_motion *motion);
/* Physical sensor packets only. Call for lift/hold packets too. Synthetic
 * wheel events keep their existing exact item-count semantics. */
int32_t ln_wheel_motion_step(struct ln_wheel_motion *motion,int32_t delta,
    bool touched,bool hold,uint32_t list_count,bool cover_flow,uint32_t now_us);
#endif
