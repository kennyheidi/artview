#pragma once
#include <stdint.h>

/* AAC (LC, HE-AAC/SBR) decoding of raw MP4 audio samples via the Helix decoder. */

#define AAC_OUT_MAX (2048 * 2)   /* max interleaved samples per frame (SBR doubles, stereo) */

typedef struct {
    void *h;
    int   channels;     /* from AudioSpecificConfig */
    int   rate;         /* core sample rate */
    int   aot;          /* audio object type */
} AacDec;

/* 0 on success. asc may be NULL/0 (then the fallbacks are used, LC assumed). */
int  aac_open(AacDec *d, const uint8_t *asc, int asc_len, int fallback_channels, int fallback_rate);

/* Decode one raw AAC frame into interleaved 16-bit PCM (out must hold AAC_OUT_MAX).
 * Returns samples per channel (0 on error) and the actual output channels/rate
 * (HE-AAC doubles the rate; mono stays mono). */
int  aac_decode(AacDec *d, const uint8_t *in, int len, int16_t *out, int *channels, int *rate);

void aac_reset(AacDec *d);    /* after seeking */
void aac_close(AacDec *d);
