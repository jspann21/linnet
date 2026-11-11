#ifndef LINNET_GAME_VORTEX_H
#define LINNET_GAME_VORTEX_H

/* Vortex */
static const int8_t ex_sin32[32]={0,25,49,71,90,106,118,125,127,125,118,106,90,71,49,25,0,-25,-49,-71,-90,-106,-118,-125,-127,-125,-118,-106,-90,-71,-49,-25};
struct vx_actor{uint8_t angle,radius,kind,live;};
static struct vx_actor vx_enemy[10],vx_shot[6];
static uint8_t vx_angle,vx_lives,vx_chain,vx_spawn;
static uint16_t vx_wave;
static void vx_start(void){for(uint8_t i=0;i<10u;++i)vx_enemy[i].live=0;for(uint8_t i=0;i<6u;++i)vx_shot[i].live=0;vx_angle=192;vx_lives=3;vx_wave=1;vx_chain=vx_spawn=0;ex_phase=EX_READY;}
static uint8_t angle_distance(uint8_t a,uint8_t b){uint8_t d=(uint8_t)(a-b);return d>128u?(uint8_t)(0u-d):d;}
static void vx_fire(void){for(uint8_t i=0;i<6u;++i)if(!vx_shot[i].live){vx_shot[i]=(struct vx_actor){vx_angle,91u,0u,1u};sound_pending=1u;return;}}
static void vx_step(void){
    if(++vx_spawn>18u-(vx_wave<8u?vx_wave:8u)){vx_spawn=0;for(uint8_t i=0;i<10u;++i)if(!vx_enemy[i].live){vx_enemy[i]=(struct vx_actor){(uint8_t)random_next(),84u,(uint8_t)(random_next()%3u),1u};break;}}
    for(uint8_t s=0;s<6u;++s)if(vx_shot[s].live){if(vx_shot[s].radius<6u)vx_shot[s].live=0;else vx_shot[s].radius=(uint8_t)(vx_shot[s].radius-5u);}
    for(uint8_t e=0;e<10u;++e)if(vx_enemy[e].live){
        uint8_t advance=(uint8_t)(1u+(vx_wave>5u));vx_enemy[e].radius=vx_enemy[e].radius>advance?(uint8_t)(vx_enemy[e].radius-advance):0u;vx_enemy[e].angle+=(vx_enemy[e].kind==1u?1u:vx_enemy[e].kind==2u?255u:0u);
        for(uint8_t s=0;s<6u;++s)if(vx_shot[s].live&&angle_distance(vx_shot[s].angle,vx_enemy[e].angle)<8u&&
            (vx_shot[s].radius>vx_enemy[e].radius?vx_shot[s].radius-vx_enemy[e].radius:vx_enemy[e].radius-vx_shot[s].radius)<7u){
            vx_enemy[e].live=vx_shot[s].live=0;if(vx_chain<12u)++vx_chain;uint32_t add=8u+vx_chain*2u;score=(uint16_t)(score>65535u-add?65535u:score+add);sound_pending=2u;
            if(score/180u+1u>vx_wave)vx_wave=(uint16_t)(score/180u+1u);break;
        }
        if(vx_enemy[e].live&&vx_enemy[e].radius<=8u){vx_enemy[e].live=0;vx_chain=0;if(vx_lives)--vx_lives;sound_pending=3u;if(!vx_lives){ex_phase=EX_LOST;extra_record();}}
    }
}

#endif
