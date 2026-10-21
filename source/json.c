#include "json.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *s; int i, len; Json *j; int depth; } P;

static void ws(P *p) { while (p->i < p->len && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r')) p->i++; }

static int add(P *p, JType t, int parent, int key)
{
    Json *j = p->j;
    if (j->count == j->cap) {
        int nc = j->cap ? j->cap * 2 : 256;
        JNode *nn = realloc(j->n, nc * sizeof *nn);
        if (!nn) return -1;
        j->n = nn;
        j->cap = nc;
    }
    JNode *n = &j->n[j->count];
    n->type = t; n->parent = parent; n->next = -1; n->child = -1; n->key = key;
    n->start = p->i; n->end = p->i;
    return j->count++;
}

/* Skips a string starting at the opening quote; returns index of the closing quote or -1. */
static int skip_str(P *p)
{
    int i = p->i + 1;
    while (i < p->len) {
        if (p->s[i] == '\\') { i += 2; continue; }
        if (p->s[i] == '"') return i;
        i++;
    }
    return -1;
}

static int value(P *p, int parent, int key);

static int container(P *p, int parent, int key, int is_obj)
{
    int me = add(p, is_obj ? J_OBJ : J_ARR, parent, key);
    if (me < 0) return -1;
    p->i++;
    int prev = -1;
    for (;;) {
        ws(p);
        if (p->i >= p->len) return -1;
        char c = p->s[p->i];
        if (c == (is_obj ? '}' : ']')) { p->i++; break; }
        if (c == ',') { p->i++; continue; }
        int k = -1;
        if (is_obj) {
            if (c != '"') return -1;
            int q = skip_str(p);
            if (q < 0) return -1;
            k = p->i + 1;
            p->i = q + 1;
            ws(p);
            if (p->i >= p->len || p->s[p->i] != ':') return -1;
            p->i++;
            ws(p);
        }
        int v = value(p, me, k);
        if (v < 0) return -1;
        if (prev < 0) p->j->n[me].child = v; else p->j->n[prev].next = v;
        prev = v;
    }
    p->j->n[me].end = p->i;
    return me;
}

static int value(P *p, int parent, int key)
{
    if (++p->depth > 64) return -1;
    ws(p);
    if (p->i >= p->len) return -1;
    char c = p->s[p->i];
    int r;
    if (c == '{' || c == '[') r = container(p, parent, key, c == '{');
    else if (c == '"') {
        int q = skip_str(p);
        if (q < 0) return -1;
        r = add(p, J_STR, parent, key);
        if (r < 0) return -1;
        p->j->n[r].start = p->i + 1;
        p->j->n[r].end = q;
        p->i = q + 1;
    } else {
        JType t = (c == 't' || c == 'f') ? J_BOOL : c == 'n' ? J_NULL : J_NUM;
        r = add(p, t, parent, key);
        if (r < 0) return -1;
        while (p->i < p->len && !strchr(",}] \t\r\n", p->s[p->i])) p->i++;
        p->j->n[r].end = p->i;
    }
    p->depth--;
    return r;
}

int json_parse(Json *j, const char *text, size_t len)
{
    memset(j, 0, sizeof *j);
    j->text = text;
    P p = { text, 0, (int)len, j, 0 };
    if (value(&p, -1, -1) != 0) { json_free(j); return -1; }
    return 0;
}

void json_free(Json *j)
{
    free(j->n);
    j->n = NULL;
    j->count = j->cap = 0;
}

/* compares an object member's (escaped) key with a plain key */
static int key_is(const Json *j, int node, const char *key)
{
    int k = j->n[node].key;
    if (k < 0) return 0;
    size_t kl = strlen(key);
    return !strncmp(j->text + k, key, kl) && j->text[k + kl] == '"';
}

int json_get(const Json *j, int obj, const char *key)
{
    if (obj < 0 || obj >= j->count || j->n[obj].type != J_OBJ) return -1;
    for (int c = j->n[obj].child; c >= 0; c = j->n[c].next)
        if (key_is(j, c, key)) return c;
    return -1;
}

int json_len(const Json *j, int arr)
{
    if (arr < 0 || arr >= j->count || (j->n[arr].type != J_ARR && j->n[arr].type != J_OBJ)) return 0;
    int n = 0;
    for (int c = j->n[arr].child; c >= 0; c = j->n[c].next) n++;
    return n;
}

int json_at(const Json *j, int arr, int i)
{
    if (arr < 0 || arr >= j->count) return -1;
    for (int c = j->n[arr].child; c >= 0; c = j->n[c].next)
        if (i-- == 0) return c;
    return -1;
}

double json_num(const Json *j, int node, double def)
{
    if (node < 0 || node >= j->count) return def;
    if (j->n[node].type == J_NUM) return strtod(j->text + j->n[node].start, NULL);
    if (j->n[node].type == J_STR) {                    /* "123" (DeviantArt sends some numbers as strings) */
        char b[32]; json_str(j, node, b, sizeof b);
        char *e; double v = strtod(b, &e);
        return e != b ? v : def;
    }
    return def;
}

int json_bool(const Json *j, int node, int def)
{
    if (node < 0 || node >= j->count || j->n[node].type != J_BOOL) return def;
    return j->text[j->n[node].start] == 't';
}

static void put_utf8(char **o, char *oe, unsigned cp)
{
    char t[4]; int n;
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | cp >> 6); t[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else { t[0] = (char)(0xE0 | cp >> 12); t[1] = (char)(0x80 | ((cp >> 6) & 63)); t[2] = (char)(0x80 | (cp & 63)); n = 3; }
    for (int i = 0; i < n && *o < oe; i++) *(*o)++ = t[i];
}

int json_str(const Json *j, int node, char *out, size_t n)
{
    if (!n) return 0;
    out[0] = 0;
    if (node < 0 || node >= j->count || j->n[node].type != J_STR) return 0;
    const char *s = j->text + j->n[node].start, *e = j->text + j->n[node].end;
    char *o = out, *oe = out + n - 1;
    while (s < e && o < oe) {
        char c = *s++;
        if (c != '\\' || s >= e) { *o++ = c; continue; }
        c = *s++;
        switch (c) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': case 'f': break;
        case 'u': {
            unsigned cp = 0;
            for (int k = 0; k < 4 && s < e; k++, s++) {
                char h = *s; cp <<= 4;
                cp |= h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0;
            }
            put_utf8(&o, oe, cp);
            break;
        }
        default: *o++ = c;   /* \" \\ \/ */
        }
    }
    *o = 0;
    return 1;
}

int json_path(const Json *j, int node, ...)
{
    va_list ap;
    va_start(ap, node);
    const char *k;
    while (node >= 0 && (k = va_arg(ap, const char *))) node = json_get(j, node, k);
    va_end(ap);
    return node;
}
