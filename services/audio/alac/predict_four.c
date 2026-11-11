#include "services/audio/alac/predict.h"

static inline int32_t signed_sample(uint32_t value,unsigned bits)
{
    return (int32_t)(value<<(32u-bits))>>(32u-bits);
}

void ln_alac_predict_four(int32_t *samples,int output_size,int readsamplesize,
                     int16_t *predictor_coef_table,int predictor_quantitization)
{
    int32_t *buffer_out=samples,*error_buffer=samples;
    int i;
    /* Retain the Linnet four-coefficient and five-sample window. */
    /* Signed halfword reloads avoid widened coefficient spills on ARM7. */
    volatile int16_t c0=predictor_coef_table[0],c1=predictor_coef_table[1];
    volatile int16_t c2=predictor_coef_table[2],c3=predictor_coef_table[3];
    int32_t x0=buffer_out[0],x1=buffer_out[1],x2=buffer_out[2];
    int32_t x3=buffer_out[3],x4=buffer_out[4];
    for (i = 4 + 1; i < output_size; i++)
    {
        int sum = 0;
        int outval;
        int error_val = error_buffer[i];

        sum = (x4-x0)*c0+(x3-x0)*c1+(x2-x0)*c2+(x1-x0)*c3;

        outval = (1 << (predictor_quantitization-1)) + sum;
        outval = outval >> predictor_quantitization;
        outval = outval + x0 + error_val;
        outval = signed_sample((uint32_t)outval, readsamplesize);

        buffer_out[4+1] = outval;

        if (error_val > 0)
        {
#define ADAPT_POS(c,x,n) do { if(error_val>0){int val=x0-(x);if(val){ \
            if(val<0){(c)=(int16_t)((c)+1);val=-val;}else (c)=(int16_t)((c)-1); \
            error_val-=(val>>predictor_quantitization)*(n);}} } while(0)
            ADAPT_POS(c3,x1,1);ADAPT_POS(c2,x2,2);
            ADAPT_POS(c1,x3,3);ADAPT_POS(c0,x4,4);
#undef ADAPT_POS
        }
        else if (error_val < 0)
        {
#define ADAPT_NEG(c,x,n) do { if(error_val<0){int val=x0-(x);if(val){ \
            if(val>0){(c)=(int16_t)((c)+1);val=-val;}else (c)=(int16_t)((c)-1); \
            error_val-=(val>>predictor_quantitization)*(n);}} } while(0)
            ADAPT_NEG(c3,x1,1);ADAPT_NEG(c2,x2,2);
            ADAPT_NEG(c1,x3,3);ADAPT_NEG(c0,x4,4);
#undef ADAPT_NEG
        }

        x0=x1;x1=x2;x2=x3;x3=x4;x4=outval;buffer_out++;
    }
    predictor_coef_table[0]=c0;predictor_coef_table[1]=c1;
    predictor_coef_table[2]=c2;predictor_coef_table[3]=c3;
}
