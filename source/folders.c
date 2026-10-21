#include "folders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool folders_clean_name(char *name)
{
    char tmp[FOLDER_NAME_MAX];
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)name;
    while (*p == ' ' || *p == '\t') p++;
    for (; *p && o + 1 < sizeof tmp; p++) {
        if (*p < 0x20 || *p == 0x7F || *p == '[' || *p == ']') continue;
        tmp[o++] = (char)*p;
    }
    while (o && tmp[o - 1] == ' ') o--;
    tmp[o] = 0;
    strcpy(name, tmp);
    return o > 0;
}

int folders_find(const Folders *fs, const char *name)
{
    for (int i = 0; i < fs->n; i++)
        if (!strcasecmp(fs->f[i].name, name)) return i;
    return -1;
}

static int push_id(Folder *f, unsigned id)
{
    if (f->n == f->cap) {
        int nc = f->cap ? f->cap * 2 : 16;
        unsigned *ni = realloc(f->ids, nc * sizeof *ni);
        if (!ni) return -1;
        f->ids = ni;
        f->cap = nc;
    }
    f->ids[f->n++] = id;
    return 0;
}

int folders_add(Folders *fs, const char *name)
{
    char nm[FOLDER_NAME_MAX];
    snprintf(nm, sizeof nm, "%s", name);
    if (!folders_clean_name(nm)) return -1;
    if (folders_find(fs, nm) >= 0) return -2;
    if (fs->n >= FOLDERS_MAX) return -3;
    Folder *f = &fs->f[fs->n];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", nm);
    return fs->n++;
}

int folders_rename(Folders *fs, int i, const char *name)
{
    char nm[FOLDER_NAME_MAX];
    snprintf(nm, sizeof nm, "%s", name);
    if (i < 0 || i >= fs->n || !folders_clean_name(nm)) return -1;
    int j = folders_find(fs, nm);
    if (j >= 0 && j != i) return -2;
    snprintf(fs->f[i].name, sizeof fs->f[i].name, "%s", nm);
    return 0;
}

void folders_delete(Folders *fs, int i)
{
    if (i < 0 || i >= fs->n) return;
    free(fs->f[i].ids);
    memmove(&fs->f[i], &fs->f[i + 1], (fs->n - i - 1) * sizeof fs->f[0]);
    fs->n--;
}

bool folders_has(const Folders *fs, int i, unsigned id)
{
    if (i < 0 || i >= fs->n) return false;
    for (int k = 0; k < fs->f[i].n; k++) if (fs->f[i].ids[k] == id) return true;
    return false;
}

int folders_toggle(Folders *fs, int i, unsigned id)
{
    if (i < 0 || i >= fs->n) return -1;
    Folder *f = &fs->f[i];
    for (int k = 0; k < f->n; k++)
        if (f->ids[k] == id) {
            memmove(&f->ids[k], &f->ids[k + 1], (f->n - k - 1) * sizeof *f->ids);
            f->n--;
            return 0;
        }
    return push_id(f, id) ? -1 : 1;
}

void folders_remove_id(Folders *fs, unsigned id)
{
    for (int i = 0; i < fs->n; i++)
        if (folders_has(fs, i, id)) folders_toggle(fs, i, id);
}

void folders_free(Folders *fs)
{
    for (int i = 0; i < fs->n; i++) free(fs->f[i].ids);
    memset(fs, 0, sizeof *fs);
}

int folders_load(Folders *fs, const char *path)
{
    folders_free(fs);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int cur = -1;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;
        size_t n = strlen(s);
        while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') {
            char *e = strrchr(s, ']');
            if (e) *e = 0;
            cur = folders_add(fs, s + 1);
            if (cur == -2) cur = folders_find(fs, s + 1);   /* duplicate header: merge */
            continue;
        }
        if (cur < 0 || *s < '0' || *s > '9') continue;
        unsigned id = (unsigned)strtoul(s, NULL, 10);
        if (!folders_has(fs, cur, id)) push_id(&fs->f[cur], id);
    }
    fclose(f);
    return 0;
}

int folders_save(const Folders *fs, const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fputs("# r34view gallery folders: [name] then one post ID per line\n", f);
    for (int i = 0; i < fs->n; i++) {
        fprintf(f, "[%s]\n", fs->f[i].name);
        for (int k = 0; k < fs->f[i].n; k++) fprintf(f, "%u\n", fs->f[i].ids[k]);
    }
    if (fclose(f) != 0) { remove(tmp); return -1; }
    remove(path);                                 /* FAT rename won't overwrite */
    return rename(tmp, path) ? -1 : 0;
}
