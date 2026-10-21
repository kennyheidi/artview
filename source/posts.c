#include "posts.h"
#include "json.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

static const char *skip_ws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static void put_utf8(char **o, char *oe, unsigned cp)
{
    char tmp[4]; int n;
    if (cp < 0x80)       { tmp[0] = cp; n = 1; }
    else if (cp < 0x800) { tmp[0] = 0xC0 | (cp >> 6); tmp[1] = 0x80 | (cp & 0x3F); n = 2; }
    else                 { tmp[0] = 0xE0 | (cp >> 12); tmp[1] = 0x80 | ((cp >> 6) & 0x3F); tmp[2] = 0x80 | (cp & 0x3F); n = 3; }
    for (int i = 0; i < n && *o < oe; i++) *(*o)++ = tmp[i];
}

/* p must point at '"'. Writes the unescaped string (truncated) to out if out != NULL.
 * Returns pointer just past the closing quote, or NULL on malformed input. */
static const char *parse_string(const char *p, const char *e, char *out, size_t n)
{
    char *o = out, *oe = out ? out + n - 1 : NULL;
    if (p >= e || *p != '"') return NULL;
    p++;
    while (p < e && *p != '"') {
        char c = *p++;
        if (c == '\\' && p < e) {
            char esc = *p++;
            switch (esc) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 0; i < 4 && p < e; i++, p++) {
                    char h = *p; cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= h - '0';
                    else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                }
                if (out) put_utf8(&o, oe, cp);
                continue;
            }
            default: c = esc; break; /* \" \\ \/ */
            }
        }
        if (out && o < oe) *o++ = c;
    }
    if (out) *o = 0;
    if (p >= e) return NULL;
    return p + 1;
}

static const char *skip_value(const char *p, const char *e)
{
    p = skip_ws(p, e);
    if (p >= e) return NULL;
    if (*p == '"') return parse_string(p, e, NULL, 0);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < e) {
            if (*p == '"') { p = parse_string(p, e, NULL, 0); if (!p) return NULL; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { depth--; if (depth == 0) return p + 1; }
            p++;
        }
        return NULL;
    }
    /* number / true / false / null */
    while (p < e && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;
    return p;
}

static void set_field(Post *po, const char *key, const char *sval, long nval, int is_num)
{
    long v = is_num ? nval : strtol(sval, NULL, 10);
#define S(name) if (!strcmp(key, #name)) { if (!is_num) { strncpy(po->name, sval, sizeof po->name - 1); po->name[sizeof po->name - 1] = 0; } return; }
    S(preview_url) S(sample_url) S(file_url) S(tags) S(rating)
#undef S
    if (!strcmp(key, "id"))     { po->id = (unsigned)v; return; }
    if (!strcmp(key, "width"))  { po->width = (int)v; return; }
    if (!strcmp(key, "height")) { po->height = (int)v; return; }
    if (!strcmp(key, "score"))  { po->score = (int)v; return; }
}

int parse_posts(const char *json, size_t len, Post *out, int max)
{
    const char *p = json, *e = json + len;
    static char sval[1024];
    p = skip_ws(p, e);
    if (p >= e) return 0;               /* empty body = no results */
    if (*p != '[') return -1;
    p++;
    int count = 0;
    for (;;) {
        p = skip_ws(p, e);
        if (p >= e) return count;
        if (*p == ']') return count;
        if (*p == ',') { p++; continue; }
        if (*p != '{') return count;
        p++;
        Post tmp; memset(&tmp, 0, sizeof tmp);
        for (;;) {
            char key[32];
            p = skip_ws(p, e);
            if (p >= e) return count;
            if (*p == '}') { p++; break; }
            if (*p == ',') { p++; continue; }
            p = parse_string(p, e, key, sizeof key);
            if (!p) return count;
            p = skip_ws(p, e);
            if (p >= e || *p != ':') return count;
            p = skip_ws(p + 1, e);
            if (p >= e) return count;
            if (*p == '"') {
                p = parse_string(p, e, sval, sizeof sval);
                if (!p) return count;
                set_field(&tmp, key, sval, 0, 0);
            } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
                char *endp;
                long v = strtol(p, &endp, 10);
                set_field(&tmp, key, "", v, 1);
                p = skip_value(endp, e);
                if (!p) return count;
            } else {
                p = skip_value(p, e);
                if (!p) return count;
            }
        }
        if (count < max) out[count++] = tmp;
    }
}

