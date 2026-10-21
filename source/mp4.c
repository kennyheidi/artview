#include "mp4.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#define MOOV_MAX     (32u * 1024 * 1024)
#define MAX_SAMPLES  (4u * 1024 * 1024)

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

/* A child-box walker over an in-memory range. */
typedef struct { const uint8_t *p, *end; } Range;

static int next_box(Range *r, char type[5], Range *body)
{
    if (r->end - r->p < 8) return 0;
    uint64_t size = be32(r->p);
    memcpy(type, r->p + 4, 4); type[4] = 0;
    size_t hdr = 8;
    if (size == 1) {
        if (r->end - r->p < 16) return 0;
        size = be64(r->p + 8);
        hdr = 16;
    } else if (size == 0) {
        size = (uint64_t)(r->end - r->p);
    }
    if (size < hdr || size > (uint64_t)(r->end - r->p)) return 0;
    body->p = r->p + hdr;
    body->end = r->p + size;
    r->p += size;
    return 1;
}

static int find_box(Range r, const char *want, Range *out)
{
    char t[5]; Range b;
    while (next_box(&r, t, &b)) if (!memcmp(t, want, 4)) { *out = b; return 1; }
    return 0;
}

/* Track tables gathered while parsing, expanded into samples afterwards. */
typedef struct {
    char handler[5];
    uint32_t timescale;
    Range stsd, stts, ctts, stss, stsc, stsz, stco;
    int co64;
    int has_ctts, has_stss;
    int64_t edit_media_time;      /* first edit's media_time, -1 if none/empty */
} TrakInfo;

