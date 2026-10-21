#pragma once
#include <stdint.h>
#include <stdio.h>

/* Minimal MP4 (ISO BMFF) demuxer: one H.264 video track and one AAC audio
 * track, non-fragmented files, moov anywhere in the file. */

typedef struct {
    uint32_t off;          /* file offset (files < 4 GB) */
    uint32_t size;
    int64_t  dts, pts;     /* in the track's timescale; pts has the edit list applied */
    uint8_t  key;          /* sync sample (video) */
} Mp4Sample;

typedef struct {
    int       present;
    uint32_t  timescale;
    uint32_t  n;
    Mp4Sample *s;
    /* video */
    int       width, height, profile, level;
    int       nal_len;            /* NAL length field size from avcC (1, 2 or 4) */
    uint8_t  *params;             /* SPS+PPS as Annex-B with 3-byte start codes */
    uint32_t  params_len;
    /* audio */
    int       channels, rate;
    uint8_t   asc[64];            /* AudioSpecificConfig */
    int       asc_len;
} Mp4Track;

typedef struct {
    FILE     *f;
    uint32_t  fsize;
    Mp4Track  v, a;
    char      vcodec[8], acodec[8];   /* fourcc / description of what was found */
    double    duration;               /* seconds */
    char      err[96];
} Mp4;

/* 0 on success; on failure m->err explains (and m is cleaned up). A file with
 * unsupported audio still opens (a.present = 0, acodec says what it was). */
int  mp4_open(Mp4 *m, const char *path);
void mp4_close(Mp4 *m);

/* Read one sample's bytes into buf. Returns bytes read, -1 on error/too big. */
int  mp4_read(Mp4 *m, const Mp4Sample *s, uint8_t *buf, uint32_t cap);

/* Convert a length-prefixed H.264 sample (AVCC) to Annex-B with 3-byte
 * 00 00 01 start codes (what the 3DS hardware decoder takes). Returns the
 * output length, or -1 if it doesn't fit / is malformed. */
int  mp4_avcc_to_annexb(const uint8_t *in, uint32_t len, int nal_len, uint8_t *out, uint32_t cap);

/* Index of the last keyframe at or before time t (seconds), for seeking. */
uint32_t mp4_video_seek_index(const Mp4 *m, double t);
