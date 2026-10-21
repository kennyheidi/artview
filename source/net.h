#pragma once
#include <stddef.h>

typedef int (*net_abort_fn)(void *ud);   /* return nonzero to cancel a transfer */

int  net_init(int verify_ssl, char *err, size_t errn);
void net_exit(void);

/* Extra request options (all optional). */
typedef struct {
    const char *post;          /* form body: makes it a POST */
    const char *userpwd;       /* "user:password" for HTTP Basic auth */
    const char *header[2];     /* extra headers, e.g. "Authorization: Bearer ..." */
    const char *cookie;        /* "NAME=value" cookie(s) to send */
    int  keep_error_body;      /* on HTTP >= 400, still return the body in *out (with -1) */
    long http;                 /* out: HTTP status */
} NetReq;

/* GET/POST url into a malloc'd, NUL-terminated buffer (caller frees). 0 on success.
 * On failure *out is only set when rq->keep_error_body and the server replied. */
int net_request(const char *url, NetReq *rq, char **out, size_t *outlen, size_t maxlen,
                net_abort_fn ab, void *ud, char *err, size_t errn);

/* GET url into a malloc'd, NUL-terminated buffer (caller frees). 0 on success. */
int net_get(const char *url, char **out, size_t *outlen, size_t maxlen,
            net_abort_fn ab, void *ud, char *err, size_t errn);

/* GET url straight into a file. 0 on success; the partial file is removed on failure. */
int net_get_file(const char *url, const char *path,
                 net_abort_fn ab, void *ud, char *err, size_t errn);

/* URL-encode a string (caller frees). */
char *net_escape(const char *s);
