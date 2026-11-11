/*
 * ALAC (Apple Lossless Audio Codec) decoder
 * Copyright (c) 2005 David Hammerton
 * All rights reserved.
 *
 * This is the actual decoder.
 *
 * http://crazney.net/programs/itunes/alac.html
 * 
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */


#include <stdint.h>
#include <stddef.h>
static void *ln_alac_fill(void *to,int value,size_t count)
{uint8_t *d=to;for(size_t i=0;i<count;i++)d[i]=(uint8_t)value;return to;}
#define memset ln_alac_fill
#define IBSS_ATTR
#define ICODE_ATTR
#include "decomp.h"
#include "services/audio/alac/predict.h"

#define SIGNEXTEND24(val) (((signed)val<<8)>>8)

static int16_t predictor_coef_table[32] IBSS_ATTR;
static int16_t predictor_coef_table_a[32] IBSS_ATTR;
static int16_t predictor_coef_table_b[32] IBSS_ATTR;


/* stream reading */

/* Linnet: every bit access is bounded by the exact packet length. */
static inline uint32_t readbits_16(alac_file *alac,int bits)
{
    if(bits<0||bits>32||alac->failed){alac->failed=1;return 0;}
    uint32_t available=(uint32_t)(alac->input_end-alac->input_buffer)*8u-(uint32_t)alac->input_buffer_bitaccumulator;
    if((uint32_t)bits>available){alac->failed=1;return 0;}
    uint32_t value=0;
    while(bits){
        uint32_t remaining=8u-(uint32_t)alac->input_buffer_bitaccumulator;
        uint32_t take=(uint32_t)bits<remaining?(uint32_t)bits:remaining;
        value=(value<<take)|(((uint32_t)*alac->input_buffer>>(remaining-take))&((1u<<take)-1u));
        bits-=(int)take;alac->input_buffer_bitaccumulator+=(int)take;
        if(alac->input_buffer_bitaccumulator==8){alac->input_buffer++;alac->input_buffer_bitaccumulator=0;}
    }
    return value;
}
static inline uint32_t readbits(alac_file *alac,int bits){return readbits_16(alac,bits);}
static inline int readbit(alac_file *alac){
    if(alac->failed){alac->failed=1;return 0;}
    uint32_t available=(uint32_t)(alac->input_end-alac->input_buffer)*8u-(uint32_t)alac->input_buffer_bitaccumulator;
    if(!available){alac->failed=1;return 0;}
    int value=(int)((*alac->input_buffer>>(7-alac->input_buffer_bitaccumulator))&1u);
    if(++alac->input_buffer_bitaccumulator==8){alac->input_buffer++;alac->input_buffer_bitaccumulator=0;}
    return value;
}
static inline void unreadbits(alac_file *alac,int bits)
{
    uint32_t position=(uint32_t)(alac->input_buffer-alac->input_start)*8u+(uint32_t)alac->input_buffer_bitaccumulator;
    if(bits<0||(uint32_t)bits>position){alac->failed=1;return;}
    position-=(uint32_t)bits;alac->input_buffer=alac->input_start+position/8u;alac->input_buffer_bitaccumulator=(int)(position%8u);
}
static inline int ln_alac_clz(uint32_t value){
#if defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 5
    return value ? __builtin_clz(value) : 32;
#else
    if(!value)return 32;
    int count=0;
    if(!(value&0xffff0000u)){count+=16;value<<=16;}
    if(!(value&0xff000000u)){count+=8;value<<=8;}
    if(!(value&0xf0000000u)){count+=4;value<<=4;}
    if(!(value&0xc0000000u)){count+=2;value<<=2;}
    if(!(value&0x80000000u))++count;
    return count;
#endif
}
#define count_leading_zeros(x) ln_alac_clz((uint32_t)(x))

#define RICE_THRESHOLD 8 // maximum number of bits for a rice prefix.

static inline int32_t entropy_decode_value(alac_file* alac,
                             int readsamplesize,
                             int k) ICODE_ATTR_ALAC;
