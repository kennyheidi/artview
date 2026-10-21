#pragma once
#include <stddef.h>

typedef struct {
    unsigned id;
    int width, height, score;
    char rating[16];
    /* DeviantArt image links carry a signed ?token= and can run past 700
     * characters; a cut-off token gets HTTP 401 from the CDN. */
    char preview_url[1536];
    char sample_url[1536];
    char file_url[1536];
    char tags[1024];
    char title[96];        /* DeviantArt: deviation title (empty for boorus) */
} Post;

/* Parses the JSON array returned by the dapi post index (json=1).
 * Returns number of posts parsed (0 for an empty body / empty array),
 * or -1 if the body is not a JSON array (the API sends plain-text errors). */
int parse_posts(const char *json, size_t len, Post *out, int max);

/* Parses a Derpibooru / Philomena /api/v1/json/search/images response
 * ({"images":[...],"total":N}). Tag names have spaces replaced by '_' so they
 * fit the space-separated tag lists used everywhere else. Videos (WebM) get
 * their server-made MP4 copy as file_url, so saved videos play on the 3DS.
 * Returns posts parsed, or -1 if the body isn't that JSON. *total may be NULL. */
int parse_posts_derpi(const char *json, size_t len, Post *out, int max, int *total);

/* Parses a DeviantArt API listing ({"results":[...],"has_more":..,"next_offset":..}).
 * Literature/deleted items are skipped. id = the number at the end of the
 * deviation's URL; tags = "by:<artist>" (+ "mature"); videos use the best MP4
 * up to 720p. *has_more / *next_offset / next_cursor (may be NULL) report paging. */
int parse_posts_da(const char *json, size_t len, Post *out, int max,
                   int *has_more, int *next_offset, char *next_cursor, size_t cursor_n,
                   char *err, size_t errn);

/* Parses an e621 /posts.json response ({"posts":[...]}). Tags from every
 * category, space-separated (artists first); rating -> safe/questionable/explicit.
 * Videos get an H.264 MP4 version up to 720p as file_url when e621 has one, so
 * saved videos play on the 3DS. Posts e621 hides from this account (no file
 * URL) are skipped and counted in *hidden. Returns posts parsed, or -1 with a
 * message in err if the body isn't that JSON. */
int parse_posts_e621(const char *json, size_t len, Post *out, int max, int *hidden,
                     char *err, size_t errn);

/* pixiv (the website's JSON): kind 0 = tag search (body.illustManga.data, 60 a
 * page), 1 = bookmarks / work lists (body.works), 2 = ranking.php?format=json
 * (contents). Only items [skip, skip + max) of the reply are kept, since the
 * app shows 12 per page. preview_url = the thumbnail, sample_url = file_url =
 * the 1200px "master" image of the first page. *total is the result count when
 * pixiv says (else -1); *more says if a later page exists. Returns posts kept,
 * or -1 with pixiv's message in err. */
int parse_posts_pixiv(const char *json, size_t len, int kind, int skip, Post *out, int max,
                      int *total, int *more, char *err, size_t errn);

/* pixiv thumbnail/square URL -> its 1200px "master" image URL (in place).
 * https://i.pximg.net/c/250x250_80_a2/img-master/img/D/123_p0_square1200.jpg ->
 * https://i.pximg.net/img-master/img/D/123_p0_master1200.jpg */
void pixiv_master_url(char *url);

/* True if the URL points to a video (mp4/webm). */
int url_is_video(const char *url);

/* Extension of the URL's file name including the dot (".jpg"), or ".bin".
 * Ignores ?query / #fragment. Returns a short-lived static copy (a small ring
 * of buffers, so a few calls in one expression are fine). */
const char *url_ext(const char *url);
