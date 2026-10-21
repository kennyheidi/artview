#include "qrscan.h"
#include "quirc/quirc.h"   /* quirc v1.1: v1.2+ fails to pair finder patterns on tilted codes */
#include <string.h>

struct quirc *qrscan_new(int w, int h)
{
    struct quirc *q = quirc_new();
    if (!q) return NULL;
    if (quirc_resize(q, w, h) < 0) { quirc_destroy(q); return NULL; }
    return q;
}

/* Transpose the module grid: decodes codes seen mirrored (quirc v1.1 has no quirc_flip). */
static void flip_code(struct quirc_code *c)
{
    struct quirc_code t = *c;
    memset(c->cell_bitmap, 0, sizeof c->cell_bitmap);
    for (int y = 0; y < c->size; y++)
        for (int x = 0; x < c->size; x++) {
            int src = y * c->size + x, dst = x * c->size + y;
            if (t.cell_bitmap[src >> 3] & (1 << (src & 7)))
                c->cell_bitmap[dst >> 3] |= (uint8_t)(1 << (dst & 7));
        }
}

void qrscan_free(struct quirc *q)
{
    if (q) quirc_destroy(q);
}

int qrscan_rgb565(struct quirc *q, const uint16_t *px, int w, int h, char *out, size_t n, int *seen)
{
    int qw, qh;
    uint8_t *g = quirc_begin(q, &qw, &qh);
    if (qw != w || qh != h) { quirc_end(q); *seen = 0; return 0; }
    for (int i = 0; i < w * h; i++) {
        uint16_t c = px[i];
        unsigned r = (c >> 11) & 0x1F, gr = (c >> 5) & 0x3F, b = c & 0x1F;
        g[i] = (uint8_t)((r * 8 * 77 + gr * 4 * 150 + b * 8 * 29) >> 8);
    }
    quirc_end(q);

    int count = quirc_count(q);
    *seen = count;
    for (int i = 0; i < count; i++) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(q, i, &code);
        quirc_decode_error_t err = quirc_decode(&code, &data);
        if (err == QUIRC_ERROR_DATA_ECC) {       /* mirrored code: try flipped */
            flip_code(&code);
            err = quirc_decode(&code, &data);
        }
        if (err) continue;
        size_t len = (size_t)data.payload_len < n - 1 ? (size_t)data.payload_len : n - 1;
        memcpy(out, data.payload, len);
        out[len] = 0;
        return 1;
    }
    return 0;
}
