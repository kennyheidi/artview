#pragma once
/* Which site(s) a build talks to.
 *   make                     -> artview: rule34.xxx, Derpibooru and DeviantArt in one app,
 *                               switched with the site tab at the bottom of the grid
 *   make SITE=rule34         -> r34view    (rule34.xxx only)
 *   make SITE=derpibooru     -> derpiview  (Derpibooru only)
 *   make SITE=deviantart     -> daview     (DeviantArt only)
 *   make SITE=e621           -> e6view     (e621 only)
 *   make SITE=pixiv          -> pxview     (pixiv only)
 * Every site keeps its own folder (config, gallery, gallery folders) whichever
 * build uses it: sdmc:/3ds/r34view, /derpiview, /daview, /e6view, /pxview. So the combined app and
 * the single-site apps share galleries and logins. */

enum { SITE_R34, SITE_DERPI, SITE_DA, SITE_E6, SITE_PX, SITE_COUNT };

typedef struct {
    const char *app;           /* folder name: sdmc:/3ds/<app> */
    const char *name;          /* "rule34.xxx" */
    const char *tab;           /* short label for the site tab */
    int key_required;          /* searching needs credentials */
    int key_needs_uid;         /* credentials are a key + an id */
    int key_len;               /* expected key length (0 = any) */
    unsigned accent;           /* 0xRRGGBB */
} SiteInfo;

extern const SiteInfo site_info[SITE_COUNT];

#if defined(SITE_MULTI)
extern int g_site;
#define CUR_SITE      g_site
#define BUILD_NAME    "artview"
#elif defined(SITE_PIXIV)
#define CUR_SITE      SITE_PX
#define BUILD_NAME    "pxview"
#elif defined(SITE_E621)
#define CUR_SITE      SITE_E6
#define BUILD_NAME    "e6view"
#elif defined(SITE_DEVIANTART)
#define CUR_SITE      SITE_DA
#define BUILD_NAME    "daview"
#elif defined(SITE_DERPIBOORU)
#define CUR_SITE      SITE_DERPI
#define BUILD_NAME    "derpiview"
#else
#define CUR_SITE      SITE_R34
#define BUILD_NAME    "r34view"
#endif

#define IS_R34        (CUR_SITE == SITE_R34)
#define IS_DERPI      (CUR_SITE == SITE_DERPI)
#define IS_DA         (CUR_SITE == SITE_DA)
#define IS_E6         (CUR_SITE == SITE_E6)
#define IS_PX         (CUR_SITE == SITE_PX)
#define APP_NAME      (site_info[CUR_SITE].app)
#define SITE_NAME     (site_info[CUR_SITE].name)
#define KEY_REQUIRED  (site_info[CUR_SITE].key_required)
#define KEY_NEEDS_UID (site_info[CUR_SITE].key_needs_uid)
#define KEY_LEN       (site_info[CUR_SITE].key_len)
