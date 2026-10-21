#include "net.h"
#include "log.h"
#include "site.h"
#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOC_ALIGN   0x1000
#define SOC_BUFSIZE 0x100000
#define USER_AGENT  BUILD_NAME "-3ds/1.2 (New Nintendo 3DS homebrew)"
#define CA_PATH     "romfs:/cacert.pem"

static u32 *soc_buf;
static bool soc_up;
static CURL *curl;
static int g_verify = 1;
static char errbuf[CURL_ERROR_SIZE];

typedef struct { char *data; size_t len, cap, max; } MemBuf;
typedef struct { net_abort_fn fn; void *ud; } AbortCtx;

int net_init(int verify_ssl, char *err, size_t errn)
{
    log_msg("net: %s", curl_version());
    soc_buf = memalign(SOC_ALIGN, SOC_BUFSIZE);
    Result r = soc_buf ? socInit(soc_buf, SOC_BUFSIZE) : -1;
    if (R_FAILED(r)) {
        snprintf(err, errn, "socInit failed (0x%08lX)", (unsigned long)r);
        log_msg("net: %s", err);
        return -1;
    }
    soc_up = true;
    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    if (!curl) { snprintf(err, errn, "curl_easy_init failed"); log_msg("net: %s", err); return -1; }

    g_verify = verify_ssl;
    if (g_verify) {
        FILE *f = fopen(CA_PATH, "rb");
        if (!f) {
            snprintf(err, errn, "missing %s - rebuild with cacert.pem in romfs/", CA_PATH);
            log_msg("net: %s", err);
            return -1;
        }
        fseek(f, 0, SEEK_END);
        log_msg("net: CA bundle %ld bytes", ftell(f));
        fclose(f);
    } else {
        log_msg("net: WARNING certificate verification disabled");
    }
    log_msg("net: ready");
    return 0;
}

void net_exit(void)
{
    if (curl) curl_easy_cleanup(curl);
    curl = NULL;
    curl_global_cleanup();
    if (soc_up) socExit();
    soc_up = false;
    free(soc_buf);
    soc_buf = NULL;
}

char *net_escape(const char *s)
{
    char *e = curl ? curl_easy_escape(curl, s, 0) : NULL;
    char *r = strdup(e ? e : "");
    curl_free(e);
    return r;
}

static size_t mem_write(char *ptr, size_t sz, size_t nm, void *ud)
{
    MemBuf *b = ud;
    size_t n = sz * nm;
    if (b->len + n + 1 > b->max) return 0;          /* too big: abort */
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64 * 1024;
        while (nc < b->len + n + 1) nc *= 2;
        if (nc > b->max) nc = b->max;
        char *nd = realloc(b->data, nc);
        if (!nd) return 0;
        b->data = nd; b->cap = nc;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = 0;
    return n;
}

static int xferinfo(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un)
{
    (void)dt; (void)dn; (void)ut; (void)un;
    AbortCtx *a = ud;
    return a->fn && a->fn(a->ud);
}

/* verbose_log=1: curl's connection/TLS/header trace, one log line per line */
static int debug_cb(CURL *h, curl_infotype t, char *data, size_t n, void *ud)
{
    (void)h; (void)ud;
    const char *tag = t == CURLINFO_TEXT ? "*" : t == CURLINFO_HEADER_IN ? "<" : t == CURLINFO_HEADER_OUT ? ">" : NULL;
    if (!tag) return 0;
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && data[j] != '\n') j++;
        size_t len = j - i;
        if (len && data[i + len - 1] == '\r') len--;
        if (len) log_msg("curl %s %.*s", tag, (int)(len > 400 ? 400 : len), data + i);
        i = j + 1;
    }
    return 0;
}

static void common_opts(const char *url, AbortCtx *ac)
{
    curl_easy_reset(curl);
    errbuf[0] = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    /* pixiv: its image server only answers requests that come "from" pixiv.net,
     * and the site wants a browser user agent (this is the New 3DS browser's) */
    if (strstr(url, "pximg.net/") || strstr(url, "://www.pixiv.net/")) {
        curl_easy_setopt(curl, CURLOPT_REFERER, "https://www.pixiv.net/");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (New Nintendo 3DS like iPhone) AppleWebKit/536.30 "
                         "(KHTML, like Gecko) NX/3.0.0.5.21 Mobile NF/3.0.0.5.21 " BUILD_NAME "/1.3");
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 256L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");   /* gzip if available */
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, ac);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 64L * 1024);
    if (g_verify) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, CA_PATH);
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (log_verbose()) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, debug_cb);
    }
}