static inline int32_t entropy_decode_value(alac_file* alac,
                             int readsamplesize,
                             int k)
{
    if(k<0||k>31||readsamplesize<1||readsamplesize>32){alac->failed=1;return 0;}
    int32_t x = 0; // decoded value
    
    // read x, number of 1s before 0 represent the rice value.
    while (x <= RICE_THRESHOLD && readbit(alac))
    {
        x++;
    }
    
    if (x > RICE_THRESHOLD)
    {
        // read the number from the bit stream (raw value)
        int32_t value;
        
        value = readbits(alac, readsamplesize);
        
        /* mask value to readsamplesize size */
        if (readsamplesize != 32)
            value &= (((uint32_t)0xffffffff) >> (32 - readsamplesize));
        
        x = value;
    }
    else
    {
        if (k != 1)
        {
            int extrabits = readbits(alac, k);
            
            // x = x * (2^k - 1)
            x = (x << k) - x;
            
            if (extrabits > 1)
                x += extrabits - 1;
            else
                unreadbits(alac, 1);
        }
    }
    
    return x;
}

static void entropy_rice_decode(alac_file* alac,
                         int32_t* output_buffer,
                         int output_size,
                         int readsamplesize,
                         int rice_initialhistory,
                         int rice_kmodifier,
                         int rice_historymult,
                         int rice_kmodifier_mask) ICODE_ATTR_ALAC;
static void entropy_rice_decode(alac_file* alac,
                         int32_t* output_buffer,
                         int output_size,
                         int readsamplesize,
                         int rice_initialhistory,
                         int rice_kmodifier,
                         int rice_historymult,
                         int rice_kmodifier_mask)
{
    int                output_count;
    int                history = rice_initialhistory;
    int                sign_modifier = 0;
    
    for (output_count = 0; output_count < output_size; output_count++)
    {
        int32_t        decoded_value;
        int32_t        final_value;
        int32_t        k;
        
        k = 31 - rice_kmodifier - count_leading_zeros((history >> 9) + 3);
        
        if (k < 0) k += rice_kmodifier;
        else k = rice_kmodifier;
        
        decoded_value = entropy_decode_value(alac, readsamplesize, k);
        
        if(alac->failed)return;
        decoded_value += sign_modifier;
        final_value = (decoded_value + 1) / 2; // inc by 1 and shift out sign bit
        if (decoded_value & 1) // the sign is stored in the low bit
            final_value *= -1;
        
        output_buffer[output_count] = final_value;
        
        sign_modifier = 0;
        
        // update history
        history += (decoded_value * rice_historymult)
                - ((history * rice_historymult) >> 9);
        
        if (decoded_value > 0xFFFF)
            history = 0xFFFF;
        
        // special case, for compressed blocks of 0
        if ((history < 128) && (output_count + 1 < output_size))
        {
            int32_t        block_size;
            
            sign_modifier = 1;
            
            k = count_leading_zeros(history) + ((history + 16) / 64) - 24;
            
            // note: block_size is always 16bit
            block_size = entropy_decode_value(alac, 16, k) & rice_kmodifier_mask;
            
            if(alac->failed||block_size>output_size-output_count-1){alac->failed=1;return;}
            // got block_size 0s
            if (block_size > 0)
            {
                memset(&output_buffer[output_count + 1], 0, 
                       block_size * sizeof(*output_buffer));
                output_count += block_size;
            }
            
            if (block_size > 0xFFFF)
                sign_modifier = 0;
            
            history = 0;
        }
    }
}

#define SIGN_EXTENDED32(val, bits) ((val << (32 - bits)) >> (32 - bits))