static int read_desc_len(const uint8_t **p, const uint8_t *end)
{
    int len = 0;
    for (int i = 0; i < 4 && *p < end; i++) {
        uint8_t b = *(*p)++;
        len = (len << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return len;
}

/* esds -> AudioSpecificConfig; returns object type indication or -1 */
static int parse_esds(Range esds, Mp4Track *a)
{
    const uint8_t *p = esds.p + 4, *end = esds.end;     /* skip version/flags */
    if (p >= end || *p++ != 0x03) return -1;
    read_desc_len(&p, end);
    if (end - p < 3) return -1;
    p += 2;                                             /* ES_ID */
    uint8_t fl = *p++;
    if (fl & 0x80) p += 2;
    if (fl & 0x40) { if (p >= end) return -1; p += 1 + *p; }
    if (fl & 0x20) p += 2;
    if (p >= end || *p++ != 0x04) return -1;
    read_desc_len(&p, end);
    if (end - p < 13) return -1;
    int oti = p[0];
    p += 13;
    if (p < end && *p == 0x05) {
        p++;
        int len = read_desc_len(&p, end);
        if (len > (int)sizeof a->asc) len = sizeof a->asc;
        if (len > end - p) len = (int)(end - p);
        memcpy(a->asc, p, len);
        a->asc_len = len;
    }
    return oti;
}

static void parse_trak(Range trak, TrakInfo *ti)
{
    memset(ti, 0, sizeof *ti);
    ti->edit_media_time = -1;
    Range edts, elst, mdia, mdhd, hdlr, minf, stbl;
    if (find_box(trak, "edts", &edts) && find_box(edts, "elst", &elst) && elst.end - elst.p >= 8) {
        int ver = elst.p[0];
        uint32_t cnt = be32(elst.p + 4);
        const uint8_t *e = elst.p + 8;
        for (uint32_t i = 0; i < cnt; i++) {
            int64_t mt;
            if (ver == 1) { if (elst.end - e < 20) break; mt = (int64_t)be64(e + 8); e += 20; }
            else          { if (elst.end - e < 12) break; mt = (int32_t)be32(e + 4); e += 12; }
            if (mt >= 0) { ti->edit_media_time = mt; break; }   /* skip empty edits (-1) */
        }
    }
    if (!find_box(trak, "mdia", &mdia)) return;
    if (find_box(mdia, "mdhd", &mdhd) && mdhd.end - mdhd.p >= 24)
        ti->timescale = mdhd.p[0] == 1 ? be32(mdhd.p + 20) : be32(mdhd.p + 12);
    if (find_box(mdia, "hdlr", &hdlr) && hdlr.end - hdlr.p >= 12) {
        memcpy(ti->handler, hdlr.p + 8, 4);
        ti->handler[4] = 0;
    }
    if (!find_box(mdia, "minf", &minf) || !find_box(minf, "stbl", &stbl)) return;
    find_box(stbl, "stsd", &ti->stsd);
    find_box(stbl, "stts", &ti->stts);
    ti->has_ctts = find_box(stbl, "ctts", &ti->ctts);
    ti->has_stss = find_box(stbl, "stss", &ti->stss);
    find_box(stbl, "stsc", &ti->stsc);
    if (!find_box(stbl, "stsz", &ti->stsz)) find_box(stbl, "stz2", &ti->stsz);
    if (find_box(stbl, "co64", &ti->stco)) ti->co64 = 1;
    else find_box(stbl, "stco", &ti->stco);
}

static int expand_samples(const TrakInfo *ti, Mp4Track *t, char *err, size_t errn)
{
    if (!ti->stsz.p || !ti->stsc.p || !ti->stco.p || !ti->stts.p) { snprintf(err, errn, "missing sample tables"); return -1; }
    if (ti->stsz.end - ti->stsz.p < 12) return -1;
    uint32_t fixed = be32(ti->stsz.p + 4), n = be32(ti->stsz.p + 8);
    if (n == 0 || n > MAX_SAMPLES) { snprintf(err, errn, "bad sample count %u", (unsigned)n); return -1; }
    if (!fixed && ti->stsz.end - (ti->stsz.p + 12) < (ptrdiff_t)n * 4) return -1;
    Mp4Sample *s = calloc(n, sizeof *s);
    if (!s) { snprintf(err, errn, "out of memory (%u samples)", (unsigned)n); return -1; }
    for (uint32_t i = 0; i < n; i++) s[i].size = fixed ? fixed : be32(ti->stsz.p + 12 + 4 * i);

    /* chunk offsets + sample-to-chunk -> sample offsets */
    uint32_t nchunks = be32(ti->stco.p + 4);
    uint32_t nstsc = be32(ti->stsc.p + 4);
    const uint8_t *co = ti->stco.p + 8, *sc = ti->stsc.p + 8;
    if (ti->stco.end - co < (ptrdiff_t)nchunks * (ti->co64 ? 8 : 4) || ti->stsc.end - sc < (ptrdiff_t)nstsc * 12) goto bad;
    uint32_t si = 0;
    for (uint32_t e = 0; e < nstsc && si < n; e++) {
        uint32_t first = be32(sc + e * 12), per = be32(sc + e * 12 + 4);
        uint32_t last = e + 1 < nstsc ? be32(sc + (e + 1) * 12) - 1 : nchunks;
        for (uint32_t c = first; c <= last && c >= 1 && c <= nchunks && si < n; c++) {
            uint64_t off = ti->co64 ? be64(co + (c - 1) * 8) : be32(co + (c - 1) * 4);
            for (uint32_t k = 0; k < per && si < n; k++) {
                if (off > 0xFFFFFFFFu) goto bad;
                s[si].off = (uint32_t)off;
                off += s[si].size;
                si++;
            }
        }
    }
    if (si != n) goto bad;

    /* decode times */
    uint32_t nstts = be32(ti->stts.p + 4);
    const uint8_t *st = ti->stts.p + 8;
    if (ti->stts.end - st < (ptrdiff_t)nstts * 8) goto bad;
    int64_t dts = 0;
    si = 0;
    for (uint32_t e = 0; e < nstts && si < n; e++) {
        uint32_t cnt = be32(st + e * 8), delta = be32(st + e * 8 + 4);
        for (uint32_t k = 0; k < cnt && si < n; k++) { s[si++].dts = dts; dts += delta; }
    }
    for (; si < n; si++) s[si].dts = dts;            /* malformed: pad */

    /* composition offsets */
    for (uint32_t i = 0; i < n; i++) s[i].pts = s[i].dts;
    if (ti->has_ctts && ti->ctts.end - ti->ctts.p >= 8) {
        int ver = ti->ctts.p[0];
        uint32_t nc = be32(ti->ctts.p + 4);
        const uint8_t *ct = ti->ctts.p + 8;
        if (ti->ctts.end - ct >= (ptrdiff_t)nc * 8) {
            si = 0;
            for (uint32_t e = 0; e < nc && si < n; e++) {
                uint32_t cnt = be32(ct + e * 8);
                int64_t o = ver ? (int64_t)(int32_t)be32(ct + e * 8 + 4) : (int64_t)be32(ct + e * 8 + 4);
                for (uint32_t k = 0; k < cnt && si < n; k++) s[si++].pts += o;
            }
        }
    }
    if (ti->edit_media_time > 0)
        for (uint32_t i = 0; i < n; i++) s[i].pts -= ti->edit_media_time;

    /* keyframes */
    if (ti->has_stss && ti->stss.end - ti->stss.p >= 8) {
        uint32_t nk = be32(ti->stss.p + 4);
        const uint8_t *k = ti->stss.p + 8;
        for (uint32_t i = 0; i < nk && ti->stss.end - (k + 4 * i) >= 4; i++) {
            uint32_t idx = be32(k + 4 * i);
            if (idx >= 1 && idx <= n) s[idx - 1].key = 1;
        }
    } else {
        for (uint32_t i = 0; i < n; i++) s[i].key = 1;
    }
    t->s = s;
    t->n = n;
    t->timescale = ti->timescale ? ti->timescale : 1;
    return 0;
bad:
    free(s);
    snprintf(err, errn, "corrupt sample tables");
    return -1;
}

static int parse_avcc(Range avcc, Mp4Track *v)
{
    const uint8_t *p = avcc.p, *end = avcc.end;
    if (end - p < 7) return -1;
    v->profile = p[1];
    v->level = p[3];
    v->nal_len = (p[4] & 3) + 1;
    if (v->nal_len == 3) return -1;
    size_t cap = (size_t)(end - p) + 64;
    v->params = malloc(cap);
    if (!v->params) return -1;
    uint32_t o = 0;
    p += 5;
    for (int pass = 0; pass < 2; pass++) {               /* SPS list, then PPS list */
        if (p >= end) break;
        int cnt = pass == 0 ? (*p++ & 31) : *p++;
        for (int i = 0; i < cnt; i++) {
            if (end - p < 2) return -1;
            uint16_t len = be16(p);
            p += 2;
            if (end - p < len || o + 3 + len > cap) return -1;
            v->params[o++] = 0; v->params[o++] = 0; v->params[o++] = 1;
            memcpy(v->params + o, p, len);
            o += len;
            p += len;
        }
    }
    v->params_len = o;
    return 0;
}

static void track_free(Mp4Track *t)
{
    free(t->s);
    free(t->params);
    memset(t, 0, sizeof *t);
}

void mp4_close(Mp4 *m)
{
    if (m->f) fclose(m->f);
    m->f = NULL;
    track_free(&m->v);
    track_free(&m->a);
}

int mp4_open(Mp4 *m, const char *path)
{
    memset(m, 0, sizeof *m);
    m->f = fopen(path, "rb");
    if (!m->f) { snprintf(m->err, sizeof m->err, "can't open file"); return -1; }
    fseek(m->f, 0, SEEK_END);
    long fs = ftell(m->f);
    m->fsize = fs > 0 ? (uint32_t)fs : 0;

    /* walk top-level boxes on disk to find moov */
    uint64_t pos = 0, moov_off = 0, moov_size = 0;
    int moof = 0;
    uint8_t h[16];
    while (pos + 8 <= m->fsize) {
        fseek(m->f, (long)pos, SEEK_SET);
        if (fread(h, 1, 16, m->f) < 8) break;
        uint64_t size = be32(h);
        int hdr = 8;
        if (size == 1) { size = be64(h + 8); hdr = 16; }
        else if (size == 0) size = m->fsize - pos;
        if (size < (uint64_t)hdr) break;
        if (!memcmp(h + 4, "moov", 4)) { moov_off = pos + hdr; moov_size = size - hdr; }
        if (!memcmp(h + 4, "moof", 4)) moof = 1;
        pos += size;
    }
    if (!moov_size) { snprintf(m->err, sizeof m->err, "not an MP4 (no moov box)"); mp4_close(m); return -1; }
    if (moov_size > MOOV_MAX) { snprintf(m->err, sizeof m->err, "MP4 index too large"); mp4_close(m); return -1; }
    uint8_t *moov = malloc((size_t)moov_size);
    if (!moov) { snprintf(m->err, sizeof m->err, "out of memory"); mp4_close(m); return -1; }
    fseek(m->f, (long)moov_off, SEEK_SET);
    if (fread(moov, 1, (size_t)moov_size, m->f) != moov_size) {
        free(moov); snprintf(m->err, sizeof m->err, "truncated MP4"); mp4_close(m); return -1;
    }
    Range root = { moov, moov + moov_size }, r = root, b;
    char t[5];
    while (next_box(&r, t, &b)) {
        if (memcmp(t, "trak", 4)) continue;
        TrakInfo ti;
        parse_trak(b, &ti);
        if (!ti.stsd.p || ti.stsd.end - ti.stsd.p < 8 + 8) continue;
        Range entries = { ti.stsd.p + 8, ti.stsd.end }, e;      /* skip ver/flags + count */
        char fourcc[5];
        if (!next_box(&entries, fourcc, &e)) continue;

        if (!strcmp(ti.handler, "vide") && !m->v.present) {
            snprintf(m->vcodec, sizeof m->vcodec, "%s", fourcc);
            if (strcmp(fourcc, "avc1") && strcmp(fourcc, "avc3")) continue;
            if (e.end - e.p < 78) continue;
            m->v.width = be16(e.p + 24);
            m->v.height = be16(e.p + 26);
            Range kids = { e.p + 78, e.end }, avcc;
            if (!find_box(kids, "avcC", &avcc) || parse_avcc(avcc, &m->v)) {
                snprintf(m->err, sizeof m->err, "bad H.264 config"); free(m->v.params); m->v.params = NULL; continue;
            }
            if (expand_samples(&ti, &m->v, m->err, sizeof m->err)) { free(m->v.params); m->v.params = NULL; continue; }
            m->v.present = 1;
        } else if (!strcmp(ti.handler, "soun") && !m->a.present) {
            snprintf(m->acodec, sizeof m->acodec, "%s", fourcc);
            if (strcmp(fourcc, "mp4a") || e.end - e.p < 28) continue;
            int ver = be16(e.p + 8);
            m->a.channels = be16(e.p + 16);
            m->a.rate = be32(e.p + 24) >> 16;
            const uint8_t *kp = e.p + 28 + (ver == 1 ? 16 : ver == 2 ? 36 : 0);
            if (kp > e.end) continue;
            Range kids = { kp, e.end }, esds;
            if (!find_box(kids, "esds", &esds)) continue;
            int oti = parse_esds(esds, &m->a);
            if (oti != 0x40 && oti != 0x67 && oti != 0x66) {     /* MPEG-4 / MPEG-2 AAC */
                snprintf(m->acodec, sizeof m->acodec, oti == 0x6B || oti == 0x69 ? "mp3" : "oti%02x", oti);
                continue;
            }
            snprintf(m->acodec, sizeof m->acodec, "aac");
            if (expand_samples(&ti, &m->a, m->err, sizeof m->err)) continue;
            m->a.present = 1;
        }
    }
    free(moov);
    if (!m->v.present) {
        if (moof)
            snprintf(m->err, sizeof m->err, "fragmented MP4 not supported");
        else if (m->vcodec[0] && strcmp(m->vcodec, "avc1") && strcmp(m->vcodec, "avc3"))
            snprintf(m->err, sizeof m->err, "video codec %s not supported (H.264 only)", m->vcodec);
        else if (!m->err[0])
            snprintf(m->err, sizeof m->err, "no video track");
        mp4_close(m);
        return -1;
    }
    m->err[0] = 0;
    const Mp4Sample *last = &m->v.s[m->v.n - 1];
    m->duration = (double)(last->pts > last->dts ? last->pts : last->dts) / m->v.timescale;
    if (m->v.n > 1) m->duration += (double)(m->v.s[1].dts - m->v.s[0].dts) / m->v.timescale;
    return 0;
}

int mp4_read(Mp4 *m, const Mp4Sample *s, uint8_t *buf, uint32_t cap)
{
    if (s->size > cap || (uint64_t)s->off + s->size > m->fsize) return -1;
    if (fseek(m->f, (long)s->off, SEEK_SET)) return -1;
    if (fread(buf, 1, s->size, m->f) != s->size) return -1;
    return (int)s->size;
}

int mp4_avcc_to_annexb(const uint8_t *in, uint32_t len, int nal_len, uint8_t *out, uint32_t cap)
{
    uint32_t i = 0, o = 0;
    while (i + nal_len <= len) {
        uint32_t n = 0;
        for (int k = 0; k < nal_len; k++) n = n << 8 | in[i + k];
        i += nal_len;
        if (n == 0) continue;
        if (n > len - i || o + 3 + n > cap) return -1;
        out[o++] = 0; out[o++] = 0; out[o++] = 1;
        memcpy(out + o, in + i, n);
        o += n;
        i += n;
    }
    return (int)o;
}

uint32_t mp4_video_seek_index(const Mp4 *m, double t)
{
    int64_t target = (int64_t)(t * m->v.timescale);
    uint32_t best = 0;
    for (uint32_t i = 0; i < m->v.n; i++) {
        if (!m->v.s[i].key) continue;
        if (m->v.s[i].pts <= target) best = i;
        else break;
    }
    return best;
}
