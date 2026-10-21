#include "video.h"
#include "mp4.h"
#include "aac.h"
#include "image.h"
#include "log.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define VQ          4                    /* decoded frames queued for display */
#define IN_MAX      (2u * 1024 * 1024)   /* largest video sample */
#define A_NBUF      4                    /* NDSP buffers */
#define A_BUFSAMP   4096                 /* samples per channel per buffer */
#define A_CH        0                    /* NDSP channel */
/* Largest frame sent to the hardware decoder. MVD is documented for 720p-class
 * H.264; bigger streams can lock it up, so they're refused instead. */
#define MAX_PIXELS  (1280 * 736)
#define STOP_WAIT_NS (3000000000ULL)      /* how long vp_stop waits for the player thread */

/* The player thread didn't stop in time (decoder hung): it can't be reused or
 * freed, so playback stays off until the app restarts. Kept outside vp, which
 * vp_start clears. */
static bool vp_stuck;

static struct {
    volatile bool run;
    bool     active;
    Thread   vth, ath;
    char     path[256];
    int      max_w, max_h;

    volatile VpState state;
    char     msg[96];

    Mp4      m;                          /* opened by the video thread */
    double  *sorted_pts;                 /* video pts in display order, seconds */

    int      dw, dh;                     /* display frame size */
    volatile bool dims_ready;

    LightLock qlk;
    u16     *qbuf[VQ];
    double   qpts[VQ];
    int      qhead, qcount;
    volatile bool video_done;

    LightLock clk;                        /* clock + seek state */
    double   duration;
    bool     paused;
    u32      seek_gen;
    double   seek_to;
    double   tbase;                       /* timer clock: time at tms */
    u64      tms;
    bool     timer_running;
    volatile bool first_frame;            /* a frame is queued since start/seek */

    /* audio */
    volatile bool audio_on, audio_started;
    int      arate, ach;
    struct { u16 seq; s64 start; bool used; } ainfo[A_NBUF];
    double   last_atime;
} vp;

/* ---------------------------------------------------------------- clock */

static double timer_time(void)
{
    if (!vp.timer_running) return vp.tbase;
    return vp.tbase + (double)(osGetTime() - vp.tms) / 1000.0;
}

static double audio_time(void)
{
    u16 seq = ndspChnGetWaveBufSeq(A_CH);
    for (int i = 0; i < A_NBUF; i++)
        if (vp.ainfo[i].used && vp.ainfo[i].seq == seq && seq) {
            vp.last_atime = (double)(vp.ainfo[i].start + ndspChnGetSamplePos(A_CH)) / vp.arate;
            break;
        }
    return vp.last_atime;
}

/* Audio position is the master clock while sound is playing; the timer is kept
 * aligned to it and takes over when there's no sound (or it has run out). */
double vp_time(void)
{
    LightLock_Lock(&vp.clk);
    double t;
    if (vp.audio_on && vp.audio_started && ndspChnIsPlaying(A_CH) && !vp.paused) {
        t = audio_time();
        vp.tbase = t;
        vp.tms = osGetTime();
    } else {
        t = timer_time();
    }
    LightLock_Unlock(&vp.clk);
    return t;
}

static void timer_start_at(double t)    /* caller holds clk */
{
    vp.tbase = t;
    vp.tms = osGetTime();
    vp.timer_running = !vp.paused;
}

double vp_duration(void) { return vp.duration; }
bool   vp_paused(void)   { return vp.paused; }
bool   vp_has_audio(void){ return vp.audio_on; }
bool   vp_running(void)  { return vp.active; }

void vp_set_paused(bool p)
{
    LightLock_Lock(&vp.clk);
    if (p != vp.paused) {
        if (p) { vp.tbase = timer_time(); vp.timer_running = false; }
        else   { vp.tms = osGetTime(); vp.timer_running = vp.first_frame; }
        vp.paused = p;
        if (vp.audio_on) ndspChnSetPaused(A_CH, p);
    }
    LightLock_Unlock(&vp.clk);
}