const char *url_ext(const char *url)
{
    static char ring[16][8];
    static volatile unsigned next;
    char *buf = ring[__sync_fetch_and_add(&next, 1) & 15];
    size_t end = strcspn(url, "?#");                /* ignore query strings (DeviantArt tokens) */
    const char *slash = NULL, *dot = NULL;
    for (size_t i = 0; i < end; i++) {
        if (url[i] == '/') { slash = url + i; dot = NULL; }
        else if (url[i] == '.') dot = url + i;
    }
    (void)slash;
    size_t n = dot ? (size_t)(url + end - dot) : 0;
    if (!dot || n < 2 || n > 6) return ".bin";
    memcpy(buf, dot, n);
    buf[n] = 0;
    return buf;
}
int url_is_video(const char *url)
{
    const char *x = url_ext(url);
    return !strncmp(x, ".mp4", 4) || !strncmp(x, ".webm", 5);
}

/* ---------------------------------------------------------------- Derpibooru (Philomena) */

static const char *expect(const char *p, const char *e, char c)
{
    p = skip_ws(p, e);
    return (p < e && *p == c) ? p + 1 : NULL;
}

/* Replace the file name at the end of a URL ("/.../thumb.webm" -> "/.../thumb.gif"). */
static void swap_name(char *url, size_t n, const char *newname)
{
    char *slash = strrchr(url, '/');
    if (!slash) return;
    size_t keep = (size_t)(slash + 1 - url);
    if (keep + strlen(newname) + 1 > n) return;
    strcpy(slash + 1, newname);
}

static void swap_ext(char *url, size_t n, const char *from, const char *to)
{
    size_t ul = strlen(url), fl = strlen(from), tl = strlen(to);
    if (ul < fl || strcmp(url + ul - fl, from) || ul - fl + tl + 1 > n) return;
    strcpy(url + ul - fl, to);
}

static const char *rating_tags[] = { "explicit", "questionable", "suggestive", "safe",
                                     "grimdark", "semi-grimdark", "grotesque", NULL };

