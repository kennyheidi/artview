#pragma once
#include <stddef.h>

#define CREDS_KEY 1
#define CREDS_UID 2

/* Keep only [0-9A-Za-z] (drops quotes, spaces, stray punctuation). */
void creds_clean(char *v);

/* Finds api_key=... and user_id=... anywhere in text: the "&api_key=...&user_id=..."
 * line from the site, a full URL, or "api_key=X user_id=Y" on separate lines.
 * Returns CREDS_KEY | CREDS_UID for what was found (out buffers untouched otherwise). */
int creds_parse(const char *text, char *key, size_t kn, char *uid, size_t un);

/* Writes api_key/user_id into the config file, replacing any existing credential
 * lines and keeping everything else (comments, other settings). 0 on success. */
int creds_save(const char *path, const char *key, const char *uid);

/* Config key names creds_save writes (default "api_key"/"user_id";
 * DeviantArt builds use "client_secret"/"client_id"). */
void creds_set_names(const char *key, const char *uid);

/* Sets key=value in the config file (replacing an existing key= line, else
 * appending), keeping everything else. 0 on success. */
int config_set(const char *path, const char *key, const char *value);