void vp_seek(double t)
{
    if (t < 0) t = 0;
    if (vp.duration > 0 && t > vp.duration - 0.05) t = vp.duration - 0.05;
    LightLock_Lock(&vp.clk);
    vp.seek_to = t;
    vp.seek_gen++;
    vp.tbase = t;
    vp.timer_running = false;              /* restarts when the first new frame is queued */
    vp.first_frame = false;
    vp.audio_started = false;
    vp.last_atime = t;
    LightLock_Unlock(&vp.clk);
    LightLock_Lock(&vp.qlk);
    vp.qcount = 0;
    vp.video_done = false;
    LightLock_Unlock(&vp.qlk);
}

VpState vp_state(char *msg, size_t n)
{
    if (msg && n) snprintf(msg, n, "%s", vp.msg);
    return vp.state;
}

bool vp_dims(int *w, int *h)
{
    if (!vp.dims_ready) return false;
    *w = vp.dw; *h = vp.dh;
    return true;
}

bool vp_show_due(void (*upload)(const uint16_t *px, void *ud), void *ud)
{
    if (!vp.active || vp.state != VP_PLAYING) return false;
    double now = vp_time();
    bool shown = false;
    LightLock_Lock(&vp.qlk);
    while (vp.qcount >= 2 && vp.qpts[(vp.qhead + 1) % VQ] <= now) {   /* skip late frames */
        vp.qhead = (vp.qhead + 1) % VQ;
        vp.qcount--;
    }
    if (vp.qcount >= 1 && vp.qpts[vp.qhead] <= now + 0.005) {
        upload(vp.qbuf[vp.qhead], ud);
        vp.qhead = (vp.qhead + 1) % VQ;
        vp.qcount--;
        shown = true;
    }
    bool ended = vp.video_done && vp.qcount == 0;
    LightLock_Unlock(&vp.qlk);
    if (ended && !vp.paused && now >= vp.duration - 0.1) vp_seek(0);   /* loop */
    return shown;
}

/* ---------------------------------------------------------------- audio thread */