/* One image object. Returns pointer after it, or NULL. *ok = 1 if usable. */
static const char *derpi_image(const char *p, const char *e, Post *po, int *ok)
{
    char key[32], val[1024];
    char r_thumb[256] = "", r_medium[256] = "", r_large[256] = "", r_tall[256] = "", r_full[256] = "";
    char format[16] = "", view[256] = "";
    int hidden = 0;
    memset(po, 0, sizeof *po);
    *ok = 0;
    if (!(p = expect(p, e, '{'))) return NULL;
    for (;;) {
        p = skip_ws(p, e);
        if (p >= e) return NULL;
        if (*p == '}') { p++; break; }
        if (*p == ',') { p++; continue; }
        if (!(p = parse_string(p, e, key, sizeof key))) return NULL;
        if (!(p = expect(p, e, ':'))) return NULL;
        p = skip_ws(p, e);
        if (p >= e) return NULL;
        if (!strcmp(key, "tags") && *p == '[') {       /* ["safe","twilight sparkle",...] */
            p++;
            size_t o = 0;
            for (;;) {
                p = skip_ws(p, e);
                if (p >= e) return NULL;
                if (*p == ']') { p++; break; }
                if (*p == ',') { p++; continue; }
                if (*p != '"') { if (!(p = skip_value(p, e))) return NULL; continue; }
                if (!(p = parse_string(p, e, val, sizeof val))) return NULL;
                for (char *c = val; *c; c++) if (*c == ' ') *c = '_';
                if (!po->rating[0])
                    for (int i = 0; rating_tags[i]; i++)
                        if (!strcmp(val, rating_tags[i])) snprintf(po->rating, sizeof po->rating, "%s", val);
                size_t vl = strlen(val);
                if (o + vl + 2 < sizeof po->tags) {
                    if (o) po->tags[o++] = ' ';
                    memcpy(po->tags + o, val, vl);
                    o += vl;
                    po->tags[o] = 0;
                }
            }
            continue;
        }
        if (!strcmp(key, "representations") && *p == '{') {
            p++;
            for (;;) {
                char rk[24];
                p = skip_ws(p, e);
                if (p >= e) return NULL;
                if (*p == '}') { p++; break; }
                if (*p == ',') { p++; continue; }
                if (!(p = parse_string(p, e, rk, sizeof rk))) return NULL;
                if (!(p = expect(p, e, ':'))) return NULL;
                p = skip_ws(p, e);
                if (p < e && *p == '"') {
                    if (!(p = parse_string(p, e, val, sizeof val))) return NULL;
                    char *dst = !strcmp(rk, "thumb") ? r_thumb : !strcmp(rk, "medium") ? r_medium :
                                !strcmp(rk, "large") ? r_large : !strcmp(rk, "tall") ? r_tall :
                                !strcmp(rk, "full") ? r_full : NULL;
                    if (dst) snprintf(dst, 256, "%s", val);
                } else if (!(p = skip_value(p, e))) return NULL;
            }
            continue;
        }
        if (*p == '"') {
            if (!(p = parse_string(p, e, val, sizeof val))) return NULL;
            if (!strcmp(key, "format"))        snprintf(format, sizeof format, "%s", val);
            else if (!strcmp(key, "view_url")) snprintf(view, sizeof view, "%s", val);
            continue;
        }
        if (*p == '-' || (*p >= '0' && *p <= '9')) {
            char *endp;
            long v = strtol(p, &endp, 10);
            if (!strcmp(key, "id")) po->id = (unsigned)v;
            else if (!strcmp(key, "width")) po->width = (int)v;
            else if (!strcmp(key, "height")) po->height = (int)v;
            else if (!strcmp(key, "score")) po->score = (int)v;
            if (!(p = skip_value(endp, e))) return NULL;
            continue;
        }
        if (!strcmp(key, "hidden_from_users") && !strncmp(p, "true", 4)) hidden = 1;
        if (!(p = skip_value(p, e))) return NULL;
    }
    if (hidden || !po->id || !r_thumb[0]) return p;        /* deleted / not processed yet */

    const char *big = (po->height > po->width * 2 && r_tall[0]) ? r_tall : r_large[0] ? r_large : r_full;
    snprintf(po->preview_url, sizeof po->preview_url, "%s", r_thumb);
    snprintf(po->sample_url, sizeof po->sample_url, "%s", big);
    snprintf(po->file_url, sizeof po->file_url, "%s", view[0] ? view : r_full);
    if (!strcmp(format, "webm")) {
        /* Video: thumbnails are WebM clips too; the server also makes a GIF of the
         * small thumbnails, a PNG still (rendered.png) and an H.264 MP4 of every
         * size. Save the MP4 (medium = up to 800x600, easy for the New 3DS). */
        swap_ext(po->preview_url, sizeof po->preview_url, ".webm", ".gif");
        snprintf(po->sample_url, sizeof po->sample_url, "%s", r_thumb);
        swap_name(po->sample_url, sizeof po->sample_url, "rendered.png");
        snprintf(po->file_url, sizeof po->file_url, "%s", r_medium[0] ? r_medium : r_full);
        swap_ext(po->file_url, sizeof po->file_url, ".webm", ".mp4");
    } else if (!strcmp(format, "svg")) {
        /* SVGs: save the server's PNG rendering instead */
        snprintf(po->file_url, sizeof po->file_url, "%s", r_large[0] ? r_large : r_full);
    }
    *ok = 1;
    return p;
}

int parse_posts_derpi(const char *json, size_t len, Post *out, int max, int *total)
{
    const char *p = json, *e = json + len;
    char key[32];
    int count = 0, seen_images = 0;
    if (total) *total = -1;
    if (!(p = expect(p, e, '{'))) return -1;
    for (;;) {
        p = skip_ws(p, e);
        if (p >= e) break;
        if (*p == '}') break;
        if (*p == ',') { p++; continue; }
        if (!(p = parse_string(p, e, key, sizeof key))) break;
        if (!(p = expect(p, e, ':'))) break;
        p = skip_ws(p, e);
        if (!strcmp(key, "images") && p < e && *p == '[') {
            seen_images = 1;
            p++;
            for (;;) {
                p = skip_ws(p, e);
                if (p >= e) break;
                if (*p == ']') { p++; break; }
                if (*p == ',') { p++; continue; }
                Post tmp;
                int ok;
                const char *q = derpi_image(p, e, &tmp, &ok);
                if (!q) { p = NULL; break; }
                p = q;
                if (ok && count < max) out[count++] = tmp;
            }
            if (!p) break;
            continue;
        }
        if (!strcmp(key, "total") && p < e && *p >= '0' && *p <= '9') {
            char *endp;
            long v = strtol(p, &endp, 10);
            if (total) *total = (int)v;
            p = endp;
            continue;
        }
        if (!(p = skip_value(p, e))) break;
    }
    return seen_images ? count : -1;
}

