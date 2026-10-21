#include "log.h"
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static FILE *lf;
static LightLock ll;
static u64 t0;
static int verbose;

void log_open(const char *path, const char *old_path)
{
    LightLock_Init(&ll);
    t0 = osGetTime();
    remove(old_path);
    rename(path, old_path);
    lf = fopen(path, "w");
}

void log_close(void)
{
    LightLock_Lock(&ll);
    if (lf) fclose(lf);
    lf = NULL;
    LightLock_Unlock(&ll);
}

void log_set_verbose(int v) { verbose = v; }
int  log_verbose(void)      { return verbose; }

/* Secrets that can appear in logged URLs, headers and bodies: the value after
 * each marker is replaced with "<N chars>". */
static const char *const secret_markers[] = {
    "PHPSESSID=", "phpsessid=", "Cookie: ",
    "api_key=", "&key=", "?key=", "access_token=", "client_secret=", "\"access_token\":\"",
    "Bearer ", "Basic ", "refresh_token=", "token=", NULL
};

void log_mask(const char *src, char *dst, unsigned dstn)
{
    unsigned o = 0;
    if (!dstn) return;
    while (*src && o + 1 < dstn) {
        const char *hit = NULL;
        for (int k = 0; secret_markers[k]; k++)
            if (!strncmp(src, secret_markers[k], strlen(secret_markers[k]))) { hit = secret_markers[k]; break; }
        if (hit) {
            size_t ml = strlen(hit);
            const char *v = src + ml;
            unsigned n = 0;
            while (v[n] && v[n] != '&' && v[n] != ' ' && v[n] != '"' && v[n] != '\r' && v[n] != '\n') n++;
            int w = snprintf(dst + o, dstn - o, "%.*s<%u chars>", (int)ml, hit, n);
            o += w > 0 ? (unsigned)w : 0;
            if (o >= dstn) { o = dstn - 1; break; }
            src = v + n;
            continue;
        }
        dst[o++] = *src++;
    }
    dst[o] = 0;
}

void log_msg(const char *fmt, ...)
{
    char raw[1024], masked[1024];
    if (!lf) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(raw, sizeof raw, fmt, ap);
    va_end(ap);
    size_t n = strlen(raw);
    while (n && (raw[n - 1] == '\n' || raw[n - 1] == '\r')) raw[--n] = 0;
    log_mask(raw, masked, sizeof masked);

    LightLock_Lock(&ll);
    if (lf) {
        u64 t = osGetTime() - t0;
        fprintf(lf, "[%5llu.%03llu] %s\n", (unsigned long long)(t / 1000), (unsigned long long)(t % 1000), masked);
        fflush(lf);
    }
    LightLock_Unlock(&ll);
}