static int finish(const char *url, CURLcode rc, size_t bytes, u64 t0, char *err, size_t errn)
{
    long code = 0;
    char *ctype = NULL;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ctype);
    unsigned long ms = (unsigned long)(osGetTime() - t0);
    if (rc != CURLE_OK) {
        if (rc == CURLE_ABORTED_BY_CALLBACK) snprintf(err, errn, "cancelled");
        else snprintf(err, errn, "%s", errbuf[0] ? errbuf : curl_easy_strerror(rc));
        log_msg("GET %s -> curl error %d (%s) after %lu ms, HTTP %ld", url, (int)rc, err, ms, code);
        return -1;
    }
    log_msg("GET %s -> HTTP %ld, %u bytes, %s, %lu ms", url, code, (unsigned)bytes,
            ctype ? ctype : "?", ms);
    if (code >= 400) {
        snprintf(err, errn, "HTTP %ld", code);
        return -1;
    }
    return 0;
}

int net_request(const char *url, NetReq *rq, char **out, size_t *outlen, size_t maxlen,
                net_abort_fn ab, void *ud, char *err, size_t errn)
{
    if (!curl) { snprintf(err, errn, "network unavailable"); return -1; }
    MemBuf b = { NULL, 0, 0, maxlen };
    AbortCtx ac = { ab, ud };
    struct curl_slist *hdrs = NULL;
    u64 t0 = osGetTime();
    common_opts(url, &ac);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    if (rq) {
        rq->http = 0;
        if (rq->post) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, rq->post);
        if (rq->cookie) curl_easy_setopt(curl, CURLOPT_COOKIE, rq->cookie);
        if (rq->userpwd) {
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
            curl_easy_setopt(curl, CURLOPT_USERPWD, rq->userpwd);
        }
        for (int i = 0; i < 2; i++)
            if (rq->header[i]) hdrs = curl_slist_append(hdrs, rq->header[i]);
        if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    }
    CURLcode rc = curl_easy_perform(curl);
    if (hdrs) curl_slist_free_all(hdrs);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (rq) rq->http = code;
    if (rc == CURLE_WRITE_ERROR) {
        snprintf(err, errn, "response too large (limit %u KB)", (unsigned)(maxlen / 1024));
        log_msg("%s %s -> %s", rq && rq->post ? "POST" : "GET", url, err);
        free(b.data);
        return -1;
    }
    if (finish(url, rc, b.len, t0, err, errn)) {
        if (b.data && b.len) log_msg("  body: %.200s", b.data);
        if (rc == CURLE_OK && rq && rq->keep_error_body && b.data) {   /* caller parses the error JSON */
            *out = b.data;
            *outlen = b.len;
            return -1;
        }
        free(b.data);
        return -1;
    }
    if (!b.data) b.data = calloc(1, 1);
    *out = b.data;
    *outlen = b.len;
    return 0;
}

int net_get(const char *url, char **out, size_t *outlen, size_t maxlen,
            net_abort_fn ab, void *ud, char *err, size_t errn)
{
    return net_request(url, NULL, out, outlen, maxlen, ab, ud, err, errn);
}

static size_t file_write(char *ptr, size_t sz, size_t nm, void *ud)
{
    return fwrite(ptr, sz, nm, (FILE *)ud);
}

int net_get_file(const char *url, const char *path,
                 net_abort_fn ab, void *ud, char *err, size_t errn)
{
    if (!curl) { snprintf(err, errn, "network unavailable"); return -1; }
    AbortCtx ac = { ab, ud };
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(err, errn, "cannot write %s", path); log_msg("%s", err); return -1; }
    setvbuf(f, NULL, _IOFBF, 64 * 1024);
    u64 t0 = osGetTime();
    common_opts(url, &ac);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    CURLcode rc = curl_easy_perform(curl);
    long bytes = ftell(f);
    int werr = ferror(f);
    if (fclose(f) != 0) werr = 1;
    if (rc == CURLE_WRITE_ERROR || (rc == CURLE_OK && werr)) {
        snprintf(err, errn, "SD card write failed (full?)");
        log_msg("GET %s -> %s", url, err);
        remove(path);
        return -1;
    }
    if (finish(url, rc, bytes > 0 ? (size_t)bytes : 0, t0, err, errn)) { remove(path); return -1; }
    return 0;
}