static void audio_thread(void *arg)
{
    (void)arg;
    Mp4 am = vp.m;                          /* shares the sample tables; own file handle */
    am.f = fopen(vp.path, "rb");
    AacDec d;
    u8 *in = malloc(64 * 1024);
    s16 *pcm = malloc(AAC_OUT_MAX * sizeof(s16));
    s16 *bufs[A_NBUF] = { 0 };
    ndspWaveBuf wb[A_NBUF];
    memset(wb, 0, sizeof wb);
    bool configured = false;
    bool ok = am.f && in && pcm && !aac_open(&d, am.a.asc, am.a.asc_len, am.a.channels, am.a.rate);
    if (!ok) { log_msg("video: audio decoder init failed"); vp.audio_on = false; goto out_nodec; }
    for (int i = 0; i < A_NBUF; i++) {
        bufs[i] = linearAlloc(A_BUFSAMP * 2 * sizeof(s16));
        if (!bufs[i]) { log_msg("video: out of linear memory for audio"); vp.audio_on = false; goto out; }
    }

    u32 my_gen = (u32)-1;
    u32 idx = 0;
    int pcm_n = 0, pcm_pos = 0, ch = 0, rate = 0;   /* leftover decoded frame */
    s64 tl = 0;                                     /* timeline position (out-rate samples) of pcm[pcm_pos] */
    s64 skip_until = 0;                             /* drop samples before this (priming / seek) */
    u32 decode_errors = 0, fails_in_row = 0;

    while (vp.run) {
        LightLock_Lock(&vp.clk);
        u32 gen = vp.seek_gen;
        double target = vp.seek_to;
        LightLock_Unlock(&vp.clk);
        if (gen != my_gen) {                       /* start / seek */
            my_gen = gen;
            if (configured) ndspChnWaveBufClear(A_CH);
            LightLock_Lock(&vp.clk);
            for (int i = 0; i < A_NBUF; i++) { wb[i].status = NDSP_WBUF_FREE; vp.ainfo[i].used = false; }
            LightLock_Unlock(&vp.clk);
            aac_reset(&d);
            idx = 0;
            s64 tts = (s64)(target * am.a.timescale);
            while (idx + 1 < am.a.n && am.a.s[idx + 1].pts <= tts) idx++;
            pcm_n = pcm_pos = 0;
            tl = -1;
            skip_until = 0;
            vp.audio_started = false;
        }
        if (vp.paused || !vp.first_frame) { svcSleepThread(5 * 1000000LL); continue; }

        int fb = -1;
        for (int i = 0; i < A_NBUF; i++)
            if (wb[i].status == NDSP_WBUF_FREE || wb[i].status == NDSP_WBUF_DONE) { fb = i; break; }
        if (fb < 0) { svcSleepThread(4 * 1000000LL); continue; }

        /* fill one buffer */
        int filled = 0;
        s64 start_tl = -1;
        while (filled < A_BUFSAMP && vp.run) {
            if (pcm_pos >= pcm_n) {
                if (idx >= am.a.n) break;          /* end of audio */
                int r = mp4_read(&am, &am.a.s[idx], in, 64 * 1024);
                double fpts = (double)am.a.s[idx].pts / am.a.timescale;
                idx++;
                int n = r > 0 ? aac_decode(&d, in, r, pcm, &ch, &rate) : 0;
                if (!n) {
                    if (decode_errors++ < 5) log_msg("video: AAC frame %u failed", idx - 1);
                    if (++fails_in_row >= 64) {           /* this audio can't be decoded: stop trying */
                        log_msg("video: giving up on the audio after %u bad frames", fails_in_row);
                        snprintf(vp.msg, sizeof vp.msg, "no sound: couldn't decode this video's audio");
                        vp.audio_on = false;
                        goto out;
                    }
                    if ((fails_in_row & 7) == 0) svcSleepThread(1000000LL);   /* let other threads run */
                    continue;
                }
                fails_in_row = 0;
                if (!configured) {
                    vp.arate = rate; vp.ach = ch;
                    ndspChnReset(A_CH);
                    ndspChnSetInterp(A_CH, NDSP_INTERP_LINEAR);
                    ndspChnSetRate(A_CH, (float)rate);
                    ndspChnSetFormat(A_CH, ch == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
                    float mix[12] = { 1.0f, 1.0f };
                    ndspChnSetMix(A_CH, mix);
                    configured = true;
                    log_msg("video: audio %d Hz, %d ch (AAC object type %d)", rate, ch, d.aot);
                }
                if (ch != vp.ach || rate != vp.arate) continue;   /* format change mid-stream: skip */
                pcm_n = n; pcm_pos = 0;
                tl = (s64)(fpts * rate);
                if (!skip_until) skip_until = (s64)(target * rate);
                if (skip_until < 0) skip_until = 0;
            }
            if (tl < skip_until) {                  /* encoder priming / before the seek point */
                int drop = (int)(skip_until - tl);
                if (drop > pcm_n - pcm_pos) drop = pcm_n - pcm_pos;
                pcm_pos += drop; tl += drop;
                continue;
            }
            int take = pcm_n - pcm_pos;
            if (take > A_BUFSAMP - filled) take = A_BUFSAMP - filled;
            if (start_tl < 0) start_tl = tl;
            memcpy(bufs[fb] + filled * vp.ach, pcm + pcm_pos * vp.ach, take * vp.ach * sizeof(s16));
            filled += take; pcm_pos += take; tl += take;
        }
        if (filled == 0) { svcSleepThread(10 * 1000000LL); continue; }   /* end of audio: idle */

        LightLock_Lock(&vp.clk);
        bool stale = gen != vp.seek_gen;
        LightLock_Unlock(&vp.clk);
        if (stale) continue;
        wb[fb].data_vaddr = bufs[fb];
        wb[fb].nsamples = filled;
        wb[fb].looping = false;
        DSP_FlushDataCache(bufs[fb], filled * vp.ach * sizeof(s16));
        ndspChnWaveBufAdd(A_CH, &wb[fb]);
        LightLock_Lock(&vp.clk);
        vp.ainfo[fb].seq = wb[fb].sequence_id;
        vp.ainfo[fb].start = start_tl;
        vp.ainfo[fb].used = true;
        if (!vp.audio_started) { vp.audio_started = true; vp.last_atime = (double)start_tl / vp.arate; }
        LightLock_Unlock(&vp.clk);
    }
out:
    if (configured) ndspChnWaveBufClear(A_CH);
    for (int i = 0; i < A_NBUF; i++) if (bufs[i]) linearFree(bufs[i]);
    aac_close(&d);
out_nodec:
    free(in);
    free(pcm);
    if (am.f) fclose(am.f);
}

/* ---------------------------------------------------------------- video thread */

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vp.msg, sizeof vp.msg, fmt, ap);
    va_end(ap);
    log_msg("video: %s", vp.msg);
    vp.state = VP_ERROR;
}