static inline int decode_frame_mono(
                    alac_file *alac,
                    int32_t outputbuffer[ALAC_MAX_CHANNELS][ALAC_BLOCKSIZE],
                    void (*yield)(void))
{
    int hassize;
    int isnotcompressed;
    int readsamplesize;
    int infosamplesize = alac->setinfo_sample_size;
    int outputsamples = alac->setinfo_max_samples_per_frame;

    int uncompressed_bytes;
    int ricemodifier;


    /* 2^result = something to do with output waiting.
     * perhaps matters if we read > 1 frame in a pass?
     */
    readbits(alac, 4);

    readbits(alac, 12); /* unknown, skip 12 bits */

    hassize = readbits(alac, 1); /* the output sample size is stored soon */

    /* number of bytes in the (compressed) stream that are not compressed */
    uncompressed_bytes = readbits(alac, 2);

    isnotcompressed = readbits(alac, 1); /* whether the frame is compressed */

    if (hassize)
    {
        /* now read the number of samples,
         * as a 32bit integer */
        outputsamples = readbits(alac, 32);
    }

    if(alac->failed||outputsamples<=0||outputsamples>ALAC_BLOCKSIZE||(uint32_t)outputsamples>alac->setinfo_max_samples_per_frame||uncompressed_bytes)return -1;
    readsamplesize = infosamplesize - (uncompressed_bytes * 8);

    if (!isnotcompressed)
    { /* so it is compressed */
        int predictor_coef_num;
        int prediction_type;
        int prediction_quantitization;
        int i;

        /* skip 16 bits, not sure what they are. seem to be used in
         * two channel case */
        readbits(alac, 8);
        readbits(alac, 8);

        prediction_type = readbits(alac, 4);
        prediction_quantitization = readbits(alac, 4);

        ricemodifier = readbits(alac, 3);
        predictor_coef_num = readbits(alac, 5);

        if(prediction_type||!prediction_quantitization||(predictor_coef_num!=31&&predictor_coef_num>=outputsamples))return -1;
        /* read the predictor table */
        for (i = 0; i < predictor_coef_num; i++)
        {
            predictor_coef_table[i] = (int16_t)readbits(alac, 16);
        }

        if (uncompressed_bytes)
        {
            int i;
            for (i = 0; i < outputsamples; i++)
            {
                outputbuffer[0][i] = readbits(alac, uncompressed_bytes * 8);
                outputbuffer[1][i] = outputbuffer[0][i];
            }
        }

        yield();

        entropy_rice_decode(alac,
                            outputbuffer[0],
                            outputsamples,
                            readsamplesize,
                            alac->setinfo_rice_initialhistory,
                            alac->setinfo_rice_kmodifier,
                            ricemodifier * alac->setinfo_rice_historymult / 4,
                            (1 << alac->setinfo_rice_kmodifier) - 1);

        yield();

        if (prediction_type == 0)
        { /* adaptive fir */
            ln_alac_predict(outputbuffer[0],
                            outputsamples,
                            readsamplesize,
                            predictor_coef_table,
                            predictor_coef_num,
                            prediction_quantitization);
        }
        else
        {
            /* Nonzero prediction types are rejected before entropy decoding. */
        }

    }
    else
    { /* not compressed, easy case */
        if (infosamplesize <= 16)
        {
            int i;
            for (i = 0; i < outputsamples; i++)
            {
                int32_t audiobits = readbits(alac, infosamplesize);

                audiobits = SIGN_EXTENDED32(audiobits, infosamplesize);

                outputbuffer[0][i] = audiobits;
            }
        }
        else
        {
            int i;
            for (i = 0; i < outputsamples; i++)
            {
                int32_t audiobits;

                audiobits = readbits(alac, 16);
                /* special case of sign extension..
                 * as we'll be ORing the low 16bits into this */
                audiobits = audiobits << (infosamplesize - 16);
                audiobits |= readbits(alac, infosamplesize - 16);
                audiobits = SIGNEXTEND24(audiobits);

                outputbuffer[0][i] = audiobits;
            }
        }
        uncompressed_bytes = 0; // always 0 for uncompressed
    }

    yield();

    alac->output_channels=1;alac->output_shift=0;alac->output_weight=0;

    return alac->failed?-1:outputsamples;
}

