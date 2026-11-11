/* Shared SHA compression implementation; intentionally included twice for Boot. */
static LN_SHA_COMPRESS_ATTR void LN_SHA_COMPRESS_NAME(struct ln_sha256 *s,const uint8_t *block) {
    LN_SHA_WORKSPACE;
    for (unsigned i=0;i<16;++i) {
        const uint8_t *p=block+4u*i;
        w[i]=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
    }
    for (unsigned i=16;i<64;++i) {
        uint32_t x=w[i-15],y=w[i-2];
        w[i]=w[i-16]+(rotate(x,7)^rotate(x,18)^(x>>3))+w[i-7]+(rotate(y,17)^rotate(y,19)^(y>>10));
    }
    uint32_t a=s->words[0],b=s->words[1],c=s->words[2],d=s->words[3];
    uint32_t e=s->words[4],f=s->words[5],g=s->words[6],h=s->words[7];
    /* Eight rounds return the rotated working-variable roles to their
     * original positions, avoiding explicit seven-word moves each round. */
#define LN_SHA_ROUND(a,b,c,d,e,f,g,h,j) do { \
    uint32_t t1=(h)+(rotate((e),6)^rotate((e),11)^rotate((e),25))+ \
        (((e)&(f))^(~(e)&(g)))+LN_SHA_CONSTANTS[j]+w[j]; \
    uint32_t t2=(rotate((a),2)^rotate((a),13)^rotate((a),22))+ \
        (((a)&(b))^((a)&(c))^((b)&(c))); \
    (d)+=t1;(h)=t1+t2; \
} while(0)
    for(unsigned i=0;i<64;i+=8){
        LN_SHA_ROUND(a,b,c,d,e,f,g,h,i+0u);
        LN_SHA_ROUND(h,a,b,c,d,e,f,g,i+1u);
        LN_SHA_ROUND(g,h,a,b,c,d,e,f,i+2u);
        LN_SHA_ROUND(f,g,h,a,b,c,d,e,i+3u);
        LN_SHA_ROUND(e,f,g,h,a,b,c,d,i+4u);
        LN_SHA_ROUND(d,e,f,g,h,a,b,c,i+5u);
        LN_SHA_ROUND(c,d,e,f,g,h,a,b,i+6u);
        LN_SHA_ROUND(b,c,d,e,f,g,h,a,i+7u);
    }
#undef LN_SHA_ROUND
    s->words[0]+=a;s->words[1]+=b;s->words[2]+=c;s->words[3]+=d;
    s->words[4]+=e;s->words[5]+=f;s->words[6]+=g;s->words[7]+=h;
}
