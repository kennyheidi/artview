#pragma once
#include <stddef.h>
#include "posts.h"

/* Saved-image library in <dir>:
 *   <dir>/<id>.<ext>          original file
 *   <dir>/<id>.txt            post info (tags, score, ...) written at save time
 *   <dir>/.thumbs/<id>.jpg    site thumbnail, used for the offline grid
 * Posts returned by saved_scan use local "sdmc:/..." paths in sample_url/file_url
 * (and preview_url when a thumbnail exists). */

/* Lists saved files newest-id first, optionally filtered by tags ("a b -c").
 * *page is clamped to the last page. Returns posts on the page, or -1 on error. */
int saved_scan(const char *dir, const char *filter, int *page, Post *out, int max,
               int *total, char *err, size_t errn);

/* Shuffle the gallery order (0 = normal order, by ID). The same seed always
 * gives the same order, so paging through a shuffle is stable. Applies to the
 * scans that follow. */
void saved_set_shuffle(unsigned seed);

/* Same, limited to the post IDs in only[0..nonly) (a gallery folder). */
int saved_scan_ids(const char *dir, const char *filter, const unsigned *only, int nonly,
                   int *page, Post *out, int max, int *total, char *err, size_t errn);

int saved_write_meta(const char *dir, const Post *p);
int saved_read_meta(const char *dir, unsigned id, Post *p);
int saved_delete(const char *dir, const Post *p);

/* Reads a whole file into a malloc'd, NUL-terminated buffer. 0 on success. */
int read_file(const char *path, char **out, size_t *len, size_t maxlen, char *err, size_t errn);
