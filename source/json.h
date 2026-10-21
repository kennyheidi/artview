#pragma once
#include <stddef.h>

/* Minimal JSON reader: parses a document into a flat node array (no copying
 * of strings until asked). Enough for API responses; not a validator. */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct {
    JType type;
    int   parent;
    int   next;        /* next sibling, -1 = last */
    int   child;       /* first child (arrays/objects), -1 = none */
    int   key;         /* for object members: offset of the key string in the text, -1 otherwise */
    int   start, end;  /* value span in the text (strings: inside the quotes) */
} JNode;

typedef struct {
    const char *text;
    JNode *n;
    int    count, cap;
} Json;

int  json_parse(Json *j, const char *text, size_t len);   /* 0 ok; root is node 0 */
void json_free(Json *j);

int    json_get(const Json *j, int obj, const char *key);  /* member value node, -1 if none */
int    json_len(const Json *j, int arr);
int    json_at(const Json *j, int arr, int i);
double json_num(const Json *j, int node, double def);
int    json_bool(const Json *j, int node, int def);
/* Copies a string value, unescaped (UTF-8), truncated to n. Returns 0 if not a string. */
int    json_str(const Json *j, int node, char *out, size_t n);
/* Path helper: json_path(j, root, "author", "username", NULL) */
int    json_path(const Json *j, int node, ...);