/* ---------------------------------------------------------------- DeviantArt */

static void remove_blur(char *url)                  /* drop ",blur_NN" from wixmp image URLs */
{
    /* Signed links (?token=...) must stay exactly as given: the CDN answers an
     * edited one with HTTP 401. */
    if (strstr(url, "token=")) return;
    char *b = strstr(url, ",blur_");
    if (!b) return;
    char *e = b + 6;
    while (*e >= '0' && *e <= '9') e++;
    memmove(b, e, strlen(e) + 1);
}

/* Copies a URL string; a link too long for the buffer is dropped, never cut off. */
static void url_str(const Json *j, int node, char *out, size_t n)
{
    json_str(j, node, out, n);
    if (strlen(out) >= n - 1) out[0] = 0;
}

static unsigned id_from_url(const char *url, const char *fallback)
{
    /* https://www.deviantart.com/artist/art/Some-Title-123456789 */
    size_t n = strcspn(url, "?#");
    size_t i = n;
    while (i && url[i - 1] >= '0' && url[i - 1] <= '9') i--;
    if (i < n && n - i <= 10) {
        unsigned long v = strtoul(url + i, NULL, 10);
        if (v && v <= 0xFFFFFFFFul) return (unsigned)v;
    }
    unsigned h = 2166136261u;                       /* no number: hash the deviation UUID */
    for (const char *c = fallback; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    return h | 0x80000000u;
}

int parse_posts_da(const char *json, size_t len, Post *out, int max,
                   int *has_more, int *next_offset, char *next_cursor, size_t cursor_n,
                   char *err, size_t errn)
{
    Json j;
    if (has_more) *has_more = 0;
    if (next_offset) *next_offset = -1;
    if (next_cursor && cursor_n) next_cursor[0] = 0;
    if (err && errn) err[0] = 0;
    if (json_parse(&j, json, len)) return -1;
    if (j.n[0].type != J_OBJ) { json_free(&j); return -1; }
    int e = json_get(&j, 0, "error");
    if (e >= 0) {                                    /* {"error":"...","error_description":"..."} */
        if (err && errn && !json_str(&j, json_get(&j, 0, "error_description"), err, errn)) json_str(&j, e, err, errn);
        json_free(&j);
        return -1;
    }
    int res = json_get(&j, 0, "results");
    if (res < 0 || j.n[res].type != J_ARR) { json_free(&j); return -1; }
    if (has_more) *has_more = json_bool(&j, json_get(&j, 0, "has_more"), 0);
    if (next_offset) *next_offset = (int)json_num(&j, json_get(&j, 0, "next_offset"), -1);
    if (next_cursor && cursor_n) json_str(&j, json_get(&j, 0, "next_cursor"), next_cursor, cursor_n);

    int count = 0, nres = json_len(&j, res);
    for (int i = 0; i < nres && count < max; i++) {
        int d = json_at(&j, res, i);
        if (json_bool(&j, json_get(&j, d, "is_deleted"), 0)) continue;
        Post *p = &out[count];
        memset(p, 0, sizeof *p);
        char url[256], uuid[64], user[48];
        json_str(&j, json_get(&j, d, "url"), url, sizeof url);
        json_str(&j, json_get(&j, d, "deviationid"), uuid, sizeof uuid);
        json_str(&j, json_path(&j, d, "author", "username", NULL), user, sizeof user);
        json_str(&j, json_get(&j, d, "title"), p->title, sizeof p->title);
        p->id = id_from_url(url, uuid);
        p->score = (int)json_num(&j, json_path(&j, d, "stats", "favourites", NULL), 0);
        int mature = json_bool(&j, json_get(&j, d, "is_mature"), 0);
        if (mature) snprintf(p->rating, sizeof p->rating, "mature");
        snprintf(p->tags, sizeof p->tags, "by:%s%s", user[0] ? user : "unknown", mature ? " mature" : "");

        int content = json_get(&j, d, "content");
        int preview = json_get(&j, d, "preview");
        int thumbs = json_get(&j, d, "thumbs");
        int videos = json_get(&j, d, "videos");

        /* grid thumbnail: the largest thumb no wider than 300px, else the first */
        int best = -1, bw = 0;
        for (int t = 0; t < json_len(&j, thumbs); t++) {
            int th = json_at(&j, thumbs, t);
            int w = (int)json_num(&j, json_get(&j, th, "width"), 0);
            if (best < 0 || (w <= 300 && w > bw)) { best = th; bw = w; }
        }
        if (best >= 0) url_str(&j, json_get(&j, best, "src"), p->preview_url, sizeof p->preview_url);

        if (content >= 0) {
            url_str(&j, json_get(&j, content, "src"), p->file_url, sizeof p->file_url);
            p->width = (int)json_num(&j, json_get(&j, content, "width"), 0);
            p->height = (int)json_num(&j, json_get(&j, content, "height"), 0);
        }
        if (preview >= 0) url_str(&j, json_get(&j, preview, "src"), p->sample_url, sizeof p->sample_url);
        if (!p->sample_url[0]) snprintf(p->sample_url, sizeof p->sample_url, "%s", p->file_url);

        /* film: best MP4 up to 720p (what the New 3DS decoder handles); else the smallest */
        if (json_len(&j, videos) > 0) {
            int pick = -1, pq = 0, small = -1, sq = 1 << 30;
            for (int v = 0; v < json_len(&j, videos); v++) {
                int vn = json_at(&j, videos, v);
                char q[16];
                json_str(&j, json_get(&j, vn, "quality"), q, sizeof q);
                int qv = atoi(q);
                if (qv <= 720 && qv > pq) { pick = vn; pq = qv; }
                if (qv < sq) { small = vn; sq = qv; }
            }
            if (pick < 0) pick = small;
            url_str(&j, json_get(&j, pick, "src"), p->file_url, sizeof p->file_url);
            if (!p->sample_url[0] || !strcmp(p->sample_url, p->file_url))
                snprintf(p->sample_url, sizeof p->sample_url, "%s", p->preview_url);
        }
        if (!p->file_url[0] || !p->preview_url[0]) continue;      /* literature, flash, ... */
        remove_blur(p->preview_url);
        remove_blur(p->sample_url);
        remove_blur(p->file_url);
        count++;
    }
    json_free(&j);
    return count;
}

/* ---------------------------------------------------------------- e621 */

#define E6_MAX_PIXELS (1280 * 736)          /* what the New 3DS video decoder handles */

/* Best MP4 for the 3DS from sample.alternates: the largest 720p-or-smaller
 * sample, else the full-size MP4 variant if it's small enough. Returns 1 if found. */
static int e6_pick_mp4(const Json *j, int alts, char *url, size_t n)
{
    long best = -1;
    int samples = json_get(j, alts, "samples");
    if (samples >= 0 && j->n[samples].type == J_OBJ) {
        for (int c = j->n[samples].child; c >= 0; c = j->n[c].next) {
            long w = (long)json_num(j, json_get(j, c, "width"), 0);
            long h = (long)json_num(j, json_get(j, c, "height"), 0);
            if (h <= 0 && j->n[c].key >= 0) h = atol(j->text + j->n[c].key);      /* "720p" */
            if (w <= 0) w = h * 16 / 9;
            long px = w * h;
            if (px <= 0 || px > E6_MAX_PIXELS || px <= best) continue;
            char u[1536];
            json_str(j, json_get(j, c, "url"), u, sizeof u);
            if (!u[0] || strlen(u) >= sizeof u - 1) continue;
            snprintf(url, n, "%s", u);
            best = px;
        }
    }
    if (best > 0) return 1;
    int mp4 = json_path(j, alts, "variants", "mp4", NULL);
    if (mp4 >= 0) {
        long w = (long)json_num(j, json_get(j, mp4, "width"), 0);
        long h = (long)json_num(j, json_get(j, mp4, "height"), 0);
        if (w > 0 && h > 0 && w * h <= E6_MAX_PIXELS) {
            json_str(j, json_get(j, mp4, "url"), url, n);
            if (url[0]) return 1;
        }
    }
    return 0;
}

int parse_posts_e621(const char *json, size_t len, Post *out, int max, int *hidden,
                     char *err, size_t errn)
{
    static const char *cats[] = { "artist", "character", "copyright", "species", "general", "meta", "lore", "invalid" };
    Json j;
    if (hidden) *hidden = 0;
    if (err && errn) err[0] = 0;
    if (json_parse(&j, json, len)) return -1;
    if (j.n[0].type != J_OBJ) { json_free(&j); return -1; }
    int posts = json_get(&j, 0, "posts");
    if (posts < 0 || j.n[posts].type != J_ARR) {           /* {"success":false,"message"/"reason":...} */
        if (err && errn && !json_str(&j, json_get(&j, 0, "message"), err, errn))
            json_str(&j, json_get(&j, 0, "reason"), err, errn);
        json_free(&j);
        return -1;
    }
    int count = 0, np = json_len(&j, posts);
    for (int i = 0; i < np && count < max; i++) {
        int d = json_at(&j, posts, i);
        int file = json_get(&j, d, "file"), prev = json_get(&j, d, "preview"), samp = json_get(&j, d, "sample");
        Post *p = &out[count];
        memset(p, 0, sizeof *p);
        url_str(&j, json_get(&j, file, "url"), p->file_url, sizeof p->file_url);
        if (!p->file_url[0]) { if (hidden) (*hidden)++; continue; }   /* hidden from this account */
        p->id = (unsigned)json_num(&j, json_get(&j, d, "id"), 0);
        p->width = (int)json_num(&j, json_get(&j, file, "width"), 0);
        p->height = (int)json_num(&j, json_get(&j, file, "height"), 0);
        p->score = (int)json_num(&j, json_path(&j, d, "score", "total", NULL), 0);
        char r[8] = "";
        json_str(&j, json_get(&j, d, "rating"), r, sizeof r);
        snprintf(p->rating, sizeof p->rating, "%s", r[0] == 's' ? "safe" : r[0] == 'q' ? "questionable" : r[0] == 'e' ? "explicit" : r);
        url_str(&j, json_get(&j, prev, "url"), p->preview_url, sizeof p->preview_url);
        if (json_bool(&j, json_get(&j, samp, "has"), 0))
            url_str(&j, json_get(&j, samp, "url"), p->sample_url, sizeof p->sample_url);

        char ext[8] = "";
        json_str(&j, json_get(&j, file, "ext"), ext, sizeof ext);
        if (!strcmp(ext, "webm") || !strcmp(ext, "mp4")) {
            char mp4[1536] = "";
            int alts = json_get(&j, samp, "alternates");
            if (alts >= 0 && e6_pick_mp4(&j, alts, mp4, sizeof mp4))
                snprintf(p->file_url, sizeof p->file_url, "%s", mp4);
            if (!p->sample_url[0] || url_is_video(p->sample_url))
                snprintf(p->sample_url, sizeof p->sample_url, "%s", p->preview_url);
        } else if (!p->sample_url[0]) {
            snprintf(p->sample_url, sizeof p->sample_url, "%s", p->file_url);
        }
        if (!p->preview_url[0]) snprintf(p->preview_url, sizeof p->preview_url, "%s", p->sample_url);

        /* tags: every category, artists first */
        int tags = json_get(&j, d, "tags");
        size_t o = 0;
        for (size_t c = 0; c < sizeof cats / sizeof cats[0]; c++) {
            int arr = json_get(&j, tags, cats[c]);
            for (int t = 0; t < json_len(&j, arr); t++) {
                char tag[128];
                json_str(&j, json_at(&j, arr, t), tag, sizeof tag);
                if (!tag[0] || o + strlen(tag) + 2 >= sizeof p->tags) continue;
                o += snprintf(p->tags + o, sizeof p->tags - o, "%s%s", o ? " " : "", tag);
            }
        }
        count++;
    }
    json_free(&j);
    return count;
}

/* ---------------------------------------------------------------- pixiv */

static void str_replace1(char *s, size_t cap, const char *from, const char *to)
{
    char *p = strstr(s, from);
    if (!p) return;
    size_t fl = strlen(from), tl = strlen(to), rest = strlen(p + fl);
    if ((size_t)(p - s) + tl + rest + 1 > cap) return;
    memmove(p + tl, p + fl, rest + 1);
    memcpy(p, to, tl);
}

void pixiv_master_url(char *url)
{
    char *c = strstr(url, ".net/c/");                  /* drop the resize segment */
    if (c) {
        char *e = strchr(c + 7, '/');
        if (e) memmove(c + 5, e + 1, strlen(e + 1) + 1);          /* keep ".net/" */
    }
    size_t cap = strlen(url) + 16;                      /* callers' buffers are 1536 */
    str_replace1(url, cap, "/custom-thumb/", "/img-master/");
    str_replace1(url, cap, "_square1200.", "_master1200.");
    str_replace1(url, cap, "_custom1200.", "_master1200.");
}

int parse_posts_pixiv(const char *json, size_t len, int kind, int skip, Post *out, int max,
                      int *total, int *more, char *err, size_t errn)
{
    Json j;
    if (total) *total = -1;
    if (more) *more = 0;
    if (err && errn) err[0] = 0;
    if (json_parse(&j, json, len)) return -1;
    if (j.n[0].type != J_OBJ) { json_free(&j); return -1; }
    int list = -1;
    if (kind == 2) {
        list = json_get(&j, 0, "contents");
        int next = json_get(&j, 0, "next");
        if (more) *more = next >= 0 && j.n[next].type == J_NUM;
        if (list < 0 && err && errn) json_str(&j, json_get(&j, 0, "error"), err, errn);
    } else {
        if (json_bool(&j, json_get(&j, 0, "error"), 0)) {
            if (err && errn) json_str(&j, json_get(&j, 0, "message"), err, errn);
            if (err && errn && !err[0]) snprintf(err, errn, "pixiv returned an error");
            json_free(&j);
            return -1;
        }
        int body = json_get(&j, 0, "body");
        int box = kind == 0 ? json_get(&j, body, "illustManga") : body;
        list = json_get(&j, box, kind == 0 ? "data" : "works");
        if (total) {
            int t = json_get(&j, box, "total");
            if (t >= 0) *total = (int)json_num(&j, t, -1);
        }
    }
    if (list < 0 || j.n[list].type != J_ARR) { json_free(&j); return -1; }

    int count = 0, seen = 0, n = json_len(&j, list);
    for (int i = 0; i < n && count < max; i++) {
        int d = json_at(&j, list, i);
        if (json_bool(&j, json_get(&j, d, "isAdContainer"), 0)) continue;
        Post *p = &out[count];
        memset(p, 0, sizeof *p);
        url_str(&j, json_get(&j, d, "url"), p->preview_url, sizeof p->preview_url);
        if (!p->preview_url[0] || strstr(p->preview_url, "/common/images/")) continue;   /* masked / deleted */
        if (seen++ < skip) continue;
        int idn = json_get(&j, d, kind == 2 ? "illust_id" : "id");
        p->id = (unsigned)json_num(&j, idn, 0);           /* json_num reads numeric strings too */
        p->width = (int)json_num(&j, json_get(&j, d, "width"), 0);
        p->height = (int)json_num(&j, json_get(&j, d, "height"), 0);
        char title[64] = "", user[48] = "";
        json_str(&j, json_get(&j, d, "title"), title, sizeof title);
        json_str(&j, json_get(&j, d, kind == 2 ? "user_name" : "userName"), user, sizeof user);
        int pages = (int)json_num(&j, json_get(&j, d, kind == 2 ? "illust_page_count" : "pageCount"), 1);
        snprintf(p->title, sizeof p->title, "%.56s%s%.32s", title, user[0] ? "  by " : "", user);
        int xr = (int)json_num(&j, json_get(&j, d, kind == 2 ? "illust_content_type" : "xRestrict"), 0);
        if (kind == 2) {                                     /* ranking: sexual >= 2 means R-18 */
            int ct = json_get(&j, d, "illust_content_type");
            xr = ct >= 0 ? (int)json_num(&j, json_get(&j, ct, "sexual"), 0) >= 2 : 0;
        }
        snprintf(p->rating, sizeof p->rating, "%s", xr == 2 ? "R-18G" : xr ? "R-18" : "all ages");
        snprintf(p->sample_url, sizeof p->sample_url, "%s", p->preview_url);
        pixiv_master_url(p->sample_url);
        snprintf(p->file_url, sizeof p->file_url, "%s", p->sample_url);
        int tags = json_get(&j, d, "tags");
        size_t o = 0;
        if (pages > 1) o += snprintf(p->tags, sizeof p->tags, "%d_pages", pages);
        for (int t = 0; t < json_len(&j, tags); t++) {
            char tag[96];
            json_str(&j, json_at(&j, tags, t), tag, sizeof tag);
            for (char *c = tag; *c; c++) if (*c == ' ') *c = '_';
            if (!tag[0] || o + strlen(tag) + 2 >= sizeof p->tags) continue;
            o += snprintf(p->tags + o, sizeof p->tags - o, "%s%s", o ? " " : "", tag);
        }
        count++;
    }
    if (kind != 2 && more) *more = seen > skip + count;        /* items left in this reply */
    json_free(&j);
    return count;
}