/* RGB565 cw-stride source (visible w x h) -> dw x dh, 2x2-sampled box filter */
static void scale565(const u16 *src, int stride, int w, int h, u16 *dst, int dw, int dh)
{
    u32 xstep = ((u32)w << 16) / dw, ystep = ((u32)h << 16) / dh;
    bool box = w >= dw * 3 / 2;
    u32 sy = ystep / 2 - (box ? ystep / 4 : 0);
    for (int y = 0; y < dh; y++, sy += ystep) {
        int y0 = sy >> 16, y1 = box ? (int)((sy + ystep / 2) >> 16) : y0;
        if (y1 >= h) y1 = h - 1;
        const u16 *r0 = src + (size_t)y0 * stride, *r1 = src + (size_t)y1 * stride;
        u32 sx = xstep / 2 - (box ? xstep / 4 : 0);
        for (int x = 0; x < dw; x++, sx += xstep) {
            int x0 = sx >> 16;
            if (!box) { *dst++ = r0[x0]; continue; }
            int x1 = (sx + xstep / 2) >> 16;
            if (x1 >= w) x1 = w - 1;
            u32 a = r0[x0], b = r0[x1], c = r1[x0], e = r1[x1];
            u32 rr = ((a >> 11) + (b >> 11) + (c >> 11) + (e >> 11) + 2) >> 2;
            u32 gg = (((a >> 5) & 63) + ((b >> 5) & 63) + ((c >> 5) & 63) + ((e >> 5) & 63) + 2) >> 2;
            u32 bb = ((a & 31) + (b & 31) + (c & 31) + (e & 31) + 2) >> 2;
            *dst++ = (u16)(rr << 11 | gg << 5 | bb);
        }
    }
}

static bool mvd_setup(MVDSTD_Config *cfg, int cw, int ch, u16 *out, int level)
{
    u32 work = MVD_DEFAULT_WORKBUF_SIZE;
    MVDSTD_CalculateWorkBufSizeConfig calc;
    memset(&calc, 0, sizeof calc);
    calc.level.enable = 1;
    calc.level.flag = MVD_CALC_WITH_LEVEL_FLAG_ENABLE_CALC | MVD_CALC_WITH_LEVEL_FLAG_ENABLE_EXTRA_OP;
    int lv = level >= 52 ? MVD_H264_LEVEL_5_2 : level >= 51 ? MVD_H264_LEVEL_5_1 : level >= 50 ? MVD_H264_LEVEL_5_0 :
             level >= 42 ? MVD_H264_LEVEL_4_2 : level >= 41 ? MVD_H264_LEVEL_4_1 : level >= 40 ? MVD_H264_LEVEL_4_0 :
             level >= 32 ? MVD_H264_LEVEL_3_2 : level >= 31 ? MVD_H264_LEVEL_3_1 : MVD_H264_LEVEL_3_0;
    calc.level.level = lv;
    calc.width = cw;
    calc.height = ch;
    u32 sz = 0;
    if (R_SUCCEEDED(mvdstdCalculateBufferSize(&calc, &sz)) && sz > work) work = sz;
    Result r = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565, work, NULL);
    if (R_FAILED(r)) {
        fail("MP4 needs a New 3DS (hardware video decoder unavailable: 0x%08lX)", (unsigned long)r);
        return false;
    }
    mvdstdGenerateDefaultConfig(cfg, cw, ch, cw, ch, NULL, (u32 *)out, (u32 *)out);
    log_msg("video: MVD ready, %dx%d, work buffer %lu KB", cw, ch, (unsigned long)(work / 1024));
    return true;
}