static inline int decode_frame_stereo(
                    alac_file *alac,
                    int32_t outputbuffer[ALAC_MAX_CHANNELS][ALAC_BLOCKSIZE],
                    void (*yield)(void))
{
    int hassize;
    int isnotcompressed;
    int readsamplesize;
    int infosamplesize = alac->setinfo_sample_size;
    int outputsamples = alac->setinfo_max_samples_per_frame;
    int uncompressed_bytes;

    uint8_t interlacing_shift;
    uint8_t interlacing_leftweight;

    /* 2^result = something to do with output waiting.
     * perhaps matters if we read > 1 frame in a pass?
     */
    readbits(alac, 4);

    readbits(alac, 12); /* unknown, skip 12 bits */

    hassize = readbits(alac, 1); /* the output sample size is stored soon */

    /* the number of bytes in the (compressed) stream that are not compressed */
    uncompressed_bytes = readbits(alac, 2);

    isnotcompressed = readbits(alac, 1); /* whether the frame is compressed */

    if (hassize)
    {
        /* now read the number of samples,
         * as a 32bit integer */
        outputsamples = readbits(alac, 32);
    }

    if(alac->failed||outputsamples<=0||outputsamples>ALAC_BLOCKSIZE||(uint32_t)outputsamples>alac->setinfo_max_samples_per_frame||uncompressed_bytes)return -1;
    readsamplesize = infosamplesize - (uncompressed_bytes * 8) + 1;

    yield();
    if (!isnotcompressed)
    { /* compressed */
        int predictor_coef_num_a;
        int prediction_type_a;
        int prediction_quantitization_a;
        int ricemodifier_a;

        int predictor_coef_num_b;
        int prediction_type_b;
        int prediction_quantitization_b;
        int ricemodifier_b;

        int i;

        interlacing_shift = readbits(alac, 8);
        interlacing_leftweight = readbits(alac, 8);

        /******** channel 1 ***********/
        prediction_type_a = readbits(alac, 4);
        prediction_quantitization_a = readbits(alac, 4);

        ricemodifier_a = readbits(alac, 3);
        predictor_coef_num_a = readbits(alac, 5);

        if(interlacing_shift>31||prediction_type_a||!prediction_quantitization_a||(predictor_coef_num_a!=31&&predictor_coef_num_a>=outputsamples))return -1;
        /* read the predictor table */
        for (i = 0; i < predictor_coef_num_a; i++)
        {
            predictor_coef_table_a[i] = (int16_t)readbits(alac, 16);
        }

        /******** channel 2 *********/
        prediction_type_b = readbits(alac, 4);
        prediction_quantitization_b = readbits(alac, 4);

        ricemodifier_b = readbits(alac, 3);
        predictor_coef_num_b = readbits(alac, 5);

        if(prediction_type_b||!prediction_quantitization_b||(predictor_coef_num_b!=31&&predictor_coef_num_b>=outputsamples))return -1;
        /* read the predictor table */
        for (i = 0; i < predictor_coef_num_b; i++)
        {
            predictor_coef_table_b[i] = (int16_t)readbits(alac, 16);
        }

        /*********************/
        if (uncompressed_bytes)
        { /* see mono case */
            int i;
            for (i = 0; i < outputsamples; i++)
            {
                outputbuffer[0][i] = readbits(alac, uncompressed_bytes * 8);
                outputbuffer[1][i] = readbits(alac, uncompressed_bytes * 8);
            }
        }

        yield();
        /* channel 1 */
        entropy_rice_decode(alac,
                            outputbuffer[0],
                            outputsamples,
                            readsamplesize,
                            alac->setinfo_rice_initialhistory,
                            alac->setinfo_rice_kmodifier,
                            ricemodifier_a * alac->setinfo_rice_historymult / 4,
                            (1 << alac->setinfo_rice_kmodifier) - 1);

        yield();
        if (prediction_type_a == 0)
        { /* adaptive fir */
            ln_alac_predict(outputbuffer[0],
                            outputsamples,
                            readsamplesize,
                            predictor_coef_table_a,
                            predictor_coef_num_a,
                            prediction_quantitization_a);
        }
        else
        { /* see mono case */
            /* Nonzero prediction types are rejected before entropy decoding. */
        }

        yield();

        /* channel 2 */
        entropy_rice_decode(alac,
                            outputbuffer[1],
                            outputsamples,
                            readsamplesize,
                            alac->setinfo_rice_initialhistory,
                            alac->setinfo_rice_kmodifier,
                            ricemodifier_b * alac->setinfo_rice_historymult / 4,
                            (1 << alac->setinfo_rice_kmodifier) - 1);

        yield();
        if (prediction_type_b == 0)
        { /* adaptive fir */
            ln_alac_predict(outputbuffer[1],
                            outputsamples,
                            readsamplesize,
                            predictor_coef_table_b,
                            predictor_coef_num_b,
                            prediction_quantitization_b);
        }
        else
        {
            /* Nonzero prediction types are rejected before entropy decoding. */
        }
    }
    else
    { /* not compressed, easy case */
        if (infosamplesize <= 16)
        {
            int i;
            for (i = 0; i < outputsamples; i++)
            {
                int32_t audiobits_a, audiobits_b;

                audiobits_a = readbits(alac, infosamplesize);
                audiobits_b = readbits(alac, infosamplesize);

                audiobits_a = SIGN_EXTENDED32(audiobits_a, infosamplesize);
                audiobits_b = SIGN_EXTENDED32(audiobits_b, infosamplesize);

                outputbuffer[0][i] = audiobits_a;
                outputbuffer[1][i] = audiobits_b;
            }
        }
        else
        {
            int i;
            for (i = 0; i < outputsamples; i++)
            {
                int32_t audiobits_a, audiobits_b;

                audiobits_a = readbits(alac, 16);
                audiobits_a = audiobits_a << (infosamplesize - 16);
                audiobits_a |= readbits(alac, infosamplesize - 16);
                audiobits_a = SIGNEXTEND24(audiobits_a);

                audiobits_b = readbits(alac, 16);
                audiobits_b = audiobits_b << (infosamplesize - 16);
                audiobits_b |= readbits(alac, infosamplesize - 16);
                audiobits_b = SIGNEXTEND24(audiobits_b);

                outputbuffer[0][i] = audiobits_a;
                outputbuffer[1][i] = audiobits_b;
            }
        }
        uncompressed_bytes = 0; // always 0 for uncompressed
        interlacing_shift = 0;
        interlacing_leftweight = 0;
    }

    yield();

    alac->output_channels=2;
    alac->output_shift=interlacing_shift;alac->output_weight=interlacing_leftweight;

    return alac->failed?-1:outputsamples;
}

