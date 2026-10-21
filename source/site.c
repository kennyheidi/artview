#include "site.h"

const SiteInfo site_info[SITE_COUNT] = {
    [SITE_R34]   = { "r34view",   "rule34.xxx", "rule34",     1, 1, 128, 0xaae5a4 },
    [SITE_DERPI] = { "derpiview", "Derpibooru", "Derpibooru", 0, 0, 0,   0xc4a0ff },  /* key optional: your filters */
    [SITE_DA]    = { "daview",    "DeviantArt", "DeviantArt", 1, 1, 32,  0x6cd9c0 },  /* your own app: client_id + secret */
    [SITE_E6]    = { "e6view",    "e621",       "e621",       0, 0, 0,   0xf2c14e },  /* key optional: login + API key */
    [SITE_PX]    = { "pxview",    "pixiv",      "pixiv",      0, 0, 0,   0x5cb8ff },  /* login optional: PHPSESSID cookie */
};

#if defined(SITE_MULTI)
int g_site = SITE_R34;
#endif
