#include "image.h"
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#include "stb_image.h"

/* Decoding needs w*h*4 bytes at full size; keep well inside N3DS app memory. */
#define MAX_DECODE_PIXELS (12u * 1000u * 1000u)

uint8_t *image_downscale(const uint8_t *src, int sw, int sh, int dw, int dh)
{
    uint8_t *dst = malloc((size_t)dw * dh * 4);
    if (!dst) return NULL;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long long)y * sh / dh);
        int y1 = (int)((long long)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long long)x * sw / dw);
            int x1 = (int)((long long)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t *p = src + ((size_t)yy * sw + x0) * 4;
                for (int xx = x0; xx < x1; xx++, p += 4) {
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]; n++;
                }
            }
            uint8_t *d = dst + ((size_t)y * dw + x) * 4;
            d[0] = r / n; d[1] = g / n; d[2] = b / n; d[3] = a / n;
        }
    }
    return dst;
}

void image_fit_dims(int w, int h, int maxw, int maxh, int *dw, int *dh)
{
    if (w <= maxw && h <= maxh) { *dw = w; *dh = h; return; }
    float s = (float)maxw / w;
    if ((float)maxh / h < s) s = (float)maxh / h;
    *dw = (int)(w * s);
    *dh = (int)(h * s);
    if (*dw < 1) *dw = 1;
    if (*dh < 1) *dh = 1;
}

uint8_t *image_decode_fit(const uint8_t *data, size_t len, int maxw, int maxh,
                          int *out_w, int *out_h, const char **err)
{
    int w, h, comp;
    *err = NULL;
    if (!stbi_info_from_memory(data, (int)len, &w, &h, &comp)) {
        *err = "unsupported image format";
        return NULL;
    }
    if ((unsigned)w * (unsigned)h > MAX_DECODE_PIXELS) {
        *err = "image too large to decode";
        return NULL;
    }
    uint8_t *px = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
    if (!px) {
        *err = stbi_failure_reason();
        return NULL;
    }
    if (w <= maxw && h <= maxh) {
        *out_w = w; *out_h = h;
        return px;
    }
    int dw, dh;
    image_fit_dims(w, h, maxw, maxh, &dw, &dh);
    uint8_t *small = image_downscale(px, w, h, dw, dh);
    stbi_image_free(px);
    if (!small) { *err = "out of memory"; return NULL; }
    *out_w = dw; *out_h = dh;
    return small;
}

/* ---------------------------------------------------------------- animated GIF
 * Frame-by-frame decoding on top of stb_image's internal GIF decoder, so only one
 * canvas is in memory however long the GIF is. For disposal method 3 ("restore to
 * previous") stb needs the canvas as it was before the previous frame was drawn;
 * that is exactly stb's own g.background at disposal time, so we pass that (stb's
 * all-frames loader passes a pointer computed from the wrong frame). */

struct GifStream {
    const uint8_t *data;
    size_t len;
    stbi__context s;
    stbi__gif g;
    int w, h, frames_done;
};

static void gif_free_bufs(GifStream *gs)
{
    STBI_FREE(gs->g.out);
    STBI_FREE(gs->g.history);
    STBI_FREE(gs->g.background);
    memset(&gs->g, 0, sizeof gs->g);
}

GifStream *gif_open(const uint8_t *data, size_t len, int *w, int *h)
{
    int x, y, comp;
    if (!stbi_info_from_memory(data, (int)len, &x, &y, &comp)) return NULL;
    stbi__context t;
    stbi__start_mem(&t, data, (int)len);
    if (!stbi__gif_test(&t)) return NULL;
    if ((unsigned)x * (unsigned)y > MAX_DECODE_PIXELS / 2) return NULL;
    GifStream *gs = calloc(1, sizeof *gs);
    if (!gs) return NULL;
    gs->data = data;
    gs->len = len;
    gs->w = x;
    gs->h = y;
    stbi__start_mem(&gs->s, data, (int)len);
    if (w) *w = x;
    if (h) *h = y;
    return gs;
}

int gif_next(GifStream *gs, const uint8_t **rgba, int *delay_ms)
{
    int comp;
    stbi_uc *u = stbi__gif_load_next(&gs->s, &gs->g, &comp, 4, gs->g.background);
    if (u == (stbi_uc *)&gs->s) return 0;          /* end of animation */
    if (!u) return gs->frames_done ? 0 : -1;       /* damaged tail after some frames: treat as end */
    if (gs->g.w != gs->w || gs->g.h != gs->h) return -1;
    gs->frames_done++;
    *rgba = u;
    *delay_ms = gs->g.delay;
    return 1;
}

void gif_rewind(GifStream *gs)
{
    gif_free_bufs(gs);
    stbi__start_mem(&gs->s, gs->data, (int)gs->len);
    gs->frames_done = 0;
}

void gif_close(GifStream *gs)
{
    if (!gs) return;
    gif_free_bufs(gs);
    free(gs);
}

int gif_frame_count(const uint8_t *data, size_t len, int max)
{
    GifStream *gs = gif_open(data, len, NULL, NULL);
    if (!gs) return 0;
    int n = 0, delay;
    const uint8_t *px;
    while (n < max && gif_next(gs, &px, &delay) == 1) n++;
    gif_close(gs);
    return n;
}