int alac_decode_frame(alac_file *alac,
                      unsigned char *inbuffer, uint32_t input_bytes,
                      int32_t outputbuffer[ALAC_MAX_CHANNELS][ALAC_BLOCKSIZE],
                      void (*yield)(void))
{
    int channels;
    int outputsamples;
    unsigned char *input_buffer_start;

    /* setup the stream */
    if(!alac||!inbuffer||!yield||!input_bytes||input_bytes>32768u||alac->setinfo_sample_size!=16||!alac->setinfo_max_samples_per_frame||alac->setinfo_max_samples_per_frame>4096||alac->setinfo_rice_kmodifier>14)return -1;
    alac->input_start=inbuffer;alac->input_end=inbuffer+input_bytes;alac->failed=0;
    alac->input_buffer = inbuffer;
    alac->input_buffer_bitaccumulator = 0;
    
    /* save to gather byte consumption */
    input_buffer_start = alac->input_buffer;

    channels = readbits(alac, 3);

    if(channels+1!=alac->setinfo_7f)return -1;
    /* TODO: The mono and stereo functions should be combined. */
    switch(channels)
    {
        case 0: /* 1 channel */
            outputsamples=decode_frame_mono(alac,outputbuffer,yield);
            break;
        case 1: /* 2 channels */
            outputsamples=decode_frame_stereo(alac,outputbuffer,yield);
            break;
        default: /* Unsupported */
            return -1;
    }
    
    if(outputsamples<=0||alac->failed||readbits(alac,3)!=7)return -1;
    while(alac->input_buffer_bitaccumulator)if(readbit(alac))return -1;
    if(alac->failed||alac->input_buffer!=alac->input_end)return -1;
    /* Exact packet consumption, including END element and zero byte padding. */
    alac->bytes_consumed = (int)(alac->input_buffer - input_buffer_start);

    return alac->failed?-1:outputsamples;
}

/* rockbox: not used
void create_alac(int samplesize, int numchannels, alac_file* alac)
{
    alac->samplesize = samplesize;
    alac->numchannels = numchannels;
    alac->bytespersample = (samplesize / 8) * numchannels;
} */
