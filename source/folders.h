#pragma once
#include <stdbool.h>

/* Gallery folders: named lists of post IDs, stored in a text file:
 *   [Folder name]
 *   12345
 *   67890
 * Images stay where they are; a post can be in any number of folders. */

#define FOLDER_NAME_MAX 40
#define FOLDERS_MAX     64

typedef struct {
    char      name[FOLDER_NAME_MAX];
    unsigned *ids;
    int       n, cap;
} Folder;

typedef struct {
    Folder f[FOLDERS_MAX];
    int    n;
} Folders;

int  folders_load(Folders *fs, const char *path);        /* missing file = no folders; 0 ok */
int  folders_save(const Folders *fs, const char *path);  /* 0 ok */
void folders_free(Folders *fs);

/* Trims, drops characters that would break the file ([ ] and control chars),
 * and truncates. Returns false if nothing is left. */
bool folders_clean_name(char *name);

int  folders_find(const Folders *fs, const char *name);  /* case-insensitive, -1 if none */
int  folders_add(Folders *fs, const char *name);         /* index; -1 invalid, -2 exists, -3 full */
int  folders_rename(Folders *fs, int i, const char *name);/* 0 ok; -1 invalid, -2 exists */
void folders_delete(Folders *fs, int i);

bool folders_has(const Folders *fs, int i, unsigned id);
int  folders_toggle(Folders *fs, int i, unsigned id);    /* 1 added, 0 removed, -1 no memory */
void folders_remove_id(Folders *fs, unsigned id);        /* from every folder */
