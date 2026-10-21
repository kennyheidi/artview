#include "saved.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

typedef struct { unsigned id; char name[64]; } Entry;

int read_file(const char *path, char **out, size_t *len, size_t maxlen, char *err, size_t errn)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errn, "cannot open file"); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (size_t)sz + 1 > maxlen) {
        fclose(f);
        snprintf(err, errn, "file too large (%ld KB)", sz / 1024);
        return -1;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); snprintf(err, errn, "out of memory"); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); snprintf(err, errn, "read error"); return -1; }
    buf[sz] = 0;
    *out = buf;
    *len = (size_t)sz;
    return 0;
}

static int is_media(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    static const char *ok[] = { ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webm", ".mp4", NULL };
    for (int i = 0; ok[i]; i++)
        if (!strcasecmp(dot, ok[i])) return 1;
    return 0;
}

static int cmp_entry(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->id != y->id) return x->id < y->id ? 1 : -1;   /* newest (highest id) first */
    return strcmp(x->name, y->name);
}

static int has_tag(const char *tags, const char *t)
{
    size_t n = strlen(t);
    for (const char *p = tags; (p = strstr(p, t)); p++) {
        int start = (p == tags || p[-1] == ' ');
        int end = (p[n] == 0 || p[n] == ' ');
        if (start && end) return 1;
    }
    return 0;
}

/* "a b -c": all plain terms present, no "-" term present. A bare number matches the id. */
static int matches(const char *filter, unsigned id, const char *tags)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", filter);
    for (char *c = buf; *c; c++) *c = tolower((unsigned char)*c);
    if (strchr(buf, ',')) {                 /* "tag one, -tag two": commas separate, spaces are '_' */
        char *o = buf;
        for (char *c = buf; *c; c++) {
            if (*c == ',') { *o++ = ' '; while (c[1] == ' ') c++; }
            else if (*c == ' ' && (c[1] == ',' || c[1] == 0)) continue;
            else *o++ = *c == ' ' ? '_' : *c;
        }
        *o = 0;
    }
    char *save = NULL;
    for (char *t = strtok_r(buf, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
        int neg = (*t == '-');
        if (neg) t++;
        if (!*t) continue;
        int hit = has_tag(tags, t);
        if (!hit) {
            char *end;
            unsigned long v = strtoul(t, &end, 10);
            hit = (*end == 0 && v == id);
        }
        if (hit == neg) return 0;
    }
    return 1;
}

int saved_read_meta(const char *dir, unsigned id, Post *p)
{
    char path[192], line[4096];
    snprintf(path, sizeof path, "%s/%u.txt", dir, id);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "width"))       p->width = atoi(v);
        else if (!strcmp(k, "height")) p->height = atoi(v);
        else if (!strcmp(k, "score"))  p->score = atoi(v);
        else if (!strcmp(k, "rating")) snprintf(p->rating, sizeof p->rating, "%s", v);
        else if (!strcmp(k, "tags"))   snprintf(p->tags, sizeof p->tags, "%s", v);
        else if (!strcmp(k, "title"))  snprintf(p->title, sizeof p->title, "%s", v);
    }
    fclose(f);
    return 0;
}

int saved_write_meta(const char *dir, const Post *p)
{
    char path[192];
    snprintf(path, sizeof path, "%s/%u.txt", dir, p->id);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "id=%u\nwidth=%d\nheight=%d\nscore=%d\nrating=%s\nfile_url=%s\ntags=%s\n",
            p->id, p->width, p->height, p->score, p->rating, p->file_url, p->tags);
    if (p->title[0]) {
        char t[sizeof p->title];
        snprintf(t, sizeof t, "%s", p->title);
        for (char *c = t; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
        fprintf(f, "title=%s\n", t);
    }
    fclose(f);
    return 0;
}

int saved_delete(const char *dir, const Post *p)
{
    char path[192];
    int rc = remove(p->file_url);
    snprintf(path, sizeof path, "%s/%u.txt", dir, p->id);
    remove(path);
    snprintf(path, sizeof path, "%s/.thumbs/%u.jpg", dir, p->id);
    remove(path);
    return rc;
}

static int in_set(const unsigned *only, int nonly, unsigned id)
{
    for (int i = 0; i < nonly; i++) if (only[i] == id) return 1;
    return 0;
}

int saved_scan(const char *dir, const char *filter, int *page, Post *out, int max,
               int *total, char *err, size_t errn)
{
    return saved_scan_ids(dir, filter, NULL, 0, page, out, max, total, err, errn);
}

static unsigned shuffle_seed;

void saved_set_shuffle(unsigned seed) { shuffle_seed = seed; }

int saved_scan_ids(const char *dir, const char *filter, const unsigned *only, int nonly,
                   int *page, Post *out, int max, int *total, char *err, size_t errn)
{
    DIR *d = opendir(dir);
    *total = 0;
    if (!d) { snprintf(err, errn, "cannot open %s", dir); return -1; }

    Entry *list = NULL;
    int n = 0, cap = 0;
    int filtering = filter && filter[strspn(filter, " ")];
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *name = de->d_name;
        if (name[0] == '.' || !is_media(name) || strlen(name) >= sizeof list->name) continue;
        unsigned id = (unsigned)strtoul(name, NULL, 10);
        if (only && !in_set(only, nonly, id)) continue;
        if (filtering) {
            Post m; memset(&m, 0, sizeof m);
            saved_read_meta(dir, id, &m);
            if (!matches(filter, id, m.tags)) continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            Entry *nl = realloc(list, cap * sizeof *list);
            if (!nl) { free(list); closedir(d); snprintf(err, errn, "out of memory"); return -1; }
            list = nl;
        }
        list[n].id = id;
        snprintf(list[n].name, sizeof list[n].name, "%s", name);
        n++;
    }
    closedir(d);
    qsort(list, n, sizeof *list, cmp_entry);
    if (shuffle_seed && n > 1) {                     /* Fisher-Yates with a seeded xorshift */
        unsigned x = shuffle_seed;
        for (int i = n - 1; i > 0; i--) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            int j = (int)(x % (unsigned)(i + 1));
            Entry t = list[i]; list[i] = list[j]; list[j] = t;
        }
    }

    *total = n;
    int last = n ? (n - 1) / max : 0;
    if (*page > last) *page = last;
    if (*page < 0) *page = 0;

    int count = 0;
    for (int i = *page * max; i < n && count < max; i++, count++) {
        Post *p = &out[count];
        memset(p, 0, sizeof *p);
        p->id = list[i].id;
        snprintf(p->file_url, sizeof p->file_url, "%s/%s", dir, list[i].name);
        snprintf(p->sample_url, sizeof p->sample_url, "%s", p->file_url);
        char tp[192];
        struct stat st;
        snprintf(tp, sizeof tp, "%s/.thumbs/%u.jpg", dir, p->id);
        if (stat(tp, &st) == 0) snprintf(p->preview_url, sizeof p->preview_url, "%s", tp);
        saved_read_meta(dir, p->id, p);
    }
    free(list);
    return count;
}
