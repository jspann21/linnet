#include <stdint.h>
/* ARM EABI returns a 64-bit integer in r0(low),r1(high), matching the divmod
 * quotient/remainder convention. No division instruction or runtime recursion.
 * Zero divisor yields quotient zero and the original numerator as remainder. */
static uint64_t unsigned_divmod(uint32_t n,uint32_t d)
{
    if(!d)return (uint64_t)n<<32;uint32_t q=0,r=0;
    for(uint32_t i=32;i;--i){uint32_t carry=r>>31;r=(r<<1)|((n>>(i-1u))&1u);
        if(carry||r>=d){r-=d;q|=1u<<(i-1u);}}
    return q|((uint64_t)r<<32);
}
__attribute__((weak)) uint64_t __aeabi_uidivmod(uint32_t n,uint32_t d){return unsigned_divmod(n,d);}
__attribute__((weak)) uint32_t __aeabi_uidiv(uint32_t n,uint32_t d){return (uint32_t)unsigned_divmod(n,d);}
__attribute__((weak)) uint64_t __aeabi_idivmod(int32_t n,int32_t d)
{
    uint32_t a=n<0?0u-(uint32_t)n:(uint32_t)n,b=d<0?0u-(uint32_t)d:(uint32_t)d;
    uint64_t result=unsigned_divmod(a,b);uint32_t q=(uint32_t)result,r=(uint32_t)(result>>32);
    if((n<0)!=(d<0))q=0u-q;if(n<0)r=0u-r;return q|((uint64_t)r<<32);
}
__attribute__((weak)) int32_t __aeabi_idiv(int32_t n,int32_t d){return (int32_t)__aeabi_idivmod(n,d);}