/* Feed one Annex-B NAL (start code included) to MVD. Returns the MVD status. */
static Result feed_nal(u8 *lin, const u8 *nal, u32 len)
{
    lin[0] = 0; lin[1] = 0; lin[2] = 1;
    memcpy(lin + 3, nal, len);
    GSPGPU_FlushDataCache(lin, len + 3);
    MVDSTD_ProcessNALUnitOut o;
    return mvdstdProcessVideoFrame(lin, len + 3, 0, &o);
}

static void feed_params(u8 *lin)
{
    const u8 *p = vp.m.v.params, *end = p + vp.m.v.params_len;
    while (p + 3 <= end) {
        p += 3;                                    /* our own 00 00 01 */
        const u8 *q = p;
        while (q + 3 <= end && !(q[0] == 0 && q[1] == 0 && q[2] == 1)) q++;
        if (q + 3 > end) q = end;
        feed_nal(lin, p, (u32)(q - p));
        p = q;
    }
}

static void video_thread(void *arg)
{
    (void)arg;
    u8 *sample = NULL, *lin = NULL;
    u16 *out = NULL;
    bool mvd = false, audio_thread_started = false;

    if (mp4_open(&vp.m, vp.path)) { fail("%s", vp.m.err); return; }
    Mp4Track *v = &vp.m.v;
    int w = v->width, h = v->height;
    int cw = (w + 15) & ~15, chh = (h + 15) & ~15;
    vp.duration = vp.m.duration;
    log_msg("video: %s %dx%d H.264 profile %d level %d.%d, %u frames, %.1f s, audio: %s",
            vp.path, w, h, v->profile, v->level / 10, v->level % 10, (unsigned)v->n, vp.duration,
            vp.m.a.present ? "AAC" : vp.m.acodec[0] ? vp.m.acodec : "none");
    if (w < 16 || h < 16 || cw > 4672 || chh > 4672) { fail("unsupported video size %dx%d", w, h); goto out; }
    if ((long)cw * chh > MAX_PIXELS) {
        fail("%dx%d is too big for the 3DS video decoder (720p max)", w, h);
        goto out;
    }

    vp.sorted_pts = malloc(v->n * sizeof(double));
    sample = malloc(IN_MAX);
    lin = linearMemAlign(IN_MAX + 64, 0x40);
    out = linearMemAlign((size_t)cw * chh * 2, 0x40);
    image_fit_dims(w, h, vp.max_w, vp.max_h, &vp.dw, &vp.dh);
    for (int i = 0; i < VQ; i++) vp.qbuf[i] = malloc((size_t)vp.dw * vp.dh * 2);
    if (!vp.sorted_pts || !sample || !lin || !out || !vp.qbuf[VQ - 1]) { fail("out of memory (%d KB frame)", cw * chh * 2 / 1024); goto out; }
    for (u32 i = 0; i < v->n; i++) vp.sorted_pts[i] = (double)v->s[i].pts / v->timescale;
    qsort(vp.sorted_pts, v->n, sizeof(double), cmp_double);

    MVDSTD_Config cfg;
    if (!(mvd = mvd_setup(&cfg, cw, chh, out, v->level))) goto out;
    if (v->level > 32) log_msg("video: level %d.%d is above what MVD is documented to handle (3.2); trying anyway",
                               v->level / 10, v->level % 10);

    if (vp.m.a.present) {
        Result r = ndspInit();
        if (R_FAILED(r)) {
            snprintf(vp.msg, sizeof vp.msg, "no sound: DSP firmware missing (sdmc:/3ds/dspfirm.cdc)");
            log_msg("video: ndspInit failed 0x%08lX - %s", (unsigned long)r, vp.msg);
        } else {
            vp.audio_on = true;
            s32 prio = 0x30;
            svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
            /* below the UI thread, so a busy decoder can never freeze the screen;
             * 4 buffers of ~90 ms ride out the frames the UI is drawing */
            vp.ath = threadCreate(audio_thread, NULL, 32 * 1024, prio + 1, -2, false);
            if (vp.ath) audio_thread_started = true;
            else { vp.audio_on = false; ndspExit(); log_msg("video: audio thread failed"); }
        }
    } else if (vp.m.acodec[0]) {
        snprintf(vp.msg, sizeof vp.msg, "no sound: %s audio isn't supported (AAC only)", vp.m.acodec);
    }

    vp.dims_ready = true;
    vp.state = VP_PLAYING;

    u32 my_gen = (u32)-1, idx = 0, out_i = 0, nal_errors = 0, frames = 0;
    u64 t0 = osGetTime();
    while (vp.run) {
        LightLock_Lock(&vp.clk);
        u32 gen = vp.seek_gen;
        double target = vp.seek_to;
        LightLock_Unlock(&vp.clk);
        if (gen != my_gen) {                           /* start / seek: restart from a keyframe */
            if (my_gen != (u32)-1) {
                mvdstdExit();
                if (!(mvd = mvd_setup(&cfg, cw, chh, out, v->level))) goto out;
            }
            my_gen = gen;
            idx = mp4_video_seek_index(&vp.m, target);
            double kp = (double)v->s[idx].pts / v->timescale;
            out_i = 0;
            while (out_i < v->n && vp.sorted_pts[out_i] < kp - 1e-6) out_i++;
            feed_params(lin);
        }
        if (idx >= v->n) {                             /* end: wait for loop/seek */
            if (!vp.video_done) {
                vp.video_done = true;
                if (frames) log_msg("video: decoded %u frames in %lu ms", (unsigned)frames, (unsigned long)(osGetTime() - t0));
            }
            svcSleepThread(10 * 1000000LL);
            continue;
        }
        int r = mp4_read(&vp.m, &v->s[idx], sample, IN_MAX);
        idx++;
        if (r < 0) { if (nal_errors++ < 5) log_msg("video: can't read sample %u", idx - 1); continue; }

        bool picture = false;
        for (u32 i = 0; i + v->nal_len <= (u32)r; ) {  /* each length-prefixed NAL */
            u32 n = 0;
            for (int k = 0; k < v->nal_len; k++) n = n << 8 | sample[i + k];
            i += v->nal_len;
            if (n == 0 || n > (u32)r - i) break;
            int type = sample[i] & 31;
            if (type != 12) {                          /* skip filler data */
                Result st = feed_nal(lin, sample + i, n);
                if (!MVD_CHECKNALUPROC_SUCCESS(st)) {
                    if (nal_errors++ < 5) log_msg("video: MVD rejected NAL type %d in frame %u: 0x%08lX", type, idx - 1, (unsigned long)st);
                } else if (st != MVD_STATUS_PARAMSET && st != MVD_STATUS_INCOMPLETEPROCESSING) {
                    picture = true;
                }
            }
            i += n;
        }
        if (!picture) continue;
        cfg.physaddr_outdata0 = osConvertVirtToPhys(out);
        Result rr = mvdstdRenderVideoFrame(&cfg, true);
        if (rr != MVD_STATUS_OK) { if (nal_errors++ < 5) log_msg("video: render failed 0x%08lX", (unsigned long)rr); continue; }
        GSPGPU_InvalidateDataCache(out, (u32)cw * chh * 2);

        /* wait for room in the display queue */
        while (vp.run) {
            LightLock_Lock(&vp.qlk);
            bool room = vp.qcount < VQ;
            LightLock_Unlock(&vp.qlk);
            LightLock_Lock(&vp.clk);
            bool stale = gen != vp.seek_gen;
            LightLock_Unlock(&vp.clk);
            if (room || stale) break;
            svcSleepThread(3 * 1000000LL);
        }
        LightLock_Lock(&vp.clk);
        bool stale = gen != vp.seek_gen;
        LightLock_Unlock(&vp.clk);
        if (stale || !vp.run) continue;

        double pts = out_i < v->n ? vp.sorted_pts[out_i] : vp.duration;
        out_i++;
        if (pts < target - 0.02) continue;             /* before the seek point: decoded, not shown */
        LightLock_Lock(&vp.qlk);
        int slot = (vp.qhead + vp.qcount) % VQ;
        scale565(out, cw, w, h, vp.qbuf[slot], vp.dw, vp.dh);
        vp.qpts[slot] = pts;
        vp.qcount++;
        LightLock_Unlock(&vp.qlk);
        frames++;
        if (!vp.first_frame) {                         /* start the clock with the first picture */
            LightLock_Lock(&vp.clk);
            if (gen == vp.seek_gen) { vp.first_frame = true; timer_start_at(pts); vp.last_atime = pts; }
            LightLock_Unlock(&vp.clk);
        }
    }
out:
    if (audio_thread_started) {
        threadJoin(vp.ath, U64_MAX);
        threadFree(vp.ath);
        vp.ath = NULL;
        ndspExit();
    }
    vp.audio_on = false;
    if (mvd) mvdstdExit();
    free(sample);
    if (lin) linearFree(lin);
    if (out) linearFree(out);
}

