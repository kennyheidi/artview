#pragma once
#include <stdint.h>
#include <stddef.h>

/* Decodes a JPEG/PNG/GIF(first frame)/BMP from memory and box-downscales it to fit
 * within maxw x maxh (aspect preserved, never upscaled). Returns malloc'd RGBA8 or NULL. */
uint8_t *image_decode_fit(const uint8_t *data, size_t len, int maxw, int maxh,
                          int *out_w, int *out_h, const char **err);

/* Box-filter downscale of an RGBA8 buffer. Returns malloc'd buffer. */
uint8_t *image_downscale(const uint8_t *src, int sw, int sh, int dw, int dh);

/* Size an image of w x h gets when fitted within maxw x maxh (never upscaled).
 * image_decode_fit uses exactly this, so callers can predict its output size. */
void image_fit_dims(int w, int h, int maxw, int maxh, int *dw, int *dh);

/* Animated GIF, decoded one frame at a time. The data buffer must stay alive
 * until gif_close. gif_next: 1 = frame (RGBA w*h, valid until the next gif_* call;
 * delay in milliseconds), 0 = end of animation, -1 = error. */
typedef struct GifStream GifStream;
GifStream *gif_open(const uint8_t *data, size_t len, int *w, int *h);
int  gif_next(GifStream *gs, const uint8_t **rgba, int *delay_ms);
void gif_rewind(GifStream *gs);
void gif_close(GifStream *gs);
/* Number of frames (stops counting at max). 0 if not a GIF. */
int  gif_frame_count(const uint8_t *data, size_t len, int max);
