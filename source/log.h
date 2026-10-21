#pragma once
/* Debug log written to sdmc:/3ds/r34view/log.txt (previous run kept as log.old.txt).
 * Thread-safe, flushed after every line so it survives crashes.
 * Any "api_key=<value>" in a message is masked automatically. */

void log_open(const char *path, const char *old_path);
void log_close(void);
void log_set_verbose(int v);
int  log_verbose(void);
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Replace the value of every api_key= in src with "<N chars>". Exposed for testing. */
void log_mask(const char *src, char *dst, unsigned dstn);
