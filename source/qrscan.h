#pragma once
#include <stddef.h>
#include <stdint.h>

struct quirc;

struct quirc *qrscan_new(int w, int h);
void qrscan_free(struct quirc *q);

/* Converts an RGB565 frame (the 3DS camera's output format) to grayscale and
 * looks for QR codes. On success copies the first decoded payload into out
 * (NUL-terminated, truncated to n) and returns 1. Returns 0 if none decoded;
 * *seen is set to the number of QR-like codes found (to show "hold steady"). */
int qrscan_rgb565(struct quirc *q, const uint16_t *px, int w, int h, char *out, size_t n, int *seen);
