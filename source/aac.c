#include "aac.h"
#include "helix-aac/aacdec.h"
#include <string.h>

static const int rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };

typedef struct { const uint8_t *p; int len, bit; } Bits;

static unsigned getbits(Bits *b, int n)
{
    unsigned v = 0;
    while (n--) {
        int byte = b->bit >> 3;
        unsigned bit = byte < b->len ? (b->p[byte] >> (7 - (b->bit & 7))) & 1 : 0;
        v = v << 1 | bit;
        b->bit++;
    }
    return v;
}

static int set_params(AacDec *d)
{
    AACFrameInfo fi;
    memset(&fi, 0, sizeof fi);
    fi.nChans = d->channels;
    fi.sampRateCore = d->rate;
    fi.profile = AAC_PROFILE_LC;
    return AACSetRawBlockParams(d->h, 0, &fi);
}

int aac_open(AacDec *d, const uint8_t *asc, int asc_len, int fallback_channels, int fallback_rate)
{
    memset(d, 0, sizeof *d);
    d->channels = fallback_channels > 0 ? fallback_channels : 2;
    d->rate = fallback_rate > 0 ? fallback_rate : 44100;
    d->aot = 2;
    if (asc && asc_len >= 2) {
        Bits b = { asc, asc_len, 0 };
        int aot = getbits(&b, 5);
        if (aot == 31) aot = 32 + getbits(&b, 6);
        int fi = getbits(&b, 4);
        int rate = fi == 15 ? (int)getbits(&b, 24) : fi < 13 ? rates[fi] : 0;
        int ch = getbits(&b, 4);
        if (aot == 5 || aot == 29) {            /* explicit SBR/PS: core is the next AOT */
            int ei = getbits(&b, 4);
            if (ei == 15) getbits(&b, 24);
            aot = getbits(&b, 5);
        }
        d->aot = aot;
        if (rate) d->rate = rate;
        if (ch >= 1 && ch <= 2) d->channels = ch;
    }
    if (d->aot != 2 && d->aot != 1 && d->aot != 4) return -1;   /* LC (and Main/LTP decoded as LC) */
    d->h = AACInitDecoder();
    if (!d->h) return -1;
    if (set_params(d)) { aac_close(d); return -1; }
    return 0;
}

int aac_decode(AacDec *d, const uint8_t *in, int len, int16_t *out, int *channels, int *rate)
{
    unsigned char *p = (unsigned char *)in;
    int left = len;
    if (AACDecode(d->h, &p, &left, (short *)out)) return 0;
    AACFrameInfo fi;
    AACGetLastFrameInfo(d->h, &fi);
    if (fi.nChans < 1) return 0;
    *channels = fi.nChans;
    *rate = fi.sampRateOut;
    return fi.outputSamps / fi.nChans;
}

void aac_reset(AacDec *d)
{
    if (!d->h) return;
    AACFlushCodec(d->h);
    set_params(d);
}

void aac_close(AacDec *d)
{
    if (d->h) AACFreeDecoder(d->h);
    d->h = NULL;
}