/* ---------------------------------------------------------------- start / stop */

bool vp_start(const char *path, int max_w, int max_h)
{
    if (vp_stuck) {
        snprintf(vp.msg, sizeof vp.msg, "the video decoder stopped responding - restart the app to play videos");
        vp.state = VP_ERROR;
        return false;
    }
    if (vp.active) vp_stop();
    memset(&vp, 0, sizeof vp);
    snprintf(vp.path, sizeof vp.path, "%s", path);
    vp.max_w = max_w;
    vp.max_h = max_h;
    LightLock_Init(&vp.qlk);
    LightLock_Init(&vp.clk);
    vp.run = true;
    vp.state = VP_STARTING;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    vp.vth = threadCreate(video_thread, NULL, 64 * 1024, prio + 1, 2, false);   /* New 3DS core 2 */
    if (!vp.vth) vp.vth = threadCreate(video_thread, NULL, 64 * 1024, prio + 1, -2, false);
    if (!vp.vth) { snprintf(vp.msg, sizeof vp.msg, "couldn't start the player"); vp.state = VP_ERROR; return false; }
    vp.active = true;
    return true;
}

void vp_stop(void)
{
    if (!vp.active) return;
    vp.run = false;
    if (R_FAILED(threadJoin(vp.vth, STOP_WAIT_NS))) {
        /* stuck inside the hardware decoder: leave it (and its buffers) alone
         * rather than freezing the app waiting for it */
        log_msg("video: player thread didn't stop within %llu ms (decoder hung); video disabled until restart",
                STOP_WAIT_NS / 1000000ULL);
        vp_stuck = true;
        vp.active = false;
        vp.state = VP_IDLE;
        vp.dims_ready = false;
        return;
    }
    threadFree(vp.vth);
    for (int i = 0; i < VQ; i++) free(vp.qbuf[i]);
    free(vp.sorted_pts);
    mp4_close(&vp.m);
    vp.active = false;
    vp.state = VP_IDLE;
    vp.dims_ready = false;
}
