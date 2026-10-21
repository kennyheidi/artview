#include "creds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void creds_clean(char *v)
{
    char *o = v;
    for (char *p = v; *p; p++)
        if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))
            *o++ = *p;
    *o = 0;
}

int creds_parse(const char *text, char *key, size_t kn, char *uid, size_t un)
{
    char buf[2048];
    int found = 0;
    /* copy, dropping spaces/tabs next to '=' so "api_key = X" parses too */
    size_t o = 0;
    for (const char *p = text; *p && o + 1 < sizeof buf; p++) {
        if (*p == ' ' || *p == '\t') {
            const char *q = p;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=' || (o && buf[o - 1] == '=')) { p = q - 1; continue; }
        }
        buf[o++] = *p;
    }
    buf[o] = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, "&?;, \t\r\n", &save); t; t = strtok_r(NULL, "&?;, \t\r\n", &save)) {
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        creds_clean(v);
        if (!*v) continue;
        /* rule34: api_key + user_id; DeviantArt apps: client_secret + client_id */
        if (!strcmp(t, "api_key") || !strcmp(t, "client_secret"))     { snprintf(key, kn, "%s", v); found |= CREDS_KEY; }
        else if (!strcmp(t, "user_id") || !strcmp(t, "client_id"))   { snprintf(uid, un, "%s", v); found |= CREDS_UID; }
    }
    return found;
}

static int is_cred_line(const char *line)
{
    const char *p = line;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#') return 0;
    return !strncmp(p, "api_key", 7) || !strncmp(p, "user_id", 7) || strstr(p, "api_key=") != NULL ||
           !strncmp(p, "client_id", 9) || !strncmp(p, "client_secret", 13) || strstr(p, "client_secret=") != NULL;
}

static const char *key_name = "api_key", *uid_name = "user_id";

void creds_set_names(const char *key, const char *uid)
{
    key_name = key;
    uid_name = uid;
}

int creds_save(const char *path, const char *key, const char *uid)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *in = fopen(path, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return -1; }

    int written = 0;
    char line[1024];
    if (in) {
        while (fgets(line, sizeof line, in)) {
            if (is_cred_line(line)) {
                if (!written) {            /* put the new lines where the old ones were */
                    fprintf(out, "%s=%s\n%s=%s\n", key_name, key, uid_name, uid);
                    written = 1;
                }
                continue;
            }
            fputs(line, out);
            size_t n = strlen(line);
            if (n && line[n - 1] != '\n' && feof(in)) fputc('\n', out);
        }
        fclose(in);
    }
    if (!written) fprintf(out, "%s=%s\n%s=%s\n", key_name, key, uid_name, uid);
    if (fclose(out) != 0) { remove(tmp); return -1; }

    remove(path);                          /* FAT rename won't overwrite */
    if (rename(tmp, path) != 0) return -1;
    return 0;
}

int config_set(const char *path, const char *key, const char *value)
{
    char tmp[256], line[1024];
    size_t kl = strlen(key);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *in = fopen(path, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return -1; }
    int done = 0;
    if (in) {
        while (fgets(line, sizeof line, in)) {
            const char *p = line;
            if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
            while (*p == ' ' || *p == '\t') p++;
            if (!strncmp(p, key, kl) && (p[kl] == '=' || p[kl] == ' ')) {
                if (!done) fprintf(out, "%s=%s\n", key, value);
                done = 1;
                continue;
            }
            fputs(line, out);
            size_t n = strlen(line);
            if (n && line[n - 1] != '\n' && feof(in)) fputc('\n', out);
        }
        fclose(in);
    }
    if (!done) fprintf(out, "%s=%s\n", key, value);
    if (fclose(out) != 0) { remove(tmp); return -1; }
    remove(path);
    return rename(tmp, path) ? -1 : 0;
}
