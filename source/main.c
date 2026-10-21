/*
 * r34view / derpiview - rule34.xxx / Derpibooru browser for the New Nintendo 3DS
 * (one codebase; see site.h)
 *
 * Bottom screen: tag search bar + 4x3 thumbnail grid (touch or D-pad).
 * Top screen:    selected post info, or the full image with zoom/pan.
 * SELECT switches between online search and the offline saved-images library.
 * Networking, SD access and image decoding run on a worker thread (core 2 on
 * New 3DS); textures are only created/freed on the main thread between frames.
 * Debug log: sdmc:/3ds/<app>/log.txt
 */
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <sys/stat.h>

#include "posts.h"
#include "image.h"
#include "net.h"
#include "saved.h"
#include "log.h"
#include "creds.h"
#include "qrscan.h"
#include "video.h"
#include "folders.h"
#include "site.h"
#include "json.h"
#include "qrgen/qrcodegen.h"

#define APP_VERSION   "1.3"

#ifdef SITE_MULTI
#define MULTI 1               /* all sites in one app, switched with the site tab */
#else
#define MULTI 0
#endif

#define PER_PAGE      12
#define GRID_COLS     4
#define GRID_ROWS     3
#define BAR_H         24
#define PAGE_BX       250     /* grid top bar: page number (tap to jump) */
#define PAGE_BW       46
#define HELP_GX       244     /* grid bottom-right buttons start here: album, then "?" */
#define ALBUM_GW      46      /* grid album (gallery) button, at HELP_GX */
#define HELPQ_GX      294     /* grid "?" (controls) button */
#define HELPQ_GW      24
#define HELP_GY       221
#define HELP_GW       74
#define HELP_GH       18
#define HELP_VX       292     /* viewer "?" button in the header */
#define DUAL_VX       262     /* viewer "second image on the bottom screen" button */
#define SEEK_X        112     /* video seek bar in the viewer header */
#define SEEK_W        140
#define DUAL_MAX_W    640     /* second image: 2x the bottom screen, so fill mode stays sharp */
#define DUAL_MAX_H    480
#define DUAL_ARROW_W  40
#define SAVE_BX       66      /* viewer save (disk) button, next to Clear */
#define BACK_BX       98      /* viewer Back button, next to the save button */
#define BACK_BW       38
#define SAVE_BW       28
#define FOLD_GX       2       /* grid "Folders" button (gallery) */
#define FOLD_GW       72
#define GEAR_GX       216     /* grid Options (menu) button */
#define GEAR_GW       24
#define SORT_GX       (FOLD_GX + FOLD_GW + 4)   /* grid Sort button (online) */
#define SORT_GW       24
#define SITE_TX       (SORT_GX + SORT_GW + 4)   /* grid site tab (combined app) */
#define SITE_TW       (GEAR_GX - 4 - SITE_TX)
#define CELL_W        80
#define CELL_H        72
#define THUMB_MAX     128
/* Full images: up to 1024 px wide and up to
 * 3072 px tall, so tall posts stay sharp in fill mode. Stored as 1024-row strips. */
#define FULL_MAX_W    1024
#define FULL_MAX_H    3072
#define TILE_H        1024
#define MAX_TILES     3
#define LIST_MAX_BYTES  (2u * 1024 * 1024)
#define IMG_MAX_BYTES   (16u * 1024 * 1024)
#define GIF_MAX_BYTES   (40u * 1024 * 1024)   /* gallery GIFs can be large */
#define GIF_MAX         400                   /* animated GIF frames fit 400x400 */
#define GIF_CACHE_BYTES (24u * 1024 * 1024)   /* frames kept for smooth looping */

/* Per-site paths (sdmc:/3ds/<site app>/...), set by site_paths() */
static char app_dir[48], cfg_path[64], save_dir[64], thumb_dir[80], folders_path[64];
static char log_path[64];             /* the build's own folder: one log for the whole app */
#define CFG_PATH     cfg_path
#define SAVE_DIR     save_dir
#define THUMB_DIR    thumb_dir
#define FOLDERS_PATH folders_path
#define SAVE_NAME(path) ((path) + strlen(save_dir) + 1)   /* "123.jpg" from a gallery path */

/* ---------------------------------------------------------------- theme
 * Colours come from the theme (Options): presets, or Custom from three colours.
 * The accent is the site's colour unless a custom theme sets its own. */
typedef struct {
    const char *name;
    unsigned bg, panel, cell, text, dim, saved, warn;   /* 0xRRGGBB */
    bool light;                                          /* light background: darken accents */
} ThemeDef;

static const ThemeDef themes[] = {
    { "Slate",         0x16181d, 0x22262e, 0x2b303a, 0xe8eaee, 0x9aa0ab, 0x8cc4ff, 0xffb46a, false },
    { "OLED black",    0x000000, 0x111111, 0x1d1d1d, 0xf0f0f0, 0x8c8c8c, 0x8cc4ff, 0xffb46a, false },
    { "Midnight blue", 0x0d1426, 0x17213a, 0x22304f, 0xe6ecff, 0x8e9bbf, 0x8cc4ff, 0xffb46a, false },
    { "Forest",        0x111a14, 0x1b2a20, 0x25382b, 0xe6f0e8, 0x93a898, 0x8cc4ff, 0xffb46a, false },
    { "Plum",          0x1a1220, 0x281c31, 0x352541, 0xf0e6f4, 0xa493ad, 0x8cc4ff, 0xffb46a, false },
    { "Light",         0xf2f3f5, 0xe1e4ea, 0xd0d5de, 0x1b1e24, 0x5d6470, 0x1f6fd1, 0xc25a00, true  },
    { "Custom",        0, 0, 0, 0, 0, 0, 0, false },
};
#define THEME_COUNT  ((int)(sizeof themes / sizeof themes[0]))
#define THEME_CUSTOM (THEME_COUNT - 1)

static struct {
    int  idx;
    unsigned custom_bg, custom_text, custom_accent;     /* custom_accent 0xFFFFFFFF = site colour */
    u32  bg, panel, cell, text, dim, saved, warn;
    bool light, own_accent;
    u32  accent;
} th = { 0, 0x16181d, 0xe8eaee, 0xFFFFFFFFu };

static u32 rgb(unsigned c, u8 a) { return C2D_Color32((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, a); }
static unsigned mix_rgb(unsigned a, unsigned b, float t)            /* a -> b by t */
{
    unsigned o = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        float x = ((a >> sh) & 0xff) * (1 - t) + ((b >> sh) & 0xff) * t;
        o |= (unsigned)(x + 0.5f) << sh;
    }
    return o;
}
static float luma(unsigned c) { return (0.299f * ((c >> 16) & 0xff) + 0.587f * ((c >> 8) & 0xff) + 0.114f * (c & 0xff)) / 255; }
static u32 with_alpha(u32 c, u8 a) { return (c & 0x00FFFFFFu) | ((u32)a << 24); }
/* blend two C2D colours (ABGR) */
static u32 tint(u32 a, u32 b, float t)
{
    unsigned ra = ((a & 0xff) << 16) | (a & 0xff00) | ((a >> 16) & 0xff);
    unsigned rb = ((b & 0xff) << 16) | (b & 0xff00) | ((b >> 16) & 0xff);
    return rgb(mix_rgb(ra, rb, t), 0xff);
}

static void theme_apply(void)
{
    ThemeDef d;
    if (th.idx < 0 || th.idx >= THEME_COUNT) th.idx = 0;
    if (th.idx == THEME_CUSTOM) {
        unsigned bg = th.custom_bg, tx = th.custom_text;
        bool light = luma(bg) > 0.5f;
        d = (ThemeDef){ "Custom", bg, mix_rgb(bg, tx, 0.07f), mix_rgb(bg, tx, 0.14f), tx, mix_rgb(tx, bg, 0.4f),
                        light ? 0x1f6fd1 : 0x8cc4ff, light ? 0xc25a00 : 0xffb46a, light };
    } else {
        d = themes[th.idx];
    }
    th.bg = rgb(d.bg, 0xff); th.panel = rgb(d.panel, 0xff); th.cell = rgb(d.cell, 0xff);
    th.text = rgb(d.text, 0xff); th.dim = rgb(d.dim, 0xff); th.saved = rgb(d.saved, 0xff); th.warn = rgb(d.warn, 0xff);
    th.light = d.light;
    th.own_accent = th.idx == THEME_CUSTOM && th.custom_accent != 0xFFFFFFFFu;
    if (th.own_accent) th.accent = rgb(th.custom_accent, 0xff);
}

/* site colour, darkened on light themes so it stays readable */
static u32 site_color(int i)
{
    unsigned c = site_info[i].accent;
    if (th.light) c = mix_rgb(c, 0x000000, 0.45f);
    return rgb(c, 0xff);
}

#define C_BG      (th.bg)
#define C_PANEL   (th.panel)
#define C_CELL    (th.cell)
/* each site's colour: rule34 green, Derpibooru purple, DeviantArt teal */
#define SITE_COLOR(i) site_color(i)
#define C_ACCENT  (th.own_accent ? th.accent : SITE_COLOR(CUR_SITE))
#define C_SAVED   (th.saved)
#define C_TEXT    (th.text)
#define C_DIM     (th.dim)
#define C_WARN    (th.warn)
#define C_SHADE   C2D_Color32(0x00, 0x00, 0x00, 0xa0)

typedef enum { SRC_ONLINE, SRC_SAVED } Source;

/* ---------------------------------------------------------------- config */

static struct {
    char api_key[512];      /* rule34 keys are 128 hex chars */
    char filter_id[16];     /* Derpibooru: filter to search with (blank = default / your account's) */
    char username[64];      /* DeviantArt: your username, for the Faves button */
    int  mature;            /* DeviantArt: include mature content */
    char user_id[64];
    char default_tags[256];
    char extra_tags[256];   /* appended to every search, e.g. "-ai_generated" */
    int  verify_ssl;
    int  verbose_log;
    int  fill_screen;       /* viewer starts in fill mode */
    char sort[32];          /* online sort (Options), a value from the site's sort list; "" = newest */
    char da_refresh[200];   /* DeviantArt: signed-in user's refresh token (rotates on every use) */
    char redirect_uri[128]; /* DeviantArt: redirect URI whitelisted in your app (sign-in) */
} cfg = { .verify_ssl = 1 };

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

static void site_paths(void)
{
    snprintf(app_dir, sizeof app_dir, "sdmc:/3ds/%s", APP_NAME);
    snprintf(cfg_path, sizeof cfg_path, "%s/config.ini", app_dir);
    snprintf(save_dir, sizeof save_dir, "%s/saved", app_dir);
    snprintf(thumb_dir, sizeof thumb_dir, "%s/.thumbs", save_dir);
    snprintf(folders_path, sizeof folders_path, "%s/folders.txt", app_dir);
}

static void ensure_dirs(void)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/" BUILD_NAME, 0777);
    mkdir(app_dir, 0777);
    mkdir(SAVE_DIR, 0777);
    mkdir(THUMB_DIR, 0777);
}

static void write_default_config(void)
{
    FILE *f = fopen(CFG_PATH, "w");
    if (!f) return;
    if (IS_DA)
    fputs("# daview config\n"
          "# DeviantArt needs your own (free) API app: log in at deviantart.com/developers,\n"
          "# Register Application, then copy its client_id and client_secret here.\n"
          "# You can also paste client_id=...&client_secret=... on one line.\n"
          "client_id=\n"
          "client_secret=\n"
          "# Your DeviantArt username: the Faves button shows your favourites\n"
          "username=\n"
          "# 1 = include mature content (unblurred only when signed in to an age-verified account)\n"
          "mature=0\n"
          "# Sign-in (optional): add this exact address to your app's \"OAuth2 Redirect URI\n"
          "# Whitelist\" at deviantart.com/developers, then use Sign in on the key screen.\n"
          "redirect_uri=https://localhost/\n"
          "# Set by the app when you sign in. Don't share it.\n"
          "refresh_token=\n"
          "# Search on startup (blank = your favourites if username is set, else Daily Deviations)\n"
          "default_tags=\n"
          "extra_tags=\n"
          "verify_ssl=1\n"
          "verbose_log=0\n"
          "fill_screen=0\n", f);
    else if (IS_PX)
    fputs("# pxview config (pixiv)\n"
          "# Optional: your pixiv login cookie, for R-18 works and your bookmarks.\n"
          "# In a PC browser logged in to pixiv.net: developer tools > Application (Storage)\n"
          "# > Cookies > https://www.pixiv.net > PHPSESSID, and copy its value here.\n"
          "# It works like your password: don't share it.\n"
          "phpsessid=\n"
          "# Your pixiv user ID (the number in your profile link), for the Faves button.\n"
          "# Not needed when phpsessid is set: it starts with your ID.\n"
          "username=\n"
          "# Search on startup (blank = today's ranking)\n"
          "default_tags=\n"
          "extra_tags=\n"
          "verify_ssl=1\n"
          "verbose_log=0\n"
          "fill_screen=0\n", f);
    else if (IS_E6)
    fputs("# e6view config (e621.net)\n"
          "# Optional: your e621 username and API key (e621.net -> Account -> Manage API Access).\n"
          "# Without them you browse as a guest, which hides some posts.\n"
          "username=\n"
          "api_key=\n"
          "# Tags searched on startup (e621 syntax, e.g. rating:s fox)\n"
          "default_tags=\n"
          "# Added to every search (e.g. -animated or rating:s)\n"
          "extra_tags=\n"
          "verify_ssl=1\n"
          "verbose_log=0\n"
          "fill_screen=0\n", f);
    else if (IS_DERPI)
    fputs("# derpiview config\n"
          "# Optional: your Derpibooru API key (derpibooru.org -> Account -> API Key).\n"
          "# With it, searches use your account's filters; without it, the site's default filter.\n"
          "api_key=\n"
          "# Optional: a filter ID to search with (from the filter's page URL on derpibooru.org)\n"
          "filter_id=\n"
          "# Tags searched on startup (Derpibooru syntax, e.g. safe, twilight sparkle)\n"
          "default_tags=\n"
          "# Added to every search\n"
          "extra_tags=\n"
          "verify_ssl=1\n"
          "verbose_log=0\n"
          "fill_screen=0\n", f);
    else
    fputs("# r34view config\n"
          "# Get these from rule34.xxx -> My Account -> Options -> API Access Credentials\n"
          "# You can also paste the whole &api_key=...&user_id=... string on one line.\n"
          "api_key=\n"
          "user_id=\n"
          "# Tags searched on startup\n"
          "default_tags=\n"
          "# Tags appended to every search (e.g. -ai_generated -video)\n"
          "extra_tags=\n"
          "# 1 = verify HTTPS certificates with romfs:/cacert.pem (recommended)\n"
          "verify_ssl=1\n"
          "# 1 = log full connection/TLS/header details to log.txt (for debugging)\n"
          "verbose_log=0\n"
          "# 1 = viewer starts in fill-screen mode (Y toggles while viewing)\n"
          "fill_screen=0\n", f);
    fclose(f);
}

/* Strip surrounding quotes and keep only characters that can appear in a key/id. */
static void clean_cred(char *v)
{
    char *o = v;
    for (char *p = v; *p; p++)
        if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))
            *o++ = *p;
    *o = 0;
}

static void apply_kv(char *k, char *v)
{
    trim(k); trim(v);
    if (!strcmp(k, "api_key") || !strcmp(k, "user_id") || !strcmp(k, "client_secret") || !strcmp(k, "client_id")) {
        clean_cred(v);
        if (!v[0]) return;   /* "api_key=&api_key=XYZ" - ignore the empty one */
        if (!strcmp(k, "api_key") || !strcmp(k, "client_secret")) snprintf(cfg.api_key, sizeof cfg.api_key, "%s", v);
        else                                                       snprintf(cfg.user_id, sizeof cfg.user_id, "%s", v);
    }
    else if (!strcmp(k, "username"))     snprintf(cfg.username, sizeof cfg.username, "%s", v);
    else if (!strcmp(k, "mature"))       cfg.mature = atoi(v) != 0;
    else if (!strcmp(k, "default_tags")) snprintf(cfg.default_tags, sizeof cfg.default_tags, "%s", v);
    else if (!strcmp(k, "filter_id"))    { clean_cred(v); snprintf(cfg.filter_id, sizeof cfg.filter_id, "%s", v); }
    else if (!strcmp(k, "extra_tags"))   snprintf(cfg.extra_tags, sizeof cfg.extra_tags, "%s", v);
    else if (!strcmp(k, "verify_ssl"))   cfg.verify_ssl = atoi(v) != 0;
    else if (!strcmp(k, "verbose_log"))  cfg.verbose_log = atoi(v) != 0;
    else if (!strcmp(k, "fill_screen"))  cfg.fill_screen = atoi(v) != 0;
    else if (!strcmp(k, "sort"))         snprintf(cfg.sort, sizeof cfg.sort, "%s", v);
    else if (!strcmp(k, "refresh_token")) { clean_cred(v); snprintf(cfg.da_refresh, sizeof cfg.da_refresh, "%s", v); }
    else if (!strcmp(k, "redirect_uri"))  snprintf(cfg.redirect_uri, sizeof cfg.redirect_uri, "%s", v);
    else if (!strcmp(k, "phpsessid"))    {                    /* pixiv login cookie: digits_letters */
        char *o = v;
        for (char *c = v; *c; c++) if ((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '_') *o++ = *c;
        *o = 0;
        snprintf(cfg.api_key, sizeof cfg.api_key, "%s", v);
    }
    else log_msg("config: unknown key '%s'", k);
}

static void load_config(void)
{
    FILE *f = fopen(CFG_PATH, "r");
    if (!f) { log_msg("config: none found, writing default %s", CFG_PATH); write_default_config(); return; }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        if ((u8)s[0] == 0xEF && (u8)s[1] == 0xBB && (u8)s[2] == 0xBF) s += 3;   /* Notepad BOM */
        trim(s);
        if (!s[0] || s[0] == '#') continue;
        /* Accept the credentials string exactly as copied from the site
         * ("&api_key=...&user_id=..."), on its own line or after "api_key=". */
        if ((strstr(s, "api_key=") || strstr(s, "client_secret=")) && strchr(s, '&')) {
            char *save = NULL;
            for (char *part = strtok_r(s, "&", &save); part; part = strtok_r(NULL, "&", &save)) {
                char *eq = strchr(part, '=');
                if (!eq) continue;
                *eq = 0;
                apply_kv(part, eq + 1);
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        apply_kv(s, eq + 1);
    }
    fclose(f);
}

/* ---------------------------------------------------------------- sort orders
 * Only what each site's API offers. rule34.xxx and e621 take sort metatags in
 * the search; Derpibooru takes sf/sd parameters; DeviantArt's listings have no
 * sort option at all. */
typedef struct { const char *label, *value; } SortOpt;

static const SortOpt sorts_r34[] = {
    { "Newest", "" }, { "Highest score", "sort:score:desc" }, { "Recently updated", "sort:updated:desc" },
    { "Oldest", "sort:id:asc" }, { "Random", "sort:random" },
};
static const SortOpt sorts_derpi[] = {                     /* sf value, ":asc" for ascending */
    { "Newest", "" }, { "Highest score", "score" }, { "Best (Wilson score)", "wilson_score" },
    { "Most faves", "faves" }, { "Most upvotes", "upvotes" }, { "Most comments", "comment_count" },
    { "Biggest", "pixels" }, { "Oldest", "id:asc" }, { "Random", "random" },
};
static const SortOpt sorts_da[] = { { "Newest (DeviantArt has no other sort)", "" } };
static const SortOpt sorts_e6[] = {
    { "Newest", "" }, { "Highest score", "order:score" }, { "Most favourites", "order:favcount" },
    { "Hot right now", "order:hot" }, { "Most comments", "order:comment" }, { "Biggest", "order:mpixels" },
    { "Longest videos", "order:duration" }, { "Oldest", "order:id_asc" }, { "Random", "order:random" },
};

static const SortOpt sorts_px[] = {                        /* order|mode */
    { "Newest", "" }, { "Oldest", "date|all" }, { "Newest, all ages only", "date_d|safe" },
    { "Newest, R-18 only (needs login)", "date_d|r18" }, { "Popular (pixiv Premium only)", "popular_d|all" },
};

static const SortOpt *site_sorts(int *n)
{
#define SORTS(a) do { *n = (int)(sizeof a / sizeof a[0]); return a; } while (0)
    if (IS_DERPI) SORTS(sorts_derpi);
    if (IS_DA)    SORTS(sorts_da);
    if (IS_E6)    SORTS(sorts_e6);
    if (IS_PX)    SORTS(sorts_px);
    SORTS(sorts_r34);
#undef SORTS
}

static int sort_index(void)
{
    int n;
    const SortOpt *o = site_sorts(&n);
    for (int i = 0; i < n; i++) if (!strcmp(o[i].value, cfg.sort)) return i;
    return 0;
}

static void log_mem(const char *when)
{
    log_msg("mem (%s): app heap free %lu KB, linear free %lu KB", when,
            (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024),
            (unsigned long)(linearSpaceFree() / 1024));
}

/* ---------------------------------------------------------------- shared state */

typedef enum { S_EMPTY, S_LOADING, S_CPU, S_GPU, S_FAIL } SlotState;

typedef struct {
    SlotState st;
    uint8_t *rgba;          /* owned by worker until st == S_CPU, then by main */
    int w, h;
    int ntiles;             /* vertical strips; thumbnails always use 1 */
    C3D_Tex tex[MAX_TILES];
    Tex3DS_SubTexture sub[MAX_TILES];
    C2D_Image img[MAX_TILES];
    char err[64];
} Slot;

static LightLock lock;
static LightLock site_lock;     /* held by the worker while it handles a request; the site
                                 * tab takes it to swap config/paths safely */
static LightEvent wake;
static volatile bool running = true;
static volatile bool net_ok;
static char neterr[160];

/* bumped by main to invalidate in-flight work */
static volatile u32 list_gen;   /* new search/page: cancels list + thumbs + full */
static volatile u32 full_gen;   /* new full-image request: cancels full */
static volatile u32 full2_gen;  /* new bottom-screen image request: cancels full2 */

/* requests (written by main under lock) */
static bool   req_list;
static Source req_src;
static char   req_tags[512];
static int    req_page;
static int    req_full = -1;    /* post index to load full-size */
static int    req_full2 = -1;   /* post index for the bottom-screen image */
static bool   req_save;
static Post   req_save_post;
static bool   req_delete;
static unsigned *req_ids;       /* gallery folder filter: copy owned by whoever holds it */
static int    req_nids;
static bool   req_use_ids;
static unsigned req_shuffle;    /* gallery shuffle seed, 0 = normal order */
static unsigned gal_shuffle;    /* main thread: gallery shuffle seed (all sites), 0 = off */
static Post   req_delete_post;
static char   req_da_code[200];  /* DeviantArt sign-in: authorization code to exchange */

/* results (written by worker under lock) */
static Post  sh_posts[PER_PAGE];
static int   sh_count;
static int   sh_total = -1;     /* saved library size; -1 = unknown (online) */
static int   sh_page;           /* page actually returned (saved mode clamps) */
static u32   sh_list_version;
static int   sh_list_state;     /* 0 idle, 1 loading, 2 ok, 3 error */
static char  sh_list_err[160];
static Slot  thumbs[PER_PAGE];
static Slot  full;
static int   full_index = -1;
static Slot  full2;             /* second image, shown on the bottom screen */
static int   full2_index = -1;
static char  sh_toast[160];
static u32   sh_toast_version;

static void toast(const char *fmt, const char *a)
{
    LightLock_Lock(&lock);
    snprintf(sh_toast, sizeof sh_toast, fmt, a);
    sh_toast_version++;
    LightLock_Unlock(&lock);
}

/* ---------------------------------------------------------------- worker */

typedef struct { u32 lgen, fgen; int use_fgen; } AbortSnap;   /* use_fgen: 1 full, 2 full2 */

static int should_abort(void *ud)
{
    AbortSnap *s = ud;
    if (!running) return 1;
    if (s->lgen != list_gen) return 1;
    if (s->use_fgen == 1 && s->fgen != full_gen) return 1;
    if (s->use_fgen == 2 && s->fgen != full2_gen) return 1;
    return 0;
}

static Post w_posts[PER_PAGE];   /* worker's own copy of the current page */
static int  w_count;

static int is_local(const char *u) { return !strncmp(u, "sdmc:", 5); }
static int is_gif(const char *u)   { return !strcasecmp(url_ext(u), ".gif"); }
static int is_mp4(const char *u)
{
    const char *x = url_ext(u);
    return !strcasecmp(x, ".mp4") || !strcasecmp(x, ".m4v") || !strcasecmp(x, ".mov");
}
static int is_webm(const char *u)  { return !strcasecmp(url_ext(u), ".webm"); }

static const unsigned no_ids = 0;     /* non-NULL "empty folder" set */
static int sh_more = -1;              /* online: more results after this page? -1 unknown */

/* ---------------------------------------------------------------- DeviantArt API (worker thread)
 * App-only OAuth (client credentials) -> 1 h bearer token, renewed as needed.
 * Search text routes to an endpoint:
 *   fav:name -> /collections/all (that user's favourites)   by:name -> /gallery/all
 *   word     -> /browse/tags (one tag)                       empty   -> your faves, or daily deviations */

static char da_token[512];
static u64  da_token_until;
static char da_query[512];
static char da_cursor[64][96];        /* cursor for page N of da_query, if the endpoint uses cursors */

static char da_user[64];                 /* signed-in DeviantArt user ("" = app only) */
static volatile bool da_signin_lost;      /* the saved sign-in stopped working */
static volatile bool da_reauth;           /* main thread signed out: get a new token */

/* DeviantArt wants an https redirect address; it doesn't have to load */
#define DA_DEFAULT_REDIRECT "https://localhost/"
static const char *da_redirect(void) { return cfg.redirect_uri[0] ? cfg.redirect_uri : DA_DEFAULT_REDIRECT; }

/* POST a grant to /oauth2/token with the app's keys in the form. On success sets
 * da_token, saves a new refresh token if one comes back, and returns 0. */
static int da_token_post(const char *grant, AbortSnap *snap, long *http, char *err, size_t errn)
{
    char form[1024];
    snprintf(form, sizeof form, "%s&client_id=%s&client_secret=%s", grant, cfg.user_id, cfg.api_key);
    NetReq rq = { .post = form, .keep_error_body = 1 };
    char *body = NULL; size_t len = 0;
    err[0] = 0;
    int rc = net_request("https://www.deviantart.com/oauth2/token", &rq, &body, &len, 64 * 1024, should_abort, snap, err, errn);
    *http = rq.http;
    Json j;
    int ok = -1;
    if (body && !json_parse(&j, body, len)) {
        if (rc == 0 && json_str(&j, json_get(&j, 0, "access_token"), da_token, sizeof da_token)) {
            int exp = (int)json_num(&j, json_get(&j, 0, "expires_in"), 3600);
            da_token_until = osGetTime() + (u64)(exp > 120 ? exp - 60 : exp) * 1000;
            char rt[200];
            if (json_str(&j, json_get(&j, 0, "refresh_token"), rt, sizeof rt) && rt[0]) {
                snprintf(cfg.da_refresh, sizeof cfg.da_refresh, "%s", rt);
                if (config_set(CFG_PATH, "refresh_token", rt)) log_msg("deviantart: couldn't save the new refresh_token");
            }
            ok = 0;
        } else {
            char d[96] = "";
            if (!json_str(&j, json_get(&j, 0, "error_description"), d, sizeof d))
                json_str(&j, json_get(&j, 0, "error"), d, sizeof d);
            if (d[0]) snprintf(err, errn, "DeviantArt: %s", d);
        }
        json_free(&j);
    }
    free(body);
    if (ok && !err[0]) snprintf(err, errn, "DeviantArt sign-in failed (HTTP %ld)", *http);
    return ok;
}

/* GET /user/whoami with the current token -> da_user */
static void da_whoami(AbortSnap *snap)
{
    char auth[560], err[96], *body = NULL; size_t len = 0;
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", da_token);
    NetReq rq = { .header = { auth, "dA-minor-version: 20210526" } };
    if (net_request("https://www.deviantart.com/api/v1/oauth2/user/whoami", &rq, &body, &len, 64 * 1024, should_abort, snap, err, sizeof err))
        return;
    Json j;
    if (!json_parse(&j, body, len)) {
        json_str(&j, json_get(&j, 0, "username"), da_user, sizeof da_user);
        json_free(&j);
    }
    free(body);
    log_msg("deviantart: signed in as %s", da_user[0] ? da_user : "(unknown)");
}

/* Sign-in step 2 (worker): authorization code -> tokens. Result as a toast. */
static void da_exchange_code(const char *code)
{
    AbortSnap snap = { list_gen, 0, 0 };
    char grant[600], *eru = net_escape(da_redirect()), err[160];
    long http = 0;
    snprintf(grant, sizeof grant, "grant_type=authorization_code&code=%s&redirect_uri=%s", code, eru);
    free(eru);
    if (da_token_post(grant, &snap, &http, err, sizeof err)) {
        log_msg("deviantart: sign-in code refused: %s", err);
        toast("Sign-in failed: %s", err);
        return;
    }
    if (!cfg.da_refresh[0]) { toast("Sign-in failed: %s", "no refresh token came back"); return; }
    da_whoami(&snap);
    if (da_user[0] && !cfg.username[0]) {                /* Faves defaults to you */
        snprintf(cfg.username, sizeof cfg.username, "%s", da_user);
        config_set(CFG_PATH, "username", cfg.username);
    }
    toast("Signed in to DeviantArt as %s", da_user[0] ? da_user : "you");
}

static int da_auth(bool force, AbortSnap *snap, char *err, size_t errn)
{
    if (da_reauth) { da_reauth = false; force = true; }
    if (!force && da_token[0] && osGetTime() < da_token_until) return 0;
    da_token[0] = 0;
    if (cfg.da_refresh[0] && cfg.api_key[0] && cfg.user_id[0]) {      /* signed in: user token */
        char grant[300];
        long http = 0;
        snprintf(grant, sizeof grant, "grant_type=refresh_token&refresh_token=%s", cfg.da_refresh);
        if (da_token_post(grant, snap, &http, err, errn) == 0) {
            log_msg("deviantart: got user token");
            if (!da_user[0]) da_whoami(snap);
            return 0;
        }
        if (should_abort(snap)) return -1;
        if (http == 400 || http == 401) {                /* expired / revoked: back to app-only */
            log_msg("deviantart: saved sign-in no longer works (%s); using app access", err);
            cfg.da_refresh[0] = 0;
            da_user[0] = 0;
            config_set(CFG_PATH, "refresh_token", "");
            da_signin_lost = true;
        } else {
            return -1;                                   /* network trouble: keep the sign-in */
        }
    }
    /* client_id is a number, client_secret 32 hex chars: fix them if entered the wrong way round */
    if (strspn(cfg.api_key, "0123456789") == strlen(cfg.api_key) && strlen(cfg.user_id) == KEY_LEN) {
        char t[64];
        snprintf(t, sizeof t, "%s", cfg.user_id);
        snprintf(cfg.user_id, sizeof cfg.user_id, "%s", cfg.api_key);
        snprintf(cfg.api_key, sizeof cfg.api_key, "%s", t);
        log_msg("deviantart: client_id and client_secret were swapped, fixing");
        creds_save(CFG_PATH, cfg.api_key, cfg.user_id);
    }
    char up[640], form[640];
    snprintf(up, sizeof up, "%s:%s", cfg.user_id, cfg.api_key);
    snprintf(form, sizeof form, "grant_type=client_credentials&client_id=%s&client_secret=%s", cfg.user_id, cfg.api_key);
    char *body = NULL; size_t len = 0;
    int rc = -1;
    long http = 0;
    for (int how = 0; how < 2 && !da_token[0]; how++) {
        /* HTTP Basic first (what DeviantArt documents), then the same keys in the form body */
        NetReq rq = { .post = how ? form : "grant_type=client_credentials", .userpwd = how ? NULL : up, .keep_error_body = 1 };
        free(body); body = NULL; len = 0;
        err[0] = 0;
        rc = net_request("https://www.deviantart.com/oauth2/token", &rq, &body, &len, 64 * 1024,
                         should_abort, snap, err, errn);
        http = rq.http;
        if (rc == 0 || (http != 400 && http != 401)) break;
        log_msg("deviantart: token request %s got HTTP %ld", how ? "(form)" : "(basic auth)", http);
    }
    Json j;
    if (body && !json_parse(&j, body, len)) {
        if (rc == 0 && json_str(&j, json_get(&j, 0, "access_token"), da_token, sizeof da_token)) {
            int exp = (int)json_num(&j, json_get(&j, 0, "expires_in"), 3600);
            da_token_until = osGetTime() + (u64)(exp > 120 ? exp - 60 : exp) * 1000;
            log_msg("deviantart: got app token, valid %d s", exp);
            da_user[0] = 0;
        } else {
            char d[96] = "";
            if (!json_str(&j, json_get(&j, 0, "error_description"), d, sizeof d))
                json_str(&j, json_get(&j, 0, "error"), d, sizeof d);
            if (d[0]) snprintf(err, errn, "DeviantArt login: %s (check client_id/client_secret)", d);
            else if (http == 401) snprintf(err, errn, "DeviantArt login refused (HTTP 401): check client_id/client_secret");
        }
        json_free(&j);
    }
    else if (http == 401) snprintf(err, errn, "DeviantArt login refused (HTTP 401): check client_id/client_secret");
    free(body);
    if (!da_token[0] && !err[0]) snprintf(err, errn, "DeviantArt login failed");
    return da_token[0] ? 0 : -1;
}

static void da_route(const char *q, char *ep, size_t n)
{
    char buf[512], *save = NULL, *tag = NULL, *user = NULL;
    const char *kind = NULL;
    snprintf(buf, sizeof buf, "%s", q);
    for (char *t = strtok_r(buf, " ,", &save); t; t = strtok_r(NULL, " ,", &save)) {
        if (!strncmp(t, "fav:", 4) && t[4]) { kind = "/collections/all"; user = t + 4; break; }
        if (!strncmp(t, "by:", 3) && t[3])  { kind = "/gallery/all";     user = t + 3; break; }
        if (!tag && t[0] != '-') tag = t[0] == '#' ? t + 1 : t;
    }
    if (!kind && !tag && cfg.username[0]) { kind = "/collections/all"; user = cfg.username; }
    if (kind) {
        char *e = net_escape(user);
        snprintf(ep, n, "%s?username=%s", kind, e);
        free(e);
    } else if (tag && *tag) {
        char *e = net_escape(tag);
        snprintf(ep, n, "/browse/tags?tag=%s", e);
        free(e);
    } else {
        snprintf(ep, n, "/browse/dailydeviations?");
    }
}

static int da_fetch(const char *q, int page, u32 gen, int *more, char *err, size_t errn)
{
    AbortSnap snap = { gen, 0, 0 };
    if (!cfg.api_key[0] || !cfg.user_id[0]) {
        snprintf(err, errn, "DeviantArt needs your app's client_id and client_secret");
        return -1;
    }
    if (strcmp(q, da_query)) {                      /* new search: forget cursors */
        snprintf(da_query, sizeof da_query, "%s", q);
        memset(da_cursor, 0, sizeof da_cursor);
    }
    char ep[640], url[1024];
    da_route(q, ep, sizeof ep);
    bool cur = page > 0 && page < 64 && da_cursor[page][0];
    snprintf(url, sizeof url, "https://www.deviantart.com/api/v1/oauth2%s&limit=%d&mature_content=%s%s%s%s%d",
             ep, PER_PAGE, cfg.mature ? "true" : "false",
             cur ? "&cursor=" : "", cur ? da_cursor[page] : "", cur ? "" : "&offset=", cur ? 0 : page * PER_PAGE);
    if (cur) { char *z = strstr(url, "&offset="); if (z) *z = 0; }
    log_msg("search: '%s' page %d -> %s", q, page + 1, ep);

    for (int attempt = 0; attempt < 2; attempt++) {
        err[0] = 0;
        if (da_auth(attempt > 0, &snap, err, errn)) return -1;
        char auth[560];
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", da_token);
        NetReq rq = { .header = { auth, "dA-minor-version: 20210526" }, .keep_error_body = 1 };
        char *body = NULL; size_t len = 0;
        int rc = net_request(url, &rq, &body, &len, LIST_MAX_BYTES, should_abort, &snap, err, errn);
        if (rc && rq.http == 401 && attempt == 0) { free(body); log_msg("deviantart: token rejected, renewing"); continue; }
        if (rc && rq.http == 429) { free(body); snprintf(err, errn, "DeviantArt is rate-limiting requests - wait a minute"); return -1; }
        if (!body) {
            if (rq.http == 401) snprintf(err, errn, "DeviantArt refused the app token (HTTP 401)");
            else if (rq.http == 403) snprintf(err, errn, "DeviantArt blocked the request (HTTP 403)");
            return -1;
        }
        char next_cursor[96], aerr[96];
        int next_offset = -1;
        int n = parse_posts_da(body, len, w_posts, PER_PAGE, more, &next_offset, next_cursor, sizeof next_cursor, aerr, sizeof aerr);
        if (n < 0) {
            if (aerr[0]) snprintf(err, errn, "DeviantArt: %s", aerr);
            else if (!err[0]) snprintf(err, errn, "DeviantArt: unexpected reply %.60s", body);
            log_msg("search: %s", err);
        } else {
            if (next_cursor[0] && page + 1 < 64) snprintf(da_cursor[page + 1], sizeof da_cursor[0], "%s", next_cursor);
            log_msg("search: %d deviations, more %d%s", n, *more, next_cursor[0] ? " (cursor paging)" : "");
        }
        free(body);
        return n;
    }
    return -1;
}

/* ---------------------------------------------------------------- pixiv (worker thread)
 * The website's own JSON: tag search (60 a page), ranking (50 a page) and a
 * user's public bookmarks. The app shows 12 a page, so the last reply is kept
 * and sliced. Login (optional) is the PHPSESSID cookie.
 *   fav:ID -> that user's bookmarks   ranking[:weekly|monthly|...] / empty -> ranking
 *   anything else -> tag search (space = AND, -tag excludes) */

static volatile bool px_cache_drop;    /* login changed: don't reuse the last reply */
static char  *px_cache_body;
static size_t px_cache_len;
static char   px_cache_url[1024];

static const char *px_cookie(char *buf, size_t n)
{
    if (!cfg.api_key[0]) return NULL;
    snprintf(buf, n, "PHPSESSID=%s", cfg.api_key);
    return buf;
}

/* user id for Faves: username= if numeric, else the digits the cookie starts with */
static bool px_user_id(char *out, size_t n)
{
    if (cfg.username[0] && strspn(cfg.username, "0123456789") == strlen(cfg.username)) { snprintf(out, n, "%s", cfg.username); return true; }
    size_t d = strspn(cfg.api_key, "0123456789");
    if (d && cfg.api_key[d] == '_') { snprintf(out, n, "%.*s", (int)d, cfg.api_key); return true; }
    return false;
}

static int px_get(const char *url, AbortSnap *snap, const char **body, size_t *len, char *err, size_t errn)
{
    if (px_cache_drop) { px_cache_drop = false; px_cache_url[0] = 0; }
    if (px_cache_body && !strcmp(url, px_cache_url)) { *body = px_cache_body; *len = px_cache_len; return 0; }
    free(px_cache_body); px_cache_body = NULL; px_cache_url[0] = 0;
    char ck[600];
    NetReq rq = { .cookie = px_cookie(ck, sizeof ck), .keep_error_body = 1 };
    char *b = NULL; size_t l = 0;
    int rc = net_request(url, &rq, &b, &l, LIST_MAX_BYTES, should_abort, snap, err, errn);
    if (rc) {
        char perr[96] = "";
        int t, m;
        if (b) parse_posts_pixiv(b, l, 1, 0, w_posts, 0, &t, &m, perr, sizeof perr);
        if (perr[0]) snprintf(err, errn, "pixiv: %.100s", perr);
        else if (rq.http == 403) snprintf(err, errn, "pixiv refused the request (HTTP 403) - try again later, or log in");
        else if (rq.http == 401) snprintf(err, errn, "pixiv: not logged in (HTTP 401) - set your PHPSESSID cookie");
        if (b) log_msg("search: pixiv HTTP %ld: %.200s", rq.http, b);
        free(b);
        return -1;
    }
    px_cache_body = b; px_cache_len = l;
    snprintf(px_cache_url, sizeof px_cache_url, "%s", url);
    *body = b; *len = l;
    return 0;
}

static int px_fetch(const char *q, int page, u32 gen, int *more, int *total, char *err, size_t errn)
{
    AbortSnap snap = { gen, 0, 0 };
    char word[512] = "", fav[32] = "", rmode[32] = "";
    char buf[512], *save = NULL;
    snprintf(buf, sizeof buf, "%s", q);
    for (char *t = strtok_r(buf, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
        if (!strncmp(t, "fav:", 4) && t[4]) { snprintf(fav, sizeof fav, "%.30s", t + 4); continue; }
        if (!strcmp(t, "ranking")) { snprintf(rmode, sizeof rmode, "daily"); continue; }
        if (!strncmp(t, "ranking:", 8) && t[8]) { snprintf(rmode, sizeof rmode, "%.30s", t + 8); continue; }
        size_t o = strlen(word);
        snprintf(word + o, sizeof word - o, "%s%s", o ? " " : "", t);
    }
    for (char *c = word; *c; c++) if (*c == '_') *c = ' ';    /* tags from the picker use _ for spaces */
    char order[16] = "date_d", cmode[8] = "all";
    if (cfg.sort[0]) {
        const char *bar = strchr(cfg.sort, '|');
        snprintf(order, sizeof order, "%.*s", bar ? (int)(bar - cfg.sort) : (int)strlen(cfg.sort), cfg.sort);
        if (bar) snprintf(cmode, sizeof cmode, "%.7s", bar + 1);
    }
    if (!word[0] && !fav[0] && !rmode[0]) snprintf(rmode, sizeof rmode, "daily");
    if (rmode[0] && !strcmp(cmode, "r18") && !strstr(rmode, "r18")) {
        size_t o = strlen(rmode);
        snprintf(rmode + o, sizeof rmode - o, "_r18");
    }

    char url[1024];
    const char *body; size_t len;
    int n = 0, t = -1, m = 0;
    *more = 0; *total = -1;
    if (fav[0]) {                                            /* bookmarks: offset/limit fit exactly */
        if (strspn(fav, "0123456789") != strlen(fav)) { snprintf(err, errn, "pixiv: fav: needs a user ID number (fav:12345)"); return -1; }
        snprintf(url, sizeof url, "https://www.pixiv.net/ajax/user/%s/illusts/bookmarks?tag=&offset=%d&limit=%d&rest=show&lang=en",
                 fav, page * PER_PAGE, PER_PAGE);
        log_msg("search: pixiv bookmarks of %s, page %d", fav, page + 1);
        if (px_get(url, &snap, &body, &len, err, errn)) return -1;
        n = parse_posts_pixiv(body, len, 1, 0, w_posts, PER_PAGE, &t, &m, err, errn);
        *total = t;
    } else if (rmode[0]) {                                   /* ranking: 50 a page, may need two */
        int first = page * PER_PAGE;
        while (n < PER_PAGE) {
            int rp = first / 50 + 1, skip = first % 50;
            snprintf(url, sizeof url, "https://www.pixiv.net/ranking.php?mode=%s&content=illust&p=%d&format=json", rmode, rp);
            log_msg("search: pixiv ranking %s, page %d", rmode, rp);
            if (px_get(url, &snap, &body, &len, err, errn)) return n ? n : -1;
            int got = parse_posts_pixiv(body, len, 2, skip, w_posts + n, PER_PAGE - n, &t, &m, err, errn);
            if (got < 0) return n ? n : -1;
            n += got;
            first += got;
            if (n < PER_PAGE && m && got >= 0 && first % 50 == 0) continue;   /* window runs into the next page */
            break;
        }
        *more = m || (n == PER_PAGE);
        return n;
    } else {                                                 /* tag search: 60 a page */
        int first = page * PER_PAGE, sp = first / 60 + 1, skip = first % 60;
        char *ew = net_escape(word);
        snprintf(url, sizeof url, "https://www.pixiv.net/ajax/search/artworks/%s?word=%s&order=%s&mode=%s&p=%d&s_mode=s_tag&type=all&lang=en",
                 ew, ew, order, cmode, sp);
        free(ew);
        log_msg("search: pixiv '%s' %s/%s, pixiv page %d", word, order, cmode, sp);
        if (px_get(url, &snap, &body, &len, err, errn)) return -1;
        n = parse_posts_pixiv(body, len, 0, skip, w_posts, PER_PAGE, &t, &m, err, errn);
        *total = t;
    }
    if (n < 0 && !err[0]) snprintf(err, errn, "pixiv: unexpected reply");
    if (n >= 0) log_msg("search: %d works%s", n, t >= 0 ? " (total known)" : "");
    *more = m;
    return n;
}

static void worker_fetch_list(Source src, const char *tags, int page, u32 gen,
                              const unsigned *ids, int nids, bool use_ids)
{
    char err[160] = "";
    int n = -1, total = -1;
    u64 t0 = osGetTime();

    if (src == SRC_SAVED) {
        n = saved_scan_ids(SAVE_DIR, tags, use_ids ? (ids ? ids : &no_ids) : NULL, nids,
                           &page, w_posts, PER_PAGE, &total, err, sizeof err);
        log_msg("saved: %sfilter '%s' page %d -> %d shown, %d total, %lu ms%s%s",
                use_ids ? "folder, " : "", tags, page + 1, n, total, (unsigned long)(osGetTime() - t0), n < 0 ? ", error: " : "", n < 0 ? err : "");
    } else if (!net_ok) {
        snprintf(err, sizeof err, "Offline: %s", neterr);
    } else {
        if (IS_PX) {
        int more = 0;
        n = px_fetch(tags, page, gen, &more, &total, err, sizeof err);
        LightLock_Lock(&lock);
        if (gen == list_gen) sh_more = n >= 0 && total < 0 ? more : -1;
        LightLock_Unlock(&lock);
        } else if (IS_DA) {
        int more = 0;
        n = da_fetch(tags, page, gen, &more, err, sizeof err);
        LightLock_Lock(&lock);
        if (gen == list_gen) sh_more = n >= 0 ? more : -1;
        LightLock_Unlock(&lock);
        } else {
        char *etags = net_escape(tags[0] ? tags : "*");
        char *url = malloc(2048);
        if (IS_E6)
        snprintf(url, 2048, "https://e621.net/posts.json?limit=%d&page=%d&tags=%s",
                 PER_PAGE, page + 1, tags[0] ? etags : "");
        else if (IS_DERPI)
        snprintf(url, 2048,
                 "https://derpibooru.org/api/v1/json/search/images?q=%s&page=%d&per_page=%d"
                 "%s%.*s%s%s%s%s%s",
                 etags, page + 1, PER_PAGE,
                 cfg.sort[0] ? "&sf=" : "", (int)strcspn(cfg.sort, ":"), cfg.sort,
                 cfg.sort[0] ? (strstr(cfg.sort, ":asc") ? "&sd=asc" : "&sd=desc") : "",
                 cfg.api_key[0] ? "&key=" : "", cfg.api_key,
                 cfg.filter_id[0] ? "&filter_id=" : "", cfg.filter_id);
        else
        snprintf(url, 2048,
                 "https://api.rule34.xxx/index.php?page=dapi&s=post&q=index&json=1"
                 "&limit=%d&pid=%d&tags=%s%s%s%s%s",
                 PER_PAGE, page, tags[0] ? etags : "",
                 cfg.api_key[0] ? "&api_key=" : "", cfg.api_key,
                 cfg.user_id[0] ? "&user_id=" : "", cfg.user_id);
        free(etags);

        AbortSnap snap = { gen, 0, 0 };
        char *body = NULL; size_t len = 0;
        log_msg("search: tags '%s' page %d", tags, page + 1);
        char up[640];                               /* e621: login + API key as HTTP Basic */
        snprintf(up, sizeof up, "%s:%s", cfg.username, cfg.api_key);
        NetReq rq = { .userpwd = IS_E6 && cfg.username[0] && cfg.api_key[0] ? up : NULL, .keep_error_body = IS_E6 };
        int rc = net_request(url, &rq, &body, &len, LIST_MAX_BYTES, should_abort, &snap, err, sizeof err);
        free(url);
        if (rc && IS_E6 && body) {                  /* e621 explains errors in JSON */
            char e6err[120] = "";
            int hid;
            parse_posts_e621(body, len, w_posts, 0, &hid, e6err, sizeof e6err);
            if (rq.http == 401) snprintf(err, sizeof err, "e621 refused your username/API key (HTTP 401)");
            else if (e6err[0]) snprintf(err, sizeof err, "e621: %.110s", e6err);
            log_msg("search: e621 HTTP %ld: %.200s", rq.http, body);
        }
        if (rc == 0) {
            int e6_hidden = 0;
            char e6err[120] = "";
            if (IS_E6)         n = parse_posts_e621(body, len, w_posts, PER_PAGE, &e6_hidden, e6err, sizeof e6err);
            else if (IS_DERPI) n = parse_posts_derpi(body, len, w_posts, PER_PAGE, &total);
            else               n = parse_posts(body, len, w_posts, PER_PAGE);
            if (IS_E6 && e6_hidden) log_msg("search: %d posts hidden by e621 (log in to see them)", e6_hidden);
            if (IS_E6 && n >= 0) {
                LightLock_Lock(&lock);              /* a full page of raw results means there may be more */
                if (gen == list_gen) sh_more = n + e6_hidden >= PER_PAGE;
                LightLock_Unlock(&lock);
            }
            if (IS_E6 && n < 0 && e6err[0]) snprintf(err, sizeof err, "e621: %.110s", e6err);
            if (n < 0 && err[0]) {
                log_msg("search: %s", err);
            } else if (n < 0) {   /* plain-text API error, show the start of it */
                snprintf(err, sizeof err, "API: %.120s", body);
                for (char *c = err; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
                log_msg("search: API returned non-JSON: %.300s", body);
            } else {
                log_msg("search: %d posts parsed from %u bytes%s", n, (unsigned)len,
                        total >= 0 ? " (total known)" : "");
            }
        }
        free(body);
        }
    }

    LightLock_Lock(&lock);
    if (gen == list_gen) {
        if (n >= 0) {
            w_count = n;
            memcpy(sh_posts, w_posts, sizeof w_posts);
            sh_count = n;
            sh_list_state = 2;
        } else {
            w_count = 0;
            sh_count = 0;
            sh_list_state = 3;
            snprintf(sh_list_err, sizeof sh_list_err, "%s", err);
            if (strcmp(err, "cancelled")) log_msg("list failed: %s", err);
        }
        sh_total = total;
        sh_page = page;
        sh_list_version++;
    }
    LightLock_Unlock(&lock);
}

/* Download/read + decode the first URL that works. */
static uint8_t *fetch_image(const char *const *urls, int nurls, int maxw, int maxh,
                            int *w, int *h, AbortSnap *snap, char *err, size_t errn)
{
    for (int i = 0; i < nurls; i++) {
        if (!urls[i] || !urls[i][0] || url_is_video(urls[i])) continue;
        char *body = NULL; size_t len = 0;
        int rc;
        if (is_local(urls[i])) rc = read_file(urls[i], &body, &len, is_gif(urls[i]) ? GIF_MAX_BYTES : IMG_MAX_BYTES, err, errn);
        else if (net_ok)       rc = net_get(urls[i], &body, &len, IMG_MAX_BYTES, should_abort, snap, err, errn);
        else { snprintf(err, errn, "offline"); rc = -1; }
        if (rc) {
            if (is_local(urls[i])) log_msg("read %s: %s", urls[i], err);
            if (should_abort(snap)) return NULL;
            continue;
        }
        const char *derr = NULL;
        uint8_t *px = image_decode_fit((uint8_t *)body, len, maxw, maxh, w, h, &derr);
        free(body);
        if (px) return px;
        snprintf(err, errn, "%s", derr ? derr : "decode failed");
        log_msg("decode %s (%u bytes): %s", urls[i], (unsigned)len, err);
        if (should_abort(snap)) return NULL;
    }
    if (!err[0]) snprintf(err, errn, "no image (video?)");
    return NULL;
}

static void worker_fetch_full(int idx, u32 lgen, u32 fgen)
{
    if (idx < 0 || idx >= w_count) return;
    Post *p = &w_posts[idx];
    const char *urls[3] = { p->sample_url, p->file_url, p->preview_url };
    AbortSnap snap = { lgen, fgen, 1 };
    char err[64] = "";
    int w = 0, h = 0;
    u64 t0 = osGetTime();
    /* gallery GIFs are animated by the GIF player, which renders frames at up to
     * GIF_MAX; load the first frame at the same size so the player can reuse the texture */
    bool anim = is_local(p->file_url) && is_gif(p->file_url);
    uint8_t *px = fetch_image(urls, 3, anim ? GIF_MAX : FULL_MAX_W, anim ? GIF_MAX : FULL_MAX_H,
                              &w, &h, &snap, err, sizeof err);
    if (px) log_msg("view #%u: %dx%d in %lu ms", p->id, w, h, (unsigned long)(osGetTime() - t0));
    else if (strcmp(err, "cancelled")) log_msg("view #%u failed: %s", p->id, err);

    LightLock_Lock(&lock);
    if (lgen == list_gen && fgen == full_gen && full_index == idx) {
        if (px) { full.rgba = px; full.w = w; full.h = h; full.st = S_CPU; px = NULL; }
        else    { full.st = S_FAIL; snprintf(full.err, sizeof full.err, "%s", err); }
    }
    LightLock_Unlock(&lock);
    free(px);
}

static void worker_fetch_full2(int idx, u32 lgen, u32 fgen)
{
    if (idx < 0 || idx >= w_count) return;
    Post *p = &w_posts[idx];
    const char *urls[3] = { p->sample_url, p->file_url, p->preview_url };
    AbortSnap snap = { lgen, fgen, 2 };
    char err[64] = "";
    int w = 0, h = 0;
    uint8_t *px = fetch_image(urls, 3, DUAL_MAX_W, DUAL_MAX_H, &w, &h, &snap, err, sizeof err);
    if (!px && strcmp(err, "cancelled")) log_msg("second image #%u failed: %s", p->id, err);

    LightLock_Lock(&lock);
    if (lgen == list_gen && fgen == full2_gen && full2_index == idx) {
        if (px) { full2.rgba = px; full2.w = w; full2.h = h; full2.st = S_CPU; px = NULL; }
        else    { full2.st = S_FAIL; snprintf(full2.err, sizeof full2.err, "%s", err); }
    }
    LightLock_Unlock(&lock);
    free(px);
}

static void worker_fetch_thumb(int idx, u32 lgen)
{
    Post *p = &w_posts[idx];
    const char *urls[2] = { p->preview_url, p->sample_url };
    AbortSnap snap = { lgen, 0, 0 };
    char err[64] = "";
    int w = 0, h = 0;
    uint8_t *px = fetch_image(urls, 2, THUMB_MAX, THUMB_MAX, &w, &h, &snap, err, sizeof err);
    if (!px && strcmp(err, "cancelled")) log_msg("thumb #%u failed: %s", p->id, err);

    LightLock_Lock(&lock);
    if (lgen == list_gen) {
        Slot *s = &thumbs[idx];
        if (px) { s->rgba = px; s->w = w; s->h = h; s->st = S_CPU; px = NULL; }
        else    { s->st = S_FAIL; snprintf(s->err, sizeof s->err, "%s", err); }
    }
    LightLock_Unlock(&lock);
    free(px);
}

static int save_abort(void *ud) { (void)ud; return !running; }   /* browsing doesn't cancel saves */

/* replace the first `from` in s (capacity cap) with `to` */
static void repl1(char *s, size_t cap, const char *from, const char *to)
{
    char *p = strstr(s, from);
    if (!p) return;
    size_t fl = strlen(from), tl = strlen(to), rest = strlen(p + fl);
    if ((size_t)(p - s) + tl + rest + 1 > cap) return;
    memmove(p + tl, p + fl, rest + 1);
    memcpy(p, to, tl);
}

static void worker_save(Post *p)
{
    char path[160], part[168], err[96] = "";
    struct stat st;
    /* what to download, best first. pixiv: the original (PNG or JPEG - the
     * listing doesn't say which), then the 1200px version shown in the viewer */
    static char cand[3][1536];
    int nc = 0;
    if (IS_PX && strstr(p->file_url, "/img-master/") && strstr(p->file_url, "_master1200.")) {
        for (int e = 0; e < 2; e++, nc++) {
            snprintf(cand[nc], sizeof cand[0], "%s", p->file_url);
            repl1(cand[nc], sizeof cand[0], "/img-master/", "/img-original/");
            repl1(cand[nc], sizeof cand[0], "_master1200.jpg", e ? ".jpg" : ".png");
        }
    }
    snprintf(cand[nc++], sizeof cand[0], "%s", p->file_url);
    if (!net_ok) { toast("Can't save while offline%s", ""); return; }
    for (int i = 0; i < nc; i++) {
        snprintf(path, sizeof path, "%s/%u%s", save_dir, p->id, url_ext(cand[i]));
        if (stat(path, &st) == 0) { toast("Already in gallery: %s", SAVE_NAME(path)); return; }
    }
    snprintf(path, sizeof path, "%s/%u%s", save_dir, p->id, url_ext(cand[0]));
    toast("Saving %s ...", SAVE_NAME(path));
    u64 t0 = osGetTime();
    int ok = -1;
    for (int i = 0; i < nc && ok; i++) {
        snprintf(path, sizeof path, "%s/%u%s", save_dir, p->id, url_ext(cand[i]));
        snprintf(part, sizeof part, "%s.part", path);
        log_msg("save #%u: %s", p->id, cand[i]);
        err[0] = 0;
        ok = net_get_file(cand[i], part, save_abort, NULL, err, sizeof err);
        if (ok) { remove(part); if (i + 1 < nc) log_msg("save #%u: %s, trying the next version", p->id, err); }
    }
    const char *name = SAVE_NAME(path);
    if (ok == 0) {
        if (rename(part, path) != 0) {
            remove(part);
            log_msg("save #%u: rename to %s failed", p->id, path);
            toast("Save failed: %s", "rename error");
            return;
        }
        if (saved_write_meta(SAVE_DIR, p)) log_msg("save #%u: couldn't write info file", p->id);
        if (p->preview_url[0]) {
            char tp[160], terr[64];
            snprintf(tp, sizeof tp, "%s/%u.jpg", thumb_dir, p->id);
            if (net_get_file(p->preview_url, tp, save_abort, NULL, terr, sizeof terr))
                log_msg("save #%u: thumbnail failed: %s", p->id, terr);
        }
        if (stat(path, &st) == 0)
            log_msg("save #%u: done, %ld KB in %lu ms", p->id, (long)(st.st_size / 1024), (unsigned long)(osGetTime() - t0));
        toast("Saved to gallery: %s", name);
    } else {
        remove(part);
        log_msg("save #%u failed: %s", p->id, err);
        toast("Save failed: %s", err);
    }
}

static void worker_delete(Post *p)
{
    if (saved_delete(SAVE_DIR, p) == 0) {
        log_msg("delete #%u: %s", p->id, p->file_url);
        toast("Deleted %s", SAVE_NAME(p->file_url));
    } else {
        log_msg("delete #%u: remove(%s) failed", p->id, p->file_url);
        toast("Delete failed: %s", SAVE_NAME(p->file_url));
    }
}

/* Handles one queued item. Returns false when there's nothing left to do. */
static bool worker_step(void)
{
    {
        {
            LightLock_Lock(&lock);
            u32 lgen = list_gen, fgen = full_gen;

            if (req_da_code[0]) {
                char code[200];
                snprintf(code, sizeof code, "%s", req_da_code);
                req_da_code[0] = 0;
                LightLock_Unlock(&lock);
                da_exchange_code(code);
                return true;
            }
            if (req_delete) {
                Post p = req_delete_post;
                req_delete = false;
                LightLock_Unlock(&lock);
                worker_delete(&p);
                return true;
            }
            if (req_list) {
                char tags[512]; int page = req_page; Source src = req_src;
                unsigned *ids = req_ids; int nids = req_nids; bool use_ids = req_use_ids;
                saved_set_shuffle(req_src == SRC_SAVED ? req_shuffle : 0);
                req_ids = NULL;
                snprintf(tags, sizeof tags, "%s", req_tags);
                req_list = false;
                LightLock_Unlock(&lock);
                worker_fetch_list(src, tags, page, lgen, ids, nids, use_ids);
                free(ids);
                return true;
            }
            if (sh_list_state != 2 && sh_list_state != 3) {   /* list not ready yet */
                LightLock_Unlock(&lock);
                return false;
            }
            if (req_full >= 0) {
                int idx = req_full;
                req_full = -1;
                LightLock_Unlock(&lock);
                worker_fetch_full(idx, lgen, fgen);
                return true;
            }
            if (req_full2 >= 0) {
                int idx = req_full2;
                u32 f2 = full2_gen;
                req_full2 = -1;
                LightLock_Unlock(&lock);
                worker_fetch_full2(idx, lgen, f2);
                return true;
            }
            if (req_save) {
                Post p = req_save_post;
                req_save = false;
                LightLock_Unlock(&lock);
                worker_save(&p);
                return true;
            }
            int next = -1;
            for (int i = 0; i < w_count; i++)
                if (thumbs[i].st == S_EMPTY) { next = i; break; }
            if (next >= 0) {
                thumbs[next].st = S_LOADING;
                LightLock_Unlock(&lock);
                worker_fetch_thumb(next, lgen);
                return true;
            }
            LightLock_Unlock(&lock);
            return false;
        }
    }
}

static void worker_main(void *arg)
{
    (void)arg;
    log_msg("worker: started");
    while (running) {
        LightEvent_Wait(&wake);
        while (running) {
            LightLock_Lock(&site_lock);
            bool more = worker_step();
            LightLock_Unlock(&site_lock);
            if (!more) break;
        }
    }
    log_msg("worker: stopped");
}

/* ---------------------------------------------------------------- textures (main thread) */

static int pow2(int v) { int p = 8; while (p < v) p <<= 1; return p; }

static inline u32 morton8(u32 x, u32 y)
{
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

/* GPU textures max out at 1024x1024, so taller images are split into
 * horizontal strips of up to TILE_H rows, each its own texture. */
static bool slot_upload(Slot *s)
{
    int W = pow2(s->w);
    int n = (s->h + TILE_H - 1) / TILE_H;
    if (W > 1024 || n > MAX_TILES) return false;
    s->ntiles = 0;
    for (int t = 0; t < n; t++) {
        int y0 = t * TILE_H;
        int th = s->h - y0 < TILE_H ? s->h - y0 : TILE_H;
        int H = pow2(th);
        C3D_Tex *tex = &s->tex[t];
        if (!C3D_TexInit(tex, W, H, GPU_RGBA8)) {
            for (int i = 0; i < s->ntiles; i++) C3D_TexDelete(&s->tex[i]);
            s->ntiles = 0;
            return false;
        }
        s->ntiles = t + 1;
        C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        u32 *dst = (u32 *)tex->data;
        memset(dst, 0, tex->size);
        const u8 *src = s->rgba + (size_t)y0 * s->w * 4;
        for (int y = 0; y < th; y++) {
            u32 rowtile = (u32)(y >> 3) * (W >> 3);
            for (int x = 0; x < s->w; x++, src += 4) {
                u32 c = ((u32)src[0] << 24) | ((u32)src[1] << 16) | ((u32)src[2] << 8) | src[3];
                dst[((rowtile + (x >> 3)) << 6) + morton8(x & 7, y & 7)] = c;
            }
        }
        C3D_TexFlush(tex);
        Tex3DS_SubTexture *sub = &s->sub[t];
        sub->width  = s->w;
        sub->height = th;
        sub->left   = 0.0f;
        sub->top    = 1.0f;
        sub->right  = (float)s->w / W;
        sub->bottom = 1.0f - (float)th / H;
        s->img[t].tex = tex;
        s->img[t].subtex = sub;
    }
    return true;
}

/* caller holds lock */
static void slot_reset(Slot *s)
{
    if (s->st == S_GPU)
        for (int i = 0; i < s->ntiles; i++) C3D_TexDelete(&s->tex[i]);
    s->ntiles = 0;
    if (s->st == S_CPU) free(s->rgba);
    s->rgba = NULL;
    s->st = S_EMPTY;
    s->err[0] = 0;
}

/* ---------------------------------------------------------------- UI state (main thread) */

typedef enum { MODE_GRID, MODE_VIEW, MODE_SETUP, MODE_QR, MODE_HELP, MODE_FOLDERS, MODE_FOLDERPICK,
               MODE_OPTIONS, MODE_FAVTAGS, MODE_DALOGIN, MODE_SORT } Mode;

static Mode   mode = MODE_GRID;
/* An image/video is on screen: the viewer, or the folder picker / controls opened from it. */
static bool viewing_image(void);
static Source src = SRC_ONLINE;
static struct { char tags[256]; int page; } site_nav[SITE_COUNT][2];   /* per site, per source */
#define nav (site_nav[CUR_SITE])
#define cur_tags (nav[src].tags)
#define cur_page (nav[src].page)
static int   sel;
static Post  ui_posts[PER_PAGE];
static int   ui_count;
static int   ui_total = -1;
static int   ui_more = -1;        /* online: site says there are more pages (-1 unknown) */
static u32   ui_list_version;
static int   ui_list_state;
static char  ui_list_err[160];
static int   open_after_load = -1;     /* open this index once the next page arrives */
static char  ui_toast[160];
static u32   ui_toast_version;
static u64   toast_until;
static int   del_confirm_idx = -1;
static u64   del_confirm_until;

static void open_folders(void);
static void open_folder_pick(void);
static void open_faves(void);
static bool want_user_kb;
static bool user_kb_faves;      /* the username keyboard came from the Faves button */
static void site_tab_tap(void);
static void draw_site_tab(void);
static void open_options(void);
static void open_sort(void);
static Mode help_return;
static bool viewing_image(void)
{
    return mode == MODE_VIEW || mode == MODE_FOLDERPICK || (mode == MODE_HELP && help_return == MODE_VIEW);
}

static Folders folders;
static int     cur_folder = -1;     /* gallery: folder being shown, -1 = whole gallery */
static bool    gallery_dirty;       /* folder membership changed while viewing */

static float zoom = 1.0f, pan_x, pan_y;
static bool  fill_mode;
static int   rot;            /* quarter turns clockwise, 0-3; kept between posts */
static C2D_TextBuf tbuf;

static void ui_toast_set(const char *msg)
{
    snprintf(ui_toast, sizeof ui_toast, "%s", msg);
    toast_until = osGetTime() + 3000;
}

/* App-style "tag_one -tag_two" (what the tag picker builds) -> Derpibooru's
 * "tag one, -tag two". Text that already has commas is Derpibooru syntax: kept. */
static void derpi_query(const char *in, char *out, size_t n)
{
    if (strchr(in, ',')) { snprintf(out, n, "%s", in); return; }
    char buf[512];
    snprintf(buf, sizeof buf, "%s", in);
    size_t o = 0;
    out[0] = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, " ", &save); t && o + 1 < n; t = strtok_r(NULL, " ", &save)) {
        for (char *c = t; *c; c++) if (*c == '_') *c = ' ';
        o += snprintf(out + o, n - o, "%s%s", o ? ", " : "", t);
    }
    if (o >= n) out[n - 1] = 0;
}

static void request_list(void)
{
    LightLock_Lock(&lock);
    list_gen++;
    full_gen++;
    for (int i = 0; i < PER_PAGE; i++) slot_reset(&thumbs[i]);
    slot_reset(&full);
    full_index = -1;
    req_full = -1;
    full2_gen++;
    slot_reset(&full2);
    full2_index = -1;
    req_full2 = -1;
    if (src == SRC_ONLINE) {
        if (IS_DERPI) {
            char a[256], b[256];
            derpi_query(cur_tags, a, sizeof a);
            derpi_query(cfg.extra_tags, b, sizeof b);
            snprintf(req_tags, sizeof req_tags, "%s%s%s", a, (a[0] && b[0]) ? ", " : "", b);
        } else {
            const char *so = (IS_R34 || IS_E6) ? cfg.sort : "";      /* sort metatag */
            snprintf(req_tags, sizeof req_tags, "%s%s%s%s%s", cur_tags,
                     (cur_tags[0] && cfg.extra_tags[0]) ? " " : "", cfg.extra_tags,
                     (so[0] && (cur_tags[0] || cfg.extra_tags[0])) ? " " : "", so);
        }
    } else
        snprintf(req_tags, sizeof req_tags, "%s", cur_tags);
    req_src = src;
    free(req_ids);
    req_ids = NULL;
    req_nids = 0;
    req_use_ids = src == SRC_SAVED && cur_folder >= 0 && cur_folder < folders.n;
    req_shuffle = gal_shuffle;
    if (req_use_ids && folders.f[cur_folder].n) {
        req_nids = folders.f[cur_folder].n;
        req_ids = malloc(req_nids * sizeof *req_ids);
        if (req_ids) memcpy(req_ids, folders.f[cur_folder].ids, req_nids * sizeof *req_ids);
        else req_nids = 0;
    }
    req_page = cur_page;
    req_list = true;
    sh_list_state = 1;
    sh_count = 0;
    sh_page = cur_page;
    sh_total = -1;
    sh_more = -1;
    LightLock_Unlock(&lock);
    ui_count = 0;
    ui_list_state = 1;
    sel = 0;
    del_confirm_idx = -1;
    LightEvent_Signal(&wake);
}

static void request_full2(int idx)
{
    LightLock_Lock(&lock);
    full2_gen++;
    slot_reset(&full2);
    full2.st = S_LOADING;
    full2_index = idx;
    req_full2 = idx;
    LightLock_Unlock(&lock);
    LightEvent_Signal(&wake);
}

static void request_full(int idx)
{
    LightLock_Lock(&lock);
    full_gen++;
    slot_reset(&full);
    full.st = S_LOADING;
    full_index = idx;
    req_full = idx;
    LightLock_Unlock(&lock);
    zoom = 1.0f; pan_x = 0;
    pan_y = fill_mode ? 1e6f : 0;   /* clamped once the image arrives: top edge */
    LightEvent_Signal(&wake);
}

static void request_save(int idx)
{
    if (idx < 0 || idx >= ui_count) return;
    LightLock_Lock(&lock);
    req_save_post = ui_posts[idx];
    req_save = true;
    LightLock_Unlock(&lock);
    LightEvent_Signal(&wake);
}

/* Saved mode: first X arms, second X within 3 s deletes. */
static void request_delete(int idx)
{
    if (idx < 0 || idx >= ui_count) return;
    if (del_confirm_idx != idx || osGetTime() > del_confirm_until) {
        char msg[96];
        snprintf(msg, sizeof msg, "Press X again to delete #%u", ui_posts[idx].id);
        ui_toast_set(msg);
        del_confirm_idx = idx;
        del_confirm_until = osGetTime() + 3000;
        return;
    }
    folders_remove_id(&folders, ui_posts[idx].id);
    if (folders_save(&folders, FOLDERS_PATH)) log_msg("folders: save failed");
    LightLock_Lock(&lock);
    req_delete_post = ui_posts[idx];
    req_delete = true;
    LightLock_Unlock(&lock);
    int keep_sel = idx;
    mode = MODE_GRID;
    request_list();               /* worker deletes first, then re-lists */
    sel = keep_sel;               /* clamped when the list arrives */
}

static bool has_next_page(void)
{
    if (ui_total >= 0) return (cur_page + 1) * PER_PAGE < ui_total;   /* gallery, Derpibooru */
    if (src == SRC_ONLINE && ui_more >= 0) return ui_more;              /* DeviantArt */
    return src == SRC_ONLINE && ui_count == PER_PAGE;
}

static void change_page(int delta, int open_idx)
{
    if (cur_page + delta < 0) return;
    if (delta > 0 && !has_next_page()) return;
    cur_page += delta;
    open_after_load = open_idx;
    request_list();
}

static bool want_page_kb;

/* Page number in the top bar: type a page to jump to. */
static void open_page_kb(void)
{
    int pages = ui_total >= 0 ? (ui_total ? (ui_total + PER_PAGE - 1) / PER_PAGE : 1) : 0;
    char hint[48], out[16];
    if (pages) snprintf(hint, sizeof hint, "go to page (1-%d)", pages);
    else snprintf(hint, sizeof hint, "go to page");
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, 5);
    swkbdSetHintText(&kb, hint);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (swkbdInputText(&kb, out, sizeof out) != SWKBD_BUTTON_CONFIRM) return;
    int n = atoi(out);
    if (n < 1) { ui_toast_set("Pages start at 1"); return; }
    if (pages && n > pages) { n = pages; ui_toast_set("Past the end - showing the last page"); }
    if (n - 1 == cur_page) return;
    log_msg("ui: jump to page %d", n);
    cur_page = n - 1;
    open_after_load = -1;
    request_list();
}

/* Gallery shuffle button: shuffle (a new random order each time) / back to normal */
static void toggle_shuffle(void)
{
    if (gal_shuffle) {
        gal_shuffle = 0;
        ui_toast_set("Normal order");
    } else {
        gal_shuffle = (unsigned)(osGetTime() * 2654435761u) | 1u;
        ui_toast_set("Shuffled - tap again for normal order");
    }
    log_msg("ui: gallery shuffle %s", gal_shuffle ? "on" : "off");
    cur_page = 0;
    open_after_load = -1;
    request_list();
}

static void toggle_source(void)
{
    src = (src == SRC_ONLINE) ? SRC_SAVED : SRC_ONLINE;
    mode = MODE_GRID;
    open_after_load = -1;
    log_msg("ui: switched to %s", src == SRC_SAVED ? "gallery" : "online");
    request_list();
}

/* Pull results from the worker and turn decoded pixels into textures.
 * Must run after C3D_FrameBegin(SYNCDRAW) so no texture is in flight. */
static void sync_worker(void)
{
    LightLock_Lock(&lock);
    if (sh_list_version != ui_list_version) {
        ui_list_version = sh_list_version;
        memcpy(ui_posts, sh_posts, sizeof ui_posts);
        ui_count = sh_count;
        ui_total = sh_total;
        ui_more = sh_more;
        ui_list_state = sh_list_state;
        cur_page = sh_page;
        snprintf(ui_list_err, sizeof ui_list_err, "%s", sh_list_err);
        if (sel >= ui_count) sel = ui_count ? ui_count - 1 : 0;
        LightEvent_Signal(&wake);   /* start thumbnails */
    }
    if (sh_toast_version != ui_toast_version) {
        ui_toast_version = sh_toast_version;
        ui_toast_set(sh_toast);
    }
    for (int i = 0; i < PER_PAGE; i++) {
        Slot *s = &thumbs[i];
        if (s->st == S_CPU) {
            bool ok = slot_upload(s);
            free(s->rgba); s->rgba = NULL;
            s->st = ok ? S_GPU : S_FAIL;
            if (!ok) { snprintf(s->err, sizeof s->err, "texture alloc failed"); log_msg("thumb %d: texture alloc failed (%dx%d)", i, s->w, s->h); }
        }
    }
    if (full2.st == S_CPU) {
        bool ok = slot_upload(&full2);
        free(full2.rgba); full2.rgba = NULL;
        full2.st = ok ? S_GPU : S_FAIL;
        if (!ok) snprintf(full2.err, sizeof full2.err, "texture alloc failed");
    }
    if (full.st == S_CPU) {
        bool ok = slot_upload(&full);
        free(full.rgba); full.rgba = NULL;
        full.st = ok ? S_GPU : S_FAIL;
        if (!ok) {
            snprintf(full.err, sizeof full.err, "texture alloc failed");
            log_msg("view: texture alloc failed (%dx%d)", full.w, full.h);
            log_mem("texture fail");
        }
    }
    LightLock_Unlock(&lock);

    if (open_after_load >= 0 && ui_list_state >= 2 && ui_list_version) {
        if (ui_count > 0) {
            sel = open_after_load < ui_count ? open_after_load : ui_count - 1;
            request_full(sel);
        } else {
            mode = MODE_GRID;
        }
        open_after_load = -1;
    }
}

/* ---------------------------------------------------------------- drawing helpers */

static void text(float x, float y, float sz, u32 col, const char *s)
{
    C2D_Text t;
    C2D_TextParse(&t, tbuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, sz, sz, col);
}

static void text_wrap(float x, float y, float sz, u32 col, float width, const char *s)
{
    C2D_Text t;
    C2D_TextParse(&t, tbuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | C2D_WordWrap, x, y, 0.5f, sz, sz, col, width);
}

static void text_center(float cx, float y, float sz, u32 col, const char *s)
{
    C2D_Text t;
    C2D_TextParse(&t, tbuf, s);
    C2D_TextOptimize(&t);
    float w;
    C2D_TextGetDimensions(&t, sz, sz, &w, NULL);
    C2D_DrawText(&t, C2D_WithColor, cx - w / 2, y, 0.5f, sz, sz, col);
}

static void run_search(const char *q);
static void draw_button(float x, float y, float w, float h, u32 bg, u32 fg, const char *label, float sz)
{
    C2D_DrawRectSolid(x, y, 0.5f, w, h, bg);
    text_center(x + w / 2, y + (h - 30 * sz) / 2, sz, fg, label);
}

/* 3.5" floppy "save" icon, s x s at (x, y): body with a cut corner, metal
 * shutter at the top, white label at the bottom. */
static void draw_disk_icon(float x, float y, float s, u32 body, u32 shutter, u32 label)
{
    float c = s * 0.2f;                                         /* cut top-right corner */
    C2D_DrawRectSolid(x, y, 0.5f, s - c, s, body);
    C2D_DrawRectSolid(x + s - c, y + c, 0.5f, c, s - c, body);
    C2D_DrawTriangle(x + s - c, y, body, x + s, y + c, body, x + s - c, y + c, body, 0.5f);
    C2D_DrawRectSolid(x + s * 0.25f, y, 0.5f, s * 0.45f, s * 0.34f, shutter);
    C2D_DrawRectSolid(x + s * 0.52f, y + s * 0.05f, 0.5f, s * 0.1f, s * 0.24f, body);   /* shutter slot */
    C2D_DrawRectSolid(x + s * 0.18f, y + s * 0.52f, 0.5f, s * 0.64f, s * 0.38f, label);
}

/* Photo album icon, s wide (0.75 s tall) at (x, y): a picture (sun + hills)
 * in front of a second one. bg is the colour behind it. */
static void draw_album_icon(float x, float y, float s, u32 c, u32 bg)
{
    float h = s * 0.62f, w = s * 0.82f;
    C2D_DrawRectSolid(x + s * 0.18f, y, 0.5f, w, h, c);                          /* back photo */
    C2D_DrawRectSolid(x + s * 0.18f + 1, y + 1, 0.5f, w - 2, h - 2, bg);
    float fx = x, fy = y + s * 0.14f;                                             /* front photo */
    C2D_DrawRectSolid(fx, fy, 0.5f, w, h, c);
    C2D_DrawRectSolid(fx + 1.5f, fy + 1.5f, 0.5f, w - 3, h - 3, bg);
    C2D_DrawTriangle(fx + 2, fy + h - 2, c, fx + w * 0.38f, fy + h * 0.38f, c, fx + w * 0.7f, fy + h - 2, c, 0.5f);
    C2D_DrawTriangle(fx + w * 0.45f, fy + h - 2, c, fx + w * 0.68f, fy + h * 0.55f, c, fx + w - 2, fy + h - 2, c, 0.5f);
    C2D_DrawCircleSolid(fx + w * 0.74f, fy + h * 0.3f, 0.5f, s * 0.07f, c);      /* sun */
}

/* Folder icon, s wide (0.8 s tall) at (x, y). */
static void draw_folder_icon(float x, float y, float s, u32 c)
{
    C2D_DrawRectSolid(x, y, 0.5f, s * 0.45f, s * 0.2f, c);                 /* tab */
    C2D_DrawRectSolid(x, y + s * 0.15f, 0.5f, s, s * 0.65f, c);           /* body */
    C2D_DrawRectSolid(x + 1, y + s * 0.3f, 0.5f, s - 2, 1, C2D_Color32(0, 0, 0, 0x50));
}

static void outline(float x, float y, float w, float h, float t, u32 c)
{
    C2D_DrawRectSolid(x, y, 0.6f, w, t, c);
    C2D_DrawRectSolid(x, y + h - t, 0.6f, w, t, c);
    C2D_DrawRectSolid(x, y, 0.6f, t, h, c);
    C2D_DrawRectSolid(x + w - t, y, 0.6f, t, h, c);
}

static void draw_fit(const C2D_Image *img, int iw, int ih, float x, float y, float bw, float bh)
{
    float s = bw / iw;
    if (bh / ih < s) s = bh / ih;
    C2D_DrawImageAt(*img, x + (bw - iw * s) / 2, y + (bh - ih * s) / 2, 0.5f, NULL, s, s);
}

static const char *spinner(void)
{
    static const char *f[] = { "|", "/", "-", "\\" };
    return f[(osGetTime() / 120) % 4];
}

static void post_info(char *buf, size_t n, const Post *p)
{
    if (p->width > 0)
        snprintf(buf, n, "#%u   %dx%d   score %d   %s%s",
                 p->id, p->width, p->height, p->score, p->rating,
                 url_is_video(p->file_url) ? "   [video]" : "");
    else
        snprintf(buf, n, "#%u%s", p->id, url_is_video(p->file_url) ? "   [video]" : "");
}

static u32 src_color(void) { return src == SRC_SAVED ? C_SAVED : C_ACCENT; }

/* ---------------------------------------------------------------- screens */

static void draw_top_grid(void)
{
    char buf[256];
    if (ui_list_state == 1) {
        snprintf(buf, sizeof buf, "%s %s", src == SRC_SAVED ? "Opening gallery" : "Searching", spinner());
        text_center(200, 110, 0.7f, C_DIM, buf);
        return;
    }
    if (ui_list_state == 3) {
        text_center(200, 60, 0.7f, C_WARN, "Request failed");
        text_wrap(20, 90, 0.5f, C_TEXT, 360, ui_list_err);
        if (KEY_REQUIRED && src == SRC_ONLINE && (!cfg.api_key[0] || (KEY_NEEDS_UID && !cfg.user_id[0])))
        {
            snprintf(buf, sizeof buf, "%s needs %s for searching.", SITE_NAME, IS_DA ? "your app keys" : "an API key");
            text_center(200, 150, 0.5f, C_DIM, buf);
        }
        if (src == SRC_ONLINE) {
            text_center(200, 172, 0.5f, C_ACCENT, "Tap Set API key (or press A) to type or scan one");
            text_center(200, 190, 0.5f, C_SAVED, "Press SELECT to open the gallery");
        }
        char ci[96];
        if (KEY_NEEDS_UID)
            snprintf(ci, sizeof ci, "config: %s %u chars (expect %d), %s %s", IS_DA ? "client_secret" : "api_key",
                     (unsigned)strlen(cfg.api_key), KEY_LEN, IS_DA ? "client_id" : "user_id", cfg.user_id[0] ? cfg.user_id : "(none)");
        else if (IS_PX)
            snprintf(ci, sizeof ci, "config: pixiv login cookie %s", cfg.api_key[0] ? "set" : "not set (optional)");
        else if (IS_E6)
            snprintf(ci, sizeof ci, "config: username %s, API key %s", cfg.username[0] ? cfg.username : "(none)",
                     cfg.api_key[0] ? "set" : "not set (optional)");
        else
            snprintf(ci, sizeof ci, "config: API key %s, filter %s", cfg.api_key[0] ? "set" : "not set (optional)",
                     cfg.filter_id[0] ? cfg.filter_id : "default");
        text(20, 212, 0.42f, C_DIM, ci);
        snprintf(ci, sizeof ci, "details: %s", log_path);
        text(20, 226, 0.42f, C_DIM, ci);
        return;
    }
    if (ui_count == 0) {
        if (src == SRC_SAVED && cur_folder >= 0 && !cur_tags[0]) {
            text_center(200, 96, 0.7f, C_DIM, "This folder is empty");
            text_center(200, 126, 0.45f, C_DIM, "Open an image from the gallery and tap the");
            text_center(200, 142, 0.45f, C_DIM, "folder button (bottom left) to add it here");
        } else if (src == SRC_SAVED && !cur_tags[0]) {
            text_center(200, 100, 0.7f, C_DIM, "Your gallery is empty");
            text_center(200, 130, 0.5f, C_DIM, "Press X on a post while online to save it");
        } else {
            text_center(200, 110, 0.7f, C_DIM, "No results");
        }
        return;
    }
    const Post *p = &ui_posts[sel];
    Slot *t = &thumbs[sel];
    C2D_DrawRectSolid(8, 8, 0.4f, 176, 224, C_PANEL);
    if (t->st == S_GPU) draw_fit(&t->img[0], t->w, t->h, 8, 8, 176, 224);
    else text_center(96, 110, 0.6f, C_DIM, t->st == S_FAIL ? "x" : spinner());

    post_info(buf, sizeof buf, p);
    text_wrap(194, 10, 0.5f, src_color(), 198, buf);
    float ty = 48;
    if (p->title[0]) {                                   /* DeviantArt titles */
        text_wrap(194, 44, 0.5f, C_TEXT, 198, p->title);
        ty = 100;
    }
    if (p->tags[0]) {
        char tags[600];
        snprintf(tags, sizeof tags, "%.596s", p->tags);
        for (char *c = tags; *c; c++) if (*c == '_') *c = ' ';
        text_wrap(194, ty, 0.42f, p->title[0] ? C_DIM : C_TEXT, 198, tags);
    } else if (src == SRC_SAVED) {
        text_wrap(194, 48, 0.42f, C_DIM, 198, SAVE_NAME(p->file_url));
    }
}

/* Scale of the full image on the 400x240 top screen.
 * Fit = whole image visible; fill = image covers the whole screen (cropped). */
/* On-screen width/height of the image after rotation (quarter turns swap them). */
static float rot_w(void) { return (rot & 1) ? full.h : full.w; }
static float rot_h(void) { return (rot & 1) ? full.w : full.h; }

static float view_scale(void)
{
    float sx = 400.0f / rot_w(), sy = 240.0f / rot_h();
    float base = fill_mode ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
    return base * zoom;
}

/* Keep the image covering the screen in any direction where it's larger
 * than the screen, and centred where it's smaller. */
static void clamp_pan(void)
{
    if (full.st != S_GPU) return;
    float s = view_scale();
    float mx = (rot_w() * s - 400.0f) / 2, my = (rot_h() * s - 240.0f) / 2;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (pan_x > mx) pan_x = mx;
    if (pan_x < -mx) pan_x = -mx;
    if (pan_y > my) pan_y = my;
    if (pan_y < -my) pan_y = -my;
}

/* ---------------------------------------------------------------- GIF player
 * Gallery GIFs animate in the viewer. A thread decodes frames one at a time,
 * scales them to fit GIF_MAX and converts them to RGB565 (flattened onto the
 * background colour). On the first pass it also keeps the frames; if the whole
 * GIF fits in GIF_CACHE_BYTES, later loops play from memory with no decoding,
 * otherwise it keeps decoding as it plays. The main thread copies each frame
 * into the already-uploaded full image texture when its delay is up, so
 * fit/fill, zoom and rotation keep working. Online GIFs show their first frame. */

#define GIF_MAX_FRAMES 2000

static struct {
    Thread   th;
    volatile bool run;
    bool     active;
    unsigned post_id, failed_id;
    char     path[256];
    int      tw, th_;            /* frame size = the full image's size */
    LightLock lk;
    const u16 *ready;            /* next frame, handed from thread to main */
    bool     ready_owned;        /* main frees it (streamed) vs cache-owned */
    int      ready_delay;
    bool     has_ready;
    u64      next_time;
    u32      shown;
    bool     cached;             /* for the status line */
} gp = { .failed_id = 0xFFFFFFFFu };

static u16 *gif_to565(const u8 *rgba, int n)
{
    u16 *o = malloc((size_t)n * 2);
    if (!o) return NULL;
    for (int i = 0; i < n; i++, rgba += 4) {
        unsigned a = rgba[3], ia = 255 - a;     /* flatten onto C_BG (16,18,1d) */
        unsigned r = (rgba[0] * a + 0x16 * ia) / 255, g = (rgba[1] * a + 0x18 * ia) / 255,
                 b = (rgba[2] * a + 0x1d * ia) / 255;
        o[i] = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    return o;
}

/* Wait until main has taken a pending frame that points into the cache. */
static void gif_wait_cache_frame_taken(void)
{
    for (;;) {
        LightLock_Lock(&gp.lk);
        bool pending = gp.has_ready && !gp.ready_owned;
        LightLock_Unlock(&gp.lk);
        if (!pending || !gp.run) return;
        svcSleepThread(4 * 1000000LL);
    }
}

/* Wait until main has taken the previous frame, then hand this one over. */
static bool gif_handoff(const u16 *f, bool owned, int delay)
{
    while (gp.run) {
        LightLock_Lock(&gp.lk);
        bool busy = gp.has_ready;
        if (!busy) { gp.ready = f; gp.ready_owned = owned; gp.ready_delay = delay; gp.has_ready = true; }
        LightLock_Unlock(&gp.lk);
        if (!busy) return true;
        svcSleepThread(4 * 1000000LL);
    }
    return false;
}

static void gif_thread(void *arg)
{
    (void)arg;
    char *buf = NULL, err[64];
    size_t len = 0;
    if (read_file(gp.path, &buf, &len, GIF_MAX_BYTES, err, sizeof err)) {
        log_msg("gif: %s: %s", gp.path, err);
        return;
    }
    int w, h;
    GifStream *gs = gif_open((const u8 *)buf, len, &w, &h);
    if (!gs) { log_msg("gif: can't decode %s", gp.path); free(buf); return; }
    int tw, th;
    image_fit_dims(w, h, GIF_MAX, GIF_MAX, &tw, &th);
    if (tw != gp.tw || th != gp.th_) {
        log_msg("gif: size mismatch %dx%d vs %dx%d", tw, th, gp.tw, gp.th_);
        gif_close(gs); free(buf); return;
    }
    size_t fbytes = (size_t)tw * th * 2;
    size_t max_cache = GIF_CACHE_BYTES / fbytes;
    if (max_cache > GIF_MAX_FRAMES) max_cache = GIF_MAX_FRAMES;
    u16 **cache = calloc(max_cache ? max_cache : 1, sizeof *cache);
    int *delays = calloc(max_cache ? max_cache : 1, sizeof *delays);
    int ncache = 0, frames = 0;
    bool caching = cache && delays && max_cache > 0;
    u64 t0 = osGetTime();

    /* pass 1 (and every pass when not cached): decode */
    for (;;) {
        if (!gp.run) goto done;
        const u8 *px;
        int delay;
        int r = gif_next(gs, &px, &delay);
        if (r == 0) {
            if (frames <= 1) goto done;             /* not animated */
            if (caching) break;                     /* whole GIF cached: play from memory */
            gif_rewind(gs);
            continue;
        }
        if (r < 0) { log_msg("gif: decode error after %d frames", frames); goto done; }
        frames++;
        u8 *small = (tw == w && th == h) ? NULL : image_downscale(px, w, h, tw, th);
        if (tw != w && !small) { log_msg("gif: out of memory"); goto done; }
        u16 *f = gif_to565(small ? small : px, tw * th);
        free(small);
        if (!f) { log_msg("gif: out of memory"); goto done; }
        bool keep = false;
        if (caching) {
            if ((size_t)ncache < max_cache) { cache[ncache] = f; delays[ncache] = delay; ncache++; keep = true; }
            else {                                   /* too long to cache: stream from now on */
                log_msg("gif: longer than %u frames at %dx%d, streaming", (unsigned)max_cache, tw, th);
                gif_wait_cache_frame_taken();
                for (int i = 0; i < ncache; i++) free(cache[i]);
                ncache = 0;
                caching = false;
            }
        }
        if (!gif_handoff(f, !keep, delay)) { if (!keep) free(f); goto done; }
    }
    log_msg("gif: %d frames %dx%d -> %dx%d cached (%u KB), first pass %lu ms",
            ncache, w, h, tw, th, (unsigned)(ncache * fbytes / 1024), (unsigned long)(osGetTime() - t0));
    gp.cached = true;
    gif_close(gs); gs = NULL;
    free(buf); buf = NULL;
    for (int i = 0; gp.run; i = (i + 1) % ncache)   /* later loops: from memory */
        if (!gif_handoff(cache[i], false, delays[i])) break;

done:
    /* main may still hold a cache-owned frame: wait until it's taken before freeing.
     * When stopping (run == false) main is blocked in gif_stop joining this thread
     * and drops a cache-owned pending frame without reading it. */
    gif_wait_cache_frame_taken();
    if (gs) gif_close(gs);
    free(buf);
    if (cache) for (int i = 0; i < ncache; i++) free(cache[i]);
    free(cache);
    free(delays);
}

static void gif_stop(void)
{
    if (!gp.active) return;
    gp.run = false;
    threadJoin(gp.th, U64_MAX);
    threadFree(gp.th);
    if (gp.has_ready && gp.ready_owned) free((void *)gp.ready);
    gp.ready = NULL;
    gp.has_ready = false;
    gp.active = false;
    gp.cached = false;
}

/* Copy an RGB565 frame into the full image's (single, same-size) texture. */
static void slot_update565(Slot *s, const u16 *px)
{
    int W = s->tex[0].width;
    u32 *dst = (u32 *)s->tex[0].data;
    for (int y = 0; y < s->h; y++) {
        u32 rowtile = (u32)(y >> 3) * (W >> 3);
        for (int x = 0; x < s->w; x++) {
            u16 c = *px++;
            u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            u32 rgba = ((r << 3 | r >> 2) << 24) | ((g << 2 | g >> 4) << 16) | ((b << 3 | b >> 2) << 8) | 0xFF;
            dst[((rowtile + (x >> 3)) << 6) + morton8(x & 7, y & 7)] = rgba;
        }
    }
    C3D_TexFlush(&s->tex[0]);
}

/* Main thread, after C3D_FrameBegin. */
static void gif_sync(void)
{
    bool want = viewing_image() && src == SRC_SAVED && sel < ui_count &&
                is_local(ui_posts[sel].file_url) && is_gif(ui_posts[sel].file_url) &&
                full.st == S_GPU && full.ntiles == 1;
    unsigned id = sel < ui_count ? ui_posts[sel].id : 0;
    if (gp.active && (!want || gp.post_id != id)) gif_stop();
    if (want && !gp.active && gp.failed_id != id) {
        gp.post_id = id;
        snprintf(gp.path, sizeof gp.path, "%s", ui_posts[sel].file_url);
        gp.tw = full.w;
        gp.th_ = full.h;
        gp.run = true;
        gp.has_ready = false;
        gp.cached = false;
        gp.next_time = 0;
        gp.shown = 0;
        LightLock_Init(&gp.lk);
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        gp.th = threadCreate(gif_thread, NULL, 64 * 1024, prio + 1, 2, false);
        if (!gp.th) gp.th = threadCreate(gif_thread, NULL, 64 * 1024, prio + 1, -2, false);
        if (gp.th) gp.active = true;
        else { gp.failed_id = id; log_msg("gif: thread create failed"); }
    }
    if (!gp.active) return;
    u64 now = osGetTime();
    LightLock_Lock(&gp.lk);
    if (gp.has_ready && now >= gp.next_time) {
        slot_update565(&full, gp.ready);
        if (gp.ready_owned) free((void *)gp.ready);
        gp.ready = NULL;
        gp.has_ready = false;
        int d = gp.ready_delay < 20 ? 100 : gp.ready_delay;   /* browsers treat 0-10 ms as 100 ms */
        gp.next_time = (gp.next_time && now - gp.next_time < 200) ? gp.next_time + d : now + d;
        gp.shown++;
    }
    LightLock_Unlock(&gp.lk);
}

/* ---------------------------------------------------------------- MP4 playback
 * Gallery MP4s (H.264 + AAC) play in the viewer via video.c. Once the player
 * knows the frame size, it takes over the full image slot, so fit/fill, zoom
 * and rotation work on video like on images. */

#define VIDEO_MAX 400

static unsigned vid_post = 0xFFFFFFFFu, vid_failed = 0xFFFFFFFFu;
static bool     vid_tex;
/* Videos don't start by themselves: opening one asks first. */
static unsigned vid_play_id = 0xFFFFFFFFu;     /* the user chose to play this post */
static unsigned vid_skip_id = 0xFFFFFFFFu;     /* ... or said "not now" to it */

static bool local_mp4_selected(void)
{
    return viewing_image() && src == SRC_SAVED && sel < ui_count &&
           is_local(ui_posts[sel].file_url) && is_mp4(ui_posts[sel].file_url);
}

/* Blank single-tile RGBA texture of w x h for frames to be copied into. */
static bool slot_alloc(Slot *s, int w, int h)
{
    s->w = w; s->h = h; s->rgba = NULL;
    int W = pow2(w), H = pow2(h);
    if (W > 1024 || H > 1024 || !C3D_TexInit(&s->tex[0], W, H, GPU_RGBA8)) return false;
    C3D_TexSetFilter(&s->tex[0], GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&s->tex[0], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    u32 *d = (u32 *)s->tex[0].data;
    for (u32 i = 0; i < s->tex[0].size / 4; i++) d[i] = 0x000000FF;   /* opaque black */
    C3D_TexFlush(&s->tex[0]);
    s->ntiles = 1;
    s->sub[0] = (Tex3DS_SubTexture){ (u16)w, (u16)h, 0.0f, 1.0f, (float)w / W, 1.0f - (float)h / H };
    s->img[0].tex = &s->tex[0];
    s->img[0].subtex = &s->sub[0];
    return true;
}

static void video_upload(const uint16_t *px, void *ud)
{
    slot_update565((Slot *)ud, px);
}

static void video_stop(void)
{
    if (vp_running()) vp_stop();
    vid_tex = false;
}

static unsigned sel_id(void) { return sel < ui_count ? ui_posts[sel].id : 0xFFFFFFFEu; }

/* The "Play this video?" prompt is up: a gallery MP4 that hasn't been started or skipped. */
static bool video_prompt(void)
{
    unsigned id = sel_id();
    return mode == MODE_VIEW && local_mp4_selected() && vid_play_id != id && vid_skip_id != id;
}

static void video_play(void)
{
    vid_play_id = sel_id();
    vid_failed = 0xFFFFFFFFu;            /* asking again is a retry */
    log_msg("video: play #%u", vid_play_id);
}

static void video_skip(void)
{
    vid_skip_id = sel_id();
    ui_toast_set("Press A to play the video");
}

/* Main thread, after C3D_FrameBegin. */
static void video_sync(void)
{
    unsigned id = sel_id();
    bool want = local_mp4_selected() && vid_play_id == id;
    if (vp_running() && (!want || vid_post != id)) video_stop();
    if (want && !vp_running() && vid_failed != id) {
        vid_post = id;
        vid_tex = false;
        if (!vp_start(ui_posts[sel].file_url, VIDEO_MAX, VIDEO_MAX)) {
            char msg[96];
            vp_state(msg, sizeof msg);
            vid_failed = id;
            ui_toast_set(msg[0] ? msg : "Couldn't start the video player");
        }
    }
    if (!vp_running()) return;
    int w, h;
    if (!vid_tex && vp_dims(&w, &h)) {
        LightLock_Lock(&lock);
        full_gen++;                                  /* drop the preview the worker may still deliver */
        slot_reset(&full);
        if (slot_alloc(&full, w, h)) full.st = S_GPU;
        else { full.st = S_FAIL; snprintf(full.err, sizeof full.err, "texture alloc failed"); }
        full_index = sel;
        LightLock_Unlock(&lock);
        vid_tex = true;
        zoom = 1.0f; pan_x = 0; pan_y = fill_mode ? 1e6f : 0;
    }
    if (vid_tex && full.st == S_GPU) vp_show_due(video_upload, &full);
}

static void fmt_time(char *b, size_t n, double t)
{
    if (t < 0) t = 0;
    int s = (int)t;
    snprintf(b, n, "%d:%02d", s / 60, s % 60);
}

static void draw_top_view(void)
{
    char buf[200];
    if (video_prompt()) {                            /* preview behind a "Play?" box */
        Slot *t = sel < ui_count ? &thumbs[sel] : NULL;
        if (full.st == S_GPU && full.ntiles == 1) draw_fit(&full.img[0], full.w, full.h, 0, 0, 400, 240);
        else if (t && t->st == S_GPU) draw_fit(&t->img[0], t->w, t->h, 0, 0, 400, 240);
        C2D_DrawRectSolid(80, 80, 0.7f, 240, 80, with_alpha(C_BG, 0xe8));
        outline(80, 80, 240, 80, 2, C_ACCENT);
        text_center(200, 92, 0.6f, C_TEXT, "Play this video?");
        text_center(200, 124, 0.5f, C_DIM, "A: play     B: not now");
        return;
    }
    bool video = sel < ui_count && url_is_video(ui_posts[sel].file_url);
    if (full.st == S_GPU) {
        float s = view_scale();
        float cx = 200 + pan_x, cy = 120 + pan_y;      /* image centre on screen */
        if (rot == 0) {
            float x0 = cx - full.w * s / 2, y0 = cy - full.h * s / 2;
            for (int t = 0; t < full.ntiles; t++) {
                float ty = y0 + t * TILE_H * s, th = full.sub[t].height * s;
                if (ty > 240 || ty + th < 0) continue;  /* strip off screen */
                C2D_DrawImageAt(full.img[t], x0, ty, 0.5f, NULL, s, s);
            }
        } else {
            /* Each strip rotates about its own centre, so place that centre by
             * rotating its offset from the image centre (clockwise, y down). */
            static const float cs[4] = { 1, 0, -1, 0 }, sn[4] = { 0, 1, 0, -1 };
            float c = cs[rot], si = sn[rot], ang = rot * (float)M_PI / 2;
            for (int t = 0; t < full.ntiles; t++) {
                float dy = (t * TILE_H + full.sub[t].height / 2.0f - full.h / 2.0f) * s;
                C2D_DrawImageAtRotated(full.img[t], cx - dy * si, cy + dy * c, 0.5f, ang, NULL, s, s);
            }
        }
    } else if (full.st == S_FAIL) {
        text_center(200, 100, 0.7f, C_WARN, video ? "Video - no preview available" : "Couldn't load image");
        text_center(200, 130, 0.5f, C_DIM, full.err);
    } else {
        Slot *t = (sel < ui_count) ? &thumbs[sel] : NULL;
        if (t && t->st == S_GPU) draw_fit(&t->img[0], t->w, t->h, 0, 0, 400, 240);
        snprintf(buf, sizeof buf, "Loading %s", spinner());
        C2D_DrawRectSolid(150, 108, 0.6f, 100, 24, C_SHADE);
        text_center(200, 112, 0.55f, C_TEXT, buf);
    }
    bool local = video && sel < ui_count && is_local(ui_posts[sel].file_url);
    if (video && vp_running()) {                     /* gallery MP4 */
        char msg[100];
        VpState st = vp_state(msg, sizeof msg);
        if (st == VP_ERROR) {
            C2D_DrawRectSolid(10, 90, 0.7f, 380, 56, C_SHADE);
            text_center(200, 96, 0.55f, C_WARN, "Can't play this video");
            text_wrap(20, 118, 0.42f, C_TEXT, 360, msg);
        } else if (st == VP_STARTING || !vid_tex) {
            C2D_DrawRectSolid(140, 108, 0.6f, 120, 24, C_SHADE);
            snprintf(buf, sizeof buf, "Starting video %s", spinner());
            text_center(200, 112, 0.5f, C_TEXT, buf);
        } else if (vp_paused()) {
            C2D_DrawRectSolid(170, 108, 0.6f, 60, 24, C_SHADE);
            text_center(200, 112, 0.5f, C_TEXT, "PAUSED");
        }
    } else if (video && full.st == S_GPU) {          /* online video or WebM: preview only */
        const char *note = local && is_webm(ui_posts[sel].file_url) ? "WebM can't play on 3DS - preview only"
                         : local && vid_failed == sel_id() ? "Video couldn't play - see the log"
                         : local ? "VIDEO - press A to play" : "VIDEO - save it to play from the gallery";
        C2D_DrawRectSolid(0, 0, 0.7f, 400, 20, C_SHADE);
        text(6, 3, 0.45f, C_WARN, note);
    }
}

static void draw_bar(void)
{
    char buf[300];
    C2D_DrawRectSolid(0, 0, 0.4f, 320, BAR_H, C_PANEL);
    if (src == SRC_SAVED) C2D_DrawRectSolid(0, BAR_H - 2, 0.45f, 320, 2, C_SAVED);
    text(6, 4, 0.55f, cur_page > 0 ? src_color() : C_DIM, "<");
    text(306, 4, 0.55f, has_next_page() ? src_color() : C_DIM, ">");
    bool infolder = src == SRC_SAVED && cur_folder >= 0 && cur_folder < folders.n;
    const char *empty = src == SRC_SAVED ? (infolder ? "(all)" : "(whole gallery)") : "(all posts)";
    char label[64];
    if (infolder) snprintf(label, sizeof label, "%.18s%s: ", folders.f[cur_folder].name,
                           strlen(folders.f[cur_folder].name) > 18 ? ".." : "");
    else snprintf(label, sizeof label, "%s", src == SRC_SAVED ? "GALLERY: " : "");
    int maxc = src == SRC_SAVED ? 33 - (int)strlen(label) : 30;   /* room left after the label */
    if (maxc < 4) maxc = 4;
    snprintf(buf, sizeof buf, "%s%.*s%s", label, maxc,
             cur_tags[0] ? cur_tags : empty, (int)strlen(cur_tags) > maxc ? "..." : "");
    text(22, 5, 0.48f, src == SRC_SAVED ? C_SAVED : C_TEXT, buf);
    if (ui_total >= 0) snprintf(buf, sizeof buf, "%d/%d", cur_page + 1, ui_total ? (ui_total + PER_PAGE - 1) / PER_PAGE : 1);
    else snprintf(buf, sizeof buf, "p%d", cur_page + 1);
    C2D_DrawRectSolid(PAGE_BX, 3, 0.45f, PAGE_BW, BAR_H - 6, C_CELL);      /* tap: jump to a page */
    text_center(PAGE_BX + PAGE_BW / 2, 6, 0.42f, C_TEXT, buf);
}

static void draw_bottom_grid(void)
{
    draw_bar();
    for (int i = 0; i < ui_count; i++) {
        float x = (i % GRID_COLS) * CELL_W, y = BAR_H + (i / GRID_COLS) * CELL_H;
        C2D_DrawRectSolid(x + 2, y + 2, 0.4f, CELL_W - 4, CELL_H - 4, C_CELL);
        Slot *s = &thumbs[i];
        if (s->st == S_GPU) draw_fit(&s->img[0], s->w, s->h, x + 3, y + 3, CELL_W - 6, CELL_H - 6);
        else text_center(x + CELL_W / 2, y + 26, 0.5f, C_DIM, s->st == S_FAIL ? (url_is_video(ui_posts[i].file_url) ? "video" : "x") : spinner());
        if (url_is_video(ui_posts[i].file_url)) {
            C2D_DrawRectSolid(x + 4, y + CELL_H - 18, 0.7f, 22, 14, C_SHADE);
            text(x + 7, y + CELL_H - 18, 0.38f, C_TEXT, "vid");
        }
        if (i == sel) outline(x + 1, y + 1, CELL_W - 2, CELL_H - 2, 2, src_color());
    }
    if (ui_list_state == 2 && ui_count == 0)
        text_center(160, 120, 0.6f, C_DIM, src == SRC_SAVED ? "Gallery empty - SELECT for online" : "No results - Y to search");
    if (ui_list_state == 3) {
        text_center(160, 110, 0.55f, C_WARN, "Error - see top screen");
        if (src == SRC_ONLINE) draw_button(60, 150, 200, 32, C_ACCENT, C_BG, "Set API key (A)", 0.55f);
    }
    C2D_DrawRectSolid(0, 221, 0.5f, 320, 19, with_alpha(C_BG, 0xd0));
    if (src == SRC_SAVED) {
        C2D_DrawRectSolid(FOLD_GX, HELP_GY, 0.5f, FOLD_GW, HELP_GH, C_CELL);
        draw_folder_icon(FOLD_GX + 5, HELP_GY + 3, 14, C_SAVED);
        text(FOLD_GX + 23, HELP_GY + 2, 0.42f, C_TEXT, "Folders");
        if (!MULTI) text(SITE_TX + 2, 225, 0.38f, C_DIM, "Y filter  A open");
        /* Shuffle: two crossing arrows, lit while the gallery is shuffled */
        u32 bg = gal_shuffle ? C_SAVED : C_CELL, fg = gal_shuffle ? C_BG : C_SAVED;
        float x = SORT_GX, y = HELP_GY;
        C2D_DrawRectSolid(x, y, 0.5f, SORT_GW, HELP_GH, bg);
        C2D_DrawLine(x + 4, y + 5, fg, x + 15, y + 13, fg, 2, 0.5f);
        C2D_DrawLine(x + 4, y + 13, fg, x + 15, y + 5, fg, 2, 0.5f);
        C2D_DrawTriangle(x + 20, y + 5, fg, x + 14, y + 2, fg, x + 14, y + 8, fg, 0.5f);
        C2D_DrawTriangle(x + 20, y + 13, fg, x + 14, y + 10, fg, x + 14, y + 16, fg, 0.5f);
    } else {
        if (IS_DA || IS_E6 || IS_PX) {
            C2D_DrawRectSolid(FOLD_GX, HELP_GY, 0.5f, FOLD_GW, HELP_GH, C_CELL);
            text(FOLD_GX + 8, HELP_GY + 2, 0.42f, C_ACCENT, "* Faves");
        }
        if (!MULTI) text(SITE_TX + 2, 225, 0.38f, C_DIM, "Y search  A open");
        /* Sort: up/down arrows, in the accent colour when not the default order */
        u32 sc = cfg.sort[0] ? C_ACCENT : C_TEXT;
        C2D_DrawRectSolid(SORT_GX, HELP_GY, 0.5f, SORT_GW, HELP_GH, C_CELL);
        C2D_DrawTriangle(SORT_GX + 8, HELP_GY + 3, sc, SORT_GX + 3, HELP_GY + 9, sc, SORT_GX + 13, HELP_GY + 9, sc, 0.5f);
        C2D_DrawRectSolid(SORT_GX + 7, HELP_GY + 9, 0.5f, 2, 6, sc);
        C2D_DrawTriangle(SORT_GX + 16, HELP_GY + 15, sc, SORT_GX + 11, HELP_GY + 9, sc, SORT_GX + 21, HELP_GY + 9, sc, 0.5f);
        C2D_DrawRectSolid(SORT_GX + 15, HELP_GY + 3, 0.5f, 2, 6, sc);
    }
    if (MULTI) draw_site_tab();
    C2D_DrawRectSolid(GEAR_GX, HELP_GY, 0.5f, GEAR_GW, HELP_GH, C_CELL);     /* Options: menu icon */
    for (int i = 0; i < 3; i++) C2D_DrawRectSolid(GEAR_GX + 6, HELP_GY + 4 + i * 4.5f, 0.5f, 12, 2, C_TEXT);
    /* album (gallery <-> online) and "?" (controls) */
    C2D_DrawRectSolid(HELP_GX, HELP_GY, 0.5f, ALBUM_GW, HELP_GH, src == SRC_SAVED ? C_SAVED : C_CELL);
    draw_album_icon(HELP_GX + (ALBUM_GW - 20) / 2, HELP_GY + 2, 20, src == SRC_SAVED ? C_BG : C_SAVED,
                    src == SRC_SAVED ? C_SAVED : C_CELL);
    draw_button(HELPQ_GX, HELP_GY, HELPQ_GW, HELP_GH, C_CELL, C_TEXT, "?", 0.5f);
}

/* ---------------------------------------------------------------- tag picker
 * Viewer bottom screen: the current post's tags, each with + (include) and
 * - (exclude) buttons. Picks accumulate across posts into a query builder;
 * "Search" runs it in the current source (online search or saved filter). */

#define VB_INFO_H   22
#define LIST_Y      VB_INFO_H
#define ROW_H       22
#define LIST_H      172                 /* rows area: y 22..194 */
#define QUERY_Y     (LIST_Y + LIST_H)   /* 194..216: query preview (tap to edit) */
#define BTN_Y       (QUERY_Y + 22)      /* 216..240: Clear / Search */
#define PLUS_X      258
#define MINUS_X     288
#define TBTN_W      28
#define MAX_TAGS    256
#define MAX_PICKS   32

static char     tl_buf[1024];
static char    *tl_tags[MAX_TAGS];
static int      tl_n;
static unsigned tl_id = 0xFFFFFFFFu;
static float    tl_scroll;

static struct { char tag[96]; int sign; } qb[MAX_PICKS];   /* sign: +1 include, -1 exclude */
static int qb_n;

/* touch tracking for tap vs drag-to-scroll on the bottom screen */
static bool  tp_active, tp_moved;
static int   tp_x0, tp_y0, tp_x, tp_y;
static float tp_scroll0;

static void tag_list_sync(void)
{
    if (sel >= ui_count) { tl_n = 0; tl_id = 0xFFFFFFFFu; return; }
    if (ui_posts[sel].id == tl_id) return;
    tl_id = ui_posts[sel].id;
    tl_scroll = 0;
    tl_n = 0;
    snprintf(tl_buf, sizeof tl_buf, "%s", ui_posts[sel].tags);
    char *save = NULL;
    for (char *t = strtok_r(tl_buf, " ", &save); t && tl_n < MAX_TAGS; t = strtok_r(NULL, " ", &save))
        tl_tags[tl_n++] = t;
}

static float tag_scroll_max(void)
{
    float m = tl_n * ROW_H - LIST_H;
    return m > 0 ? m : 0;
}

static int qb_find(const char *tag)
{
    for (int i = 0; i < qb_n; i++) if (!strcmp(qb[i].tag, tag)) return i;
    return -1;
}

static int qb_sign(const char *tag)
{
    int i = qb_find(tag);
    return i < 0 ? 0 : qb[i].sign;
}

/* Tap + or -: add with that sign, flip if it had the other sign, remove if same. */
static void qb_toggle(const char *tag, int sign)
{
    int i = qb_find(tag);
    if (i >= 0) {
        if (qb[i].sign == sign) {
            memmove(&qb[i], &qb[i + 1], (qb_n - i - 1) * sizeof qb[0]);
            qb_n--;
        } else {
            qb[i].sign = sign;
        }
        return;
    }
    if (qb_n >= MAX_PICKS) { ui_toast_set("Tag limit reached (32)"); return; }
    snprintf(qb[qb_n].tag, sizeof qb[qb_n].tag, "%s", tag);
    qb[qb_n].sign = sign;
    qb_n++;
}

static void qb_string(char *out, size_t n)
{
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < qb_n && o + 1 < n; i++)
        o += snprintf(out + o, n - o, "%s%s%s", i ? " " : "", qb[i].sign < 0 ? "-" : "", qb[i].tag);
    if (o >= n) out[n - 1] = 0;
}

static void run_search(const char *q)
{
    snprintf(cur_tags, sizeof cur_tags, "%s", q);
    trim(cur_tags);
    cur_page = 0;
    mode = MODE_GRID;
    log_msg("ui: tag picker search '%s'", cur_tags);
    qb_n = 0;
    request_list();
}

/* ---------------------------------------------------------------- second image (bottom screen)
 * From the viewer header button: another post of the page fills the bottom
 * screen. Arrows at the sides step through the page, tapping the middle
 * switches fit/fill, X (top right) or B closes it. The top screen keeps
 * working as usual. */

static bool dual;
static int  dual_sel;
static bool dual_fill;

static void draw_dual_icon(float x, float y, u32 c)     /* two stacked screens, 14x14 */
{
    C2D_DrawRectSolid(x, y, 0.5f, 14, 6, c);
    C2D_DrawRectSolid(x + 2, y + 8, 0.5f, 10, 6, c);
}

static void dual_open(void)
{
    if (ui_count <= 0) return;
    dual = true;
    dual_sel = sel + 1 < ui_count ? sel + 1 : (sel > 0 ? sel - 1 : sel);
    full2_index = -1;                         /* dual_sync() requests it */
    log_msg("ui: second image on, #%d", dual_sel);
}

static void dual_close(void)
{
    dual = false;
    LightLock_Lock(&lock);
    full2_gen++;
    slot_reset(&full2);
    full2_index = -1;
    req_full2 = -1;
    LightLock_Unlock(&lock);
}

static void dual_step(int d)
{
    int n = dual_sel + d;
    if (n < 0 || n >= ui_count) return;
    dual_sel = n;
}

/* every frame: load the image dual_sel points at (also after a page change) */
static void dual_sync(void)
{
    if (!dual) return;
    if (mode != MODE_VIEW && mode != MODE_HELP && mode != MODE_FOLDERPICK) { dual_close(); return; }
    if (ui_list_state != 2 || ui_count <= 0) return;
    if (dual_sel >= ui_count) dual_sel = ui_count - 1;
    if (full2_index != dual_sel) request_full2(dual_sel);
}

static void draw_bottom_dual(void)
{
    char buf[64];
    if (full2.st == S_GPU && full2.ntiles == 1) {
        float s = 320.0f / full2.w, s2 = 240.0f / full2.h;
        s = dual_fill ? (s > s2 ? s : s2) : (s < s2 ? s : s2);
        C2D_DrawImageAt(full2.img[0], 160 - full2.w * s / 2, 120 - full2.h * s / 2, 0.5f, NULL, s, s);
    } else if (full2.st == S_FAIL) {
        text_center(160, 100, 0.55f, C_WARN, "Couldn't load image");
        text_center(160, 124, 0.42f, C_DIM, full2.err);
    } else {
        Slot *t = dual_sel < ui_count ? &thumbs[dual_sel] : NULL;
        if (t && t->st == S_GPU) draw_fit(&t->img[0], t->w, t->h, 0, 0, 320, 240);
        snprintf(buf, sizeof buf, "Loading %s", spinner());
        C2D_DrawRectSolid(110, 108, 0.6f, 100, 24, C_SHADE);
        text_center(160, 112, 0.5f, C_TEXT, buf);
    }
    if (dual_sel < ui_count && url_is_video(ui_posts[dual_sel].file_url)) {
        C2D_DrawRectSolid(DUAL_ARROW_W + 4, 218, 0.7f, 66, 18, C_SHADE);
        text(DUAL_ARROW_W + 8, 220, 0.42f, C_WARN, "video");
    }
    /* side arrows */
    bool can_l = dual_sel > 0, can_r = dual_sel < ui_count - 1;
    for (int side = 0; side < 2; side++) {
        bool on = side ? can_r : can_l;
        float x = side ? 320 - DUAL_ARROW_W : 0;
        C2D_DrawRectSolid(x, 90, 0.7f, DUAL_ARROW_W, 60, C2D_Color32(0, 0, 0, on ? 0x90 : 0x40));
        u32 c = on ? C2D_Color32(0xff, 0xff, 0xff, 0xff) : C2D_Color32(0x9a, 0xa0, 0xab, 0x60);
        float cx = x + DUAL_ARROW_W / 2, d = side ? 7 : -7;
        C2D_DrawTriangle(cx + d, 120, c, cx - d, 106, c, cx - d, 134, c, 0.75f);
    }
    /* position and close */
    snprintf(buf, sizeof buf, "%d/%d", dual_sel + 1, ui_count);
    C2D_DrawRectSolid(138, 2, 0.7f, 44, 18, C_SHADE);
    text_center(160, 4, 0.42f, C_TEXT, buf);
    C2D_DrawRectSolid(294, 2, 0.7f, 24, 20, C_SHADE);
    text_center(306, 3, 0.55f, C_TEXT, "x");
}

static void dual_tap(int x, int y)
{
    if (x >= 288 && y < 26) { dual_close(); return; }
    if (x < DUAL_ARROW_W + 10) { dual_step(-1); return; }
    if (x >= 320 - DUAL_ARROW_W - 10) { dual_step(+1); return; }
    dual_fill = !dual_fill;
}

#define VPROMPT_Y   70
static void draw_video_prompt(void)
{
    C2D_DrawRectSolid(0, 0, 0.8f, 320, 240, C2D_Color32(0x00, 0x00, 0x00, 0xb0));
    C2D_DrawRectSolid(30, VPROMPT_Y, 0.8f, 260, 100, C_PANEL);
    text_center(160, VPROMPT_Y + 10, 0.55f, C_TEXT, "Play this video?");
    draw_button(44, VPROMPT_Y + 46, 110, 40, C_ACCENT, C_BG, "Play (A)", 0.55f);
    draw_button(166, VPROMPT_Y + 46, 110, 40, C_CELL, C_TEXT, "Not now (B)", 0.5f);
}

static void draw_bottom_view(void)
{
    char buf[320];
    tag_list_sync();

    /* tag rows (drawn first; header and footer are painted over the overflow) */
    for (int i = 0; i < tl_n; i++) {
        float y = LIST_Y + i * ROW_H - tl_scroll;
        if (y + ROW_H < LIST_Y || y > QUERY_Y) continue;
        int s = qb_sign(tl_tags[i]);
        u32 rowbg = s > 0 ? tint(C_BG, C_ACCENT, 0.25f)
                  : s < 0 ? tint(C_BG, C_WARN, 0.25f)
                  : (i & 1) ? C_BG : tint(C_BG, C_TEXT, 0.03f);
        C2D_DrawRectSolid(0, y, 0.5f, 316, ROW_H, rowbg);

        char name[64];
        snprintf(name, sizeof name, "%.40s%s", tl_tags[i], strlen(tl_tags[i]) > 40 ? "..." : "");
        for (char *c = name; *c; c++) if (*c == '_') *c = ' ';
        text(6, y + 3, 0.45f, s > 0 ? C_ACCENT : s < 0 ? C_WARN : C_TEXT, name);

        draw_button(PLUS_X, y + 2, TBTN_W - 2, ROW_H - 4, s > 0 ? C_ACCENT : C_CELL,
                    s > 0 ? C_BG : C_ACCENT, "+", 0.6f);
        draw_button(MINUS_X, y + 2, TBTN_W - 2, ROW_H - 4, s < 0 ? C_WARN : C_CELL,
                    s < 0 ? C_BG : C_WARN, "-", 0.6f);
    }
    if (tl_n == 0)
        text_center(160, 90, 0.5f, C_DIM, sel < ui_count ? "This post has no tag info" : "");

    /* scrollbar */
    float smax = tag_scroll_max();
    if (smax > 0) {
        float bh = LIST_H * (float)LIST_H / (tl_n * ROW_H);
        if (bh < 12) bh = 12;
        float by = LIST_Y + (LIST_H - bh) * (tl_scroll / smax);
        C2D_DrawRectSolid(317, by, 0.5f, 3, bh, C_DIM);
    }

    /* header: post info */
    C2D_DrawRectSolid(0, 0, 0.5f, 320, VB_INFO_H, C_PANEL);
    if (vid_tex && vp_running() && vp_state(NULL, 0) == VP_PLAYING) {   /* time + seek bar */
        double t = vp_time(), d = vp_duration();
        char a[16], b[16];
        fmt_time(a, sizeof a, t); fmt_time(b, sizeof b, d);
        snprintf(buf, sizeof buf, "%s %s/%s", vp_paused() ? "||" : ">", a, b);
        text(6, 4, 0.42f, C_ACCENT, buf);
        float f = d > 0 ? (float)(t / d) : 0;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        C2D_DrawRectSolid(SEEK_X, 9, 0.5f, SEEK_W, 4, C_CELL);
        C2D_DrawRectSolid(SEEK_X, 9, 0.5f, SEEK_W * f, 4, C_ACCENT);
        C2D_DrawRectSolid(SEEK_X + SEEK_W * f - 2, 5, 0.5f, 4, 12, C_TEXT);
    } else if (sel < ui_count) {
        post_info(buf, sizeof buf, &ui_posts[sel]);
        text(6, 4, 0.42f, src_color(), buf);
    }
    C2D_DrawRectSolid(DUAL_VX - 4, 0, 0.5f, 320 - DUAL_VX + 4, VB_INFO_H, C_PANEL);  /* keeps info text clear */
    C2D_DrawRectSolid(DUAL_VX, 2, 0.5f, 26, VB_INFO_H - 4, C_CELL);
    draw_dual_icon(DUAL_VX + 6, 4, C_ACCENT);
    draw_button(HELP_VX, 2, 320 - HELP_VX - 2, VB_INFO_H - 4, C_CELL, C_TEXT, "?", 0.5f);

    /* query preview (tap to edit with the keyboard) */
    C2D_DrawRectSolid(0, QUERY_Y, 0.5f, 320, 22, C_PANEL);
    C2D_DrawRectSolid(0, QUERY_Y, 0.5f, 320, 1, C_CELL);
    if (qb_n) {
        char q[300];
        qb_string(q, sizeof q);
        snprintf(buf, sizeof buf, "%.52s%s", q, strlen(q) > 52 ? "..." : "");
        text(6, QUERY_Y + 4, 0.42f, C_TEXT, buf);
    } else {
        text(6, QUERY_Y + 4, 0.42f, C_DIM, "Tap + / - on tags to build a search");
    }

    /* footer: Clear | status | Search */
    C2D_DrawRectSolid(0, BTN_Y, 0.5f, 320, 24, C_BG);
    draw_button(2, BTN_Y + 2, 60, 20, qb_n ? C_CELL : C_PANEL, qb_n ? C_TEXT : C_DIM, "Clear", 0.45f);
    /* save to gallery (already saved when browsing the gallery: dimmed) */
    C2D_DrawRectSolid(SAVE_BX, BTN_Y + 2, 0.5f, SAVE_BW, 20, sel < ui_count ? C_CELL : C_PANEL);
    if (src == SRC_SAVED) draw_folder_icon(SAVE_BX + 5, BTN_Y + 5, 18, C_SAVED);   /* add to folder */
    else draw_disk_icon(SAVE_BX + 6, BTN_Y + 4, 16, C_ACCENT, C_TEXT, C_BG);    /* save to gallery */
    snprintf(buf, sizeof buf, "%s%s%s  Y:%s", gp.active ? "GIF " : "", fill_mode ? "FILL" : "FIT",
             rot == 1 ? " R90" : rot == 2 ? " 180" : rot == 3 ? " L90" : "", fill_mode ? "fit" : "fill");
    text_center(185, BTN_Y + 6, 0.38f, C_DIM, buf);
    draw_button(BACK_BX, BTN_Y + 2, BACK_BW, 20, C_CELL, C_TEXT, "Back", 0.42f);
    snprintf(buf, sizeof buf, qb_n ? "Search (%d)" : "Search", qb_n);
    draw_button(236, BTN_Y + 2, 82, 20, qb_n ? src_color() : C_PANEL, qb_n ? C_BG : C_DIM, buf, 0.45f);
    if (video_prompt()) draw_video_prompt();
}

static void draw_toast(void)
{
    if (!ui_toast[0] || osGetTime() > toast_until) return;
    float y = (mode == MODE_VIEW || mode == MODE_FOLDERS || mode == MODE_FOLDERPICK) ? 0 : 196;  /* keep buttons/lists clear */
    C2D_DrawRectSolid(0, y, 0.8f, 320, 22, C2D_Color32(0x00, 0x00, 0x00, 0xe0));
    text(6, y + 3, 0.45f, src_color(), ui_toast);
}

/* ---------------------------------------------------------------- input */

static void open_search(void)
{
    SwkbdState kb;
    char out[256];
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, sizeof out - 1);
    swkbdSetHintText(&kb, src == SRC_SAVED ? "filter gallery by tags or id (-tag excludes)"
                                           : "tags separated by spaces (-tag excludes)");
    swkbdSetInitialText(&kb, cur_tags);
    swkbdSetValidation(&kb, SWKBD_ANYTHING, 0, 0);
    if (swkbdInputText(&kb, out, sizeof out) == SWKBD_BUTTON_CONFIRM) {
        trim(out);
        snprintf(cur_tags, sizeof cur_tags, "%s", out);
        cur_page = 0;
        mode = MODE_GRID;
        request_list();
    }
}

static void open_post(int idx)
{
    if (idx < 0 || idx >= ui_count) return;
    sel = idx;
    mode = MODE_VIEW;
    request_full(idx);
}

static void step_post(int d)
{
    int n = sel + d;
    if (n < 0) { if (cur_page > 0) change_page(-1, PER_PAGE - 1); return; }
    if (n >= ui_count) { if (has_next_page()) change_page(+1, 0); return; }
    open_post(n);
}

static void x_action(void)
{
    if (src == SRC_SAVED) request_delete(sel);
    else request_save(sel);
}

static bool want_search, want_quit;

static void open_help(void)
{
    help_return = mode;
    mode = MODE_HELP;
}

static void input_grid(u32 down)
{
    if (down & KEY_TOUCH) {                         /* "? Controls" / "Folders" buttons */
        touchPosition t; hidTouchRead(&t);
        if (t.px >= HELPQ_GX - 2 && t.py >= HELP_GY - 2) { open_help(); return; }
        if (t.px >= GEAR_GX - 2 && t.px < GEAR_GX + GEAR_GW + 2 && t.py >= HELP_GY - 2) { open_options(); return; }
        if (t.px >= HELP_GX && t.px < HELP_GX + ALBUM_GW + 2 && t.py >= HELP_GY - 2) { toggle_source(); return; }
        if (src == SRC_SAVED && t.px < FOLD_GX + FOLD_GW + 2 && t.py >= HELP_GY - 2) { open_folders(); return; }
        if ((IS_DA || IS_E6 || IS_PX) && src == SRC_ONLINE && t.px < FOLD_GX + FOLD_GW + 2 && t.py >= HELP_GY - 2) { open_faves(); return; }
        if (MULTI && t.px >= SITE_TX && t.px < SITE_TX + SITE_TW && t.py >= HELP_GY - 2) { site_tab_tap(); return; }
        if (src == SRC_ONLINE && t.px >= SORT_GX - 1 && t.px < SORT_GX + SORT_GW + 2 && t.py >= HELP_GY - 2) { open_sort(); return; }
        if (src == SRC_SAVED && t.px >= SORT_GX - 1 && t.px < SORT_GX + SORT_GW + 2 && t.py >= HELP_GY - 2) { toggle_shuffle(); return; }
    }
    if (down & KEY_Y) want_search = true;
    if (down & KEY_START) want_quit = true;
    if (down & KEY_SELECT) { toggle_source(); return; }
    if (down & KEY_L) change_page(-1, -1);
    if (down & KEY_R) change_page(+1, -1);
    if (src == SRC_ONLINE && ui_list_state == 3) {     /* failed search: offer key setup */
        touchPosition t; hidTouchRead(&t);
        if ((down & KEY_A) || ((down & KEY_TOUCH) && t.px >= 60 && t.px < 260 && t.py >= 150 && t.py < 182)) {
            mode = MODE_SETUP;
            return;
        }
    }
    if (ui_count == 0) return;
    if (down & KEY_DLEFT  && sel > 0) sel--;
    if (down & KEY_DRIGHT && sel < ui_count - 1) sel++;
    if (down & KEY_DUP    && sel >= GRID_COLS) sel -= GRID_COLS;
    if (down & KEY_DDOWN  && sel + GRID_COLS < ui_count) sel += GRID_COLS;
    if (down & KEY_A) open_post(sel);
    if (down & KEY_X) x_action();

    if (down & KEY_TOUCH) {
        touchPosition t; hidTouchRead(&t);
        if (t.py < BAR_H) {
            if (t.px < 22) change_page(-1, -1);
            else if (t.px > 298) change_page(+1, -1);
            else if (t.px >= PAGE_BX - 2) want_page_kb = true;
            else want_search = true;
            return;
        }
        int idx = ((t.py - BAR_H) / CELL_H) * GRID_COLS + t.px / CELL_W;
        if (idx >= 0 && idx < ui_count) {
            if (idx == sel) open_post(idx);
            else sel = idx;
        }
    }
}

static bool want_qedit;

/* Keyboard prefilled with the built query; confirming searches with the edited text. */
static void open_query_edit(void)
{
    SwkbdState kb;
    char q[256], out[256];
    qb_string(q, sizeof q);
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, sizeof out - 1);
    swkbdSetHintText(&kb, "edit search (-tag excludes)");
    swkbdSetInitialText(&kb, q);
    swkbdSetValidation(&kb, SWKBD_ANYTHING, 0, 0);
    if (swkbdInputText(&kb, out, sizeof out) == SWKBD_BUTTON_CONFIRM) run_search(out);
}

static bool want_view_back;     /* viewer Back button tapped */

static void tag_tap(int x, int y)
{
    if (y < VB_INFO_H) {
        if (x >= HELP_VX - 2) open_help();
        else if (x >= DUAL_VX - 4) dual_open();
        else if (vid_tex && vp_running() && x >= SEEK_X - 6 && vp_duration() > 0) {
            float f = (float)(x - SEEK_X) / SEEK_W;
            vp_seek((f < 0 ? 0 : f > 1 ? 1 : f) * vp_duration());
        }
        return;
    }
    if (y >= BTN_Y) {
        if (x < 64) { if (qb_n) { qb_n = 0; ui_toast_set("Search cleared"); } }
        else if (x >= BACK_BX - 1 && x < BACK_BX + BACK_BW + 2) want_view_back = true;
        else if (x >= SAVE_BX - 2 && x < SAVE_BX + SAVE_BW + 2) {
            if (src == SRC_ONLINE) request_save(sel);
            else if (sel < ui_count) open_folder_pick();
        }
        else if (x >= 236 && qb_n) { char q[300]; qb_string(q, sizeof q); run_search(q); }
        return;
    }
    if (y >= QUERY_Y) { if (qb_n) want_qedit = true; return; }

    int row = (int)((y - LIST_Y + tl_scroll) / ROW_H);
    if (row < 0 || row >= tl_n) return;
    if (x >= MINUS_X)      qb_toggle(tl_tags[row], -1);
    else                   qb_toggle(tl_tags[row], +1);   /* + button or the tag name */
}

static void touch_view(u32 down, u32 held, u32 up)
{
    touchPosition t;
    if (down & KEY_TOUCH) {
        hidTouchRead(&t);
        tp_active = true; tp_moved = false;
        tp_x0 = tp_x = t.px; tp_y0 = tp_y = t.py;
        tp_scroll0 = tl_scroll;
    } else if (tp_active && (held & KEY_TOUCH)) {
        hidTouchRead(&t);
        tp_x = t.px; tp_y = t.py;
        bool in_list = tp_y0 >= LIST_Y && tp_y0 < QUERY_Y;
        if (in_list && abs(tp_y - tp_y0) > 6) tp_moved = true;
        if (tp_moved) {
            tl_scroll = tp_scroll0 - (tp_y - tp_y0);
            if (tl_scroll < 0) tl_scroll = 0;
            if (tl_scroll > tag_scroll_max()) tl_scroll = tag_scroll_max();
        }
    }
    if (tp_active && (up & KEY_TOUCH)) {          /* release: a tap if it didn't scroll */
        tp_active = false;
        if (!tp_moved) tag_tap(tp_x, tp_y);
    }
}

static void rotate_view(int dir)
{
    rot = (rot + dir + 4) & 3;
    zoom = 1.0f;
    pan_x = 0;
    pan_y = fill_mode ? 1e6f : 0;
    static const char *names[4] = { "Rotation: normal", "Rotated right (90)", "Rotated 180", "Rotated left (90)" };
    ui_toast_set(names[rot]);
}

static void input_view(u32 down, u32 held, u32 up)
{
    tag_list_sync();
    if (video_prompt()) {                         /* A / B / buttons answer it; the rest still works */
        bool play = down & KEY_A, skip = down & KEY_B;
        if ((down & KEY_TOUCH) && !dual) {
            touchPosition t; hidTouchRead(&t);
            if (t.py >= VPROMPT_Y + 46 && t.py < VPROMPT_Y + 86) {
                if (t.px >= 44 && t.px < 154) play = true;
                else if (t.px >= 166 && t.px < 276) skip = true;
            }
            down &= ~KEY_TOUCH;                   /* the dialog covers the tag list */
        }
        if (play) { video_play(); return; }
        if (skip) { video_skip(); return; }
        down &= ~(KEY_A | KEY_X | KEY_Y);
    }
    if (dual) {
        if (down & KEY_TOUCH) { touchPosition t; hidTouchRead(&t); dual_tap(t.px, t.py); }
    } else
        touch_view(down, held, up);
    if (mode != MODE_VIEW) return;                /* Search was tapped */
    if (want_view_back) { want_view_back = false; down |= KEY_B; }   /* Back button = B */
    if (down & KEY_B) {
        if (dual) dual_close();
        mode = MODE_GRID;
        if (gallery_dirty) { gallery_dirty = false; int k = sel; request_list(); sel = k; }
        return;
    }
    if (down & KEY_START) want_quit = true;
    if (down & KEY_Y) {
        fill_mode = !fill_mode;
        zoom = 1.0f;
        pan_x = 0;
        pan_y = fill_mode ? 1e6f : 0;   /* fill starts at the top of tall images */
        ui_toast_set(fill_mode ? "Fill screen (Y for fit)" : "Fit to screen (Y for fill)");
    }
    if (down & KEY_SELECT) { toggle_source(); return; }
    /* L/R + D-pad Left/Right rotate; L/R alone (on release) or D-pad alone change post. */
    static bool l_combo, r_combo;
    if (down & KEY_L) l_combo = false;
    if (down & KEY_R) r_combo = false;
    bool shoulder = held & (KEY_L | KEY_R);
    if (shoulder && (down & (KEY_DLEFT | KEY_DRIGHT))) {
        rotate_view((down & KEY_DRIGHT) ? +1 : -1);
        if (held & KEY_L) l_combo = true;
        if (held & KEY_R) r_combo = true;
    } else if (!shoulder) {
        if (down & KEY_DLEFT)  step_post(-1);
        if (down & KEY_DRIGHT) step_post(+1);
    }
    if ((up & KEY_L) && !l_combo) step_post(-1);
    if ((up & KEY_R) && !r_combo) step_post(+1);
    if (mode != MODE_VIEW) return;
    if (down & KEY_X) x_action();
    if (down & KEY_A) {
        if (vid_tex && vp_running() && vp_state(NULL, 0) == VP_PLAYING) vp_set_paused(!vp_paused());
        else if (local_mp4_selected() && !vp_running()) video_play();
        else { zoom = 1.0f; pan_x = 0; pan_y = fill_mode ? 1e6f : 0; }
    }
    if (held & (KEY_DUP | KEY_ZR))   zoom *= 1.03f;
    if (held & (KEY_DDOWN | KEY_ZL)) zoom /= 1.03f;
    if (zoom < 0.5f) zoom = 0.5f;
    if (zoom > 8.0f) zoom = 8.0f;

    /* pan: Circle Pad, or the C-stick on New 3DS (touch is used by the tag list) */
    circlePosition cp, cs;
    hidCircleRead(&cp);
    hidCstickRead(&cs);
    if (abs(cp.dx) > 15) pan_x -= cp.dx / 156.0f * 6.0f;
    if (abs(cp.dy) > 15) pan_y += cp.dy / 156.0f * 6.0f;
    if (abs(cs.dx) > 15) pan_x -= cs.dx / 156.0f * 6.0f;
    if (abs(cs.dy) > 15) pan_y += cs.dy / 156.0f * 6.0f;
    clamp_pan();
}

/* ---------------------------------------------------------------- API key setup
 * Shown at startup when no credentials are configured, or from the grid when an
 * online request fails. The key can be typed (or the whole "&api_key=...&user_id=..."
 * line), or scanned from a QR code with the outer camera. Saved to config.ini. */

#define CAM_W 400
#define CAM_H 240

static bool want_key_kb, want_uid_kb;
static char pend_key[512];                 /* key waiting for a user id */

static volatile bool cam_running;
static Thread    cam_thread;
static u16      *cam_buf, *cam_shared;     /* receive buffer (camera thread) / latest frame */
static LightLock cam_lock;
static volatile u32 cam_frame_no;
static volatile int cam_seen;              /* QR-like codes in the last frame */
static bool      cam_found;                /* payload waiting for the main thread */
static char      cam_payload[512];
static C3D_Tex   cam_tex;
static Tex3DS_SubTexture cam_sub;
static bool      cam_tex_ok;
static u32       cam_tex_frame;
static char      qr_msg[96];

static void apply_creds(const char *key, const char *uid, const char *how)
{
    snprintf(cfg.api_key, sizeof cfg.api_key, "%s", key);
    snprintf(cfg.user_id, sizeof cfg.user_id, "%s", uid);
    pend_key[0] = 0;
    int rc = creds_save(CFG_PATH, cfg.api_key, cfg.user_id);
    log_msg("config: API credentials set via %s - api_key %u chars, user_id %s, %s", how,
            (unsigned)strlen(cfg.api_key), cfg.user_id, rc ? "SAVE FAILED" : "saved to config.ini");
    if (rc) ui_toast_set("Couldn't write config.ini - key used this session only");
    else if (KEY_LEN && strlen(cfg.api_key) != KEY_LEN) ui_toast_set("Saved, but the key length looks wrong - check it");
    else ui_toast_set("API key saved");
    src = SRC_ONLINE;
    mode = MODE_GRID;
    request_list();
}

/* Text from the keyboard or a QR code. Returns true if it produced a key. */
/* e621: "login=NAME" (or username=NAME) in pasted/scanned text */
static bool e6_take_login(const char *text)
{
    const char *names[2] = { "login=", "username=" };
    for (int i = 0; i < 2; i++) {
        const char *p = strstr(text, names[i]);
        if (!p) continue;
        p += strlen(names[i]);
        char u[64];
        size_t n = 0;
        while (*p && n + 1 < sizeof u && ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
               (*p >= 'A' && *p <= 'Z') || *p == '_' || *p == '-' || *p == '.' || *p == '~')) u[n++] = *p++;
        u[n] = 0;
        if (!u[0]) continue;
        snprintf(cfg.username, sizeof cfg.username, "%s", u);
        config_set(CFG_PATH, "username", cfg.username);
        log_msg("config: e621 username set to %s", cfg.username);
        return true;
    }
    return false;
}

/* pixiv: "PHPSESSID=12345_abc..." or just the value */
static bool px_take_cookie(const char *text, const char *how)
{
    const char *p = strstr(text, "PHPSESSID=");
    if (!p) p = strstr(text, "phpsessid=");
    p = p ? p + 10 : text;
    while (*p == ' ' || *p == '"') p++;
    char v[256];
    size_t n = 0;
    while (*p && n + 1 < sizeof v && ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_'))
        v[n++] = *p++;
    v[n] = 0;
    if (n < 16 || !strchr(v, '_')) return false;          /* real ones: <user id>_<32 letters> */
    snprintf(cfg.api_key, sizeof cfg.api_key, "%s", v);
    int rc = config_set(CFG_PATH, "phpsessid", v);
    px_cache_drop = true;
    log_msg("config: pixiv cookie set via %s (%u chars)%s", how, (unsigned)n, rc ? " - SAVE FAILED" : "");
    ui_toast_set(rc ? "Couldn't write config.ini - login used this session only" : "Logged in to pixiv");
    src = SRC_ONLINE;
    mode = MODE_GRID;
    request_list();
    return true;
}

static bool take_cred_text(const char *text, const char *how, bool raw_is_key)
{
    char key[512] = "", uid[64] = "";
    if (IS_PX) return px_take_cookie(text, how);
    if (IS_E6) e6_take_login(text);
    int f = creds_parse(text, key, sizeof key, uid, sizeof uid);
    if (!(f & CREDS_KEY) && raw_is_key) {          /* just the key typed on its own */
        snprintf(key, sizeof key, "%s", text);
        creds_clean(key);
        if (key[0]) f |= CREDS_KEY;
    }
    if (!(f & CREDS_KEY)) return false;
    if (!(f & CREDS_UID) && KEY_NEEDS_UID) {
        if (cfg.user_id[0]) snprintf(uid, sizeof uid, "%s", cfg.user_id);
        else {                                     /* ask for the id next */
            snprintf(pend_key, sizeof pend_key, "%s", key);
            want_uid_kb = true;
            return true;
        }
    }
    apply_creds(key, uid, how);
    if (IS_E6 && !cfg.username[0]) { user_kb_faves = false; want_user_kb = true; }   /* key without a name: ask */
    return true;
}

static void open_key_kb(void)
{
    SwkbdState kb;
    char out[512];
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 300);
    swkbdSetHintText(&kb, IS_DA ? "client_secret, or client_id=...&client_secret=..."
                        : IS_PX ? "PHPSESSID cookie value (12345_abc...)"
                        : IS_E6 ? "API key, or login=NAME&api_key=KEY"
                                : "&api_key=...&user_id=... or just the key");
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (swkbdInputText(&kb, out, sizeof out) != SWKBD_BUTTON_CONFIRM) return;
    if (!take_cred_text(out, "keyboard", true)) ui_toast_set("No key found in that text");
}

static void open_uid_kb(void)
{
    SwkbdState kb;
    char out[64];
    swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, 16);
    swkbdSetHintText(&kb, KEY_LEN == 32 ? "client_id (numbers)" : "user ID (numbers)");
    swkbdSetInitialText(&kb, cfg.user_id);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (swkbdInputText(&kb, out, sizeof out) != SWKBD_BUTTON_CONFIRM) { pend_key[0] = 0; return; }
    creds_clean(out);
    if (!out[0]) { ui_toast_set("No user ID entered"); return; }
    if (pend_key[0])          apply_creds(pend_key, out, "keyboard");
    else if (cfg.api_key[0])  apply_creds(cfg.api_key, out, "keyboard");
    else { snprintf(cfg.user_id, sizeof cfg.user_id, "%s", out); ui_toast_set("User ID set - now add the key"); }
}

/* --- camera (outer camera 1, 400x240 RGB565) */

static void cam_main(void *arg)
{
    (void)arg;
    struct quirc *q = qrscan_new(CAM_W, CAM_H);
    if (!q) log_msg("qr: out of memory for decoder");
    Handle ev[2] = { 0, 0 };                       /* [0] frame received, [1] buffer error */
    u32 bufsize = 0;
    CAMU_GetMaxBytes(&bufsize, CAM_W, CAM_H);
    CAMU_SetTransferBytes(PORT_CAM1, bufsize, CAM_W, CAM_H);
    CAMU_GetBufferErrorInterruptEvent(&ev[1], PORT_CAM1);
    CAMU_ClearBuffer(PORT_CAM1);
    CAMU_StartCapture(PORT_CAM1);
    char payload[512];
    u32 frames = 0;
    while (cam_running) {
        if (!ev[0]) CAMU_SetReceiving(&ev[0], cam_buf, PORT_CAM1, CAM_W * CAM_H * 2, (s16)bufsize);
        s32 idx = -1;
        Result r = svcWaitSynchronizationN(&idx, ev, 2, false, 200 * 1000000LL);
        if (R_FAILED(r) || r == (Result)0x09401BFE) continue;   /* timeout */
        if (idx == 1) {                            /* buffer error (e.g. after HOME): restart */
            if (ev[0]) { svcCloseHandle(ev[0]); ev[0] = 0; }
            CAMU_ClearBuffer(PORT_CAM1);
            CAMU_StartCapture(PORT_CAM1);
            continue;
        }
        svcCloseHandle(ev[0]);
        ev[0] = 0;
        frames++;
        LightLock_Lock(&cam_lock);
        memcpy(cam_shared, cam_buf, CAM_W * CAM_H * 2);
        cam_frame_no++;
        bool busy = cam_found;
        LightLock_Unlock(&cam_lock);

        int seen = 0;
        if (q && !busy && qrscan_rgb565(q, cam_buf, CAM_W, CAM_H, payload, sizeof payload, &seen)) {
            LightLock_Lock(&cam_lock);
            snprintf(cam_payload, sizeof cam_payload, "%s", payload);
            cam_found = true;
            LightLock_Unlock(&cam_lock);
        }
        cam_seen = seen;
    }
    CAMU_StopCapture(PORT_CAM1);
    if (ev[0]) svcCloseHandle(ev[0]);
    if (ev[1]) svcCloseHandle(ev[1]);
    qrscan_free(q);
    log_msg("qr: camera stopped after %lu frames", (unsigned long)frames);
}

static void cam_stop(void)
{
    if (!cam_running && !cam_thread) return;
    cam_running = false;
    if (cam_thread) { threadJoin(cam_thread, U64_MAX); threadFree(cam_thread); cam_thread = NULL; }
    CAMU_Activate(SELECT_NONE);
    camExit();
    free(cam_buf); cam_buf = NULL;
    free(cam_shared); cam_shared = NULL;
    if (cam_tex_ok) { C3D_TexDelete(&cam_tex); cam_tex_ok = false; }
}

static bool cam_start(void)
{
    Result r = camInit();
    if (R_FAILED(r)) {
        log_msg("qr: camInit failed 0x%08lX", (unsigned long)r);
        ui_toast_set("Camera unavailable");
        return false;
    }
    CAMU_SetSize(SELECT_OUT1, SIZE_CTR_TOP_LCD, CONTEXT_A);
    CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A);
    CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_15);
    CAMU_SetNoiseFilter(SELECT_OUT1, true);
    CAMU_SetAutoExposure(SELECT_OUT1, true);
    CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);
    CAMU_SetTrimming(PORT_CAM1, false);
    CAMU_Activate(SELECT_OUT1);

    cam_buf = malloc(CAM_W * CAM_H * 2);
    cam_shared = calloc(1, CAM_W * CAM_H * 2);
    cam_tex_ok = C3D_TexInit(&cam_tex, 512, 256, GPU_RGB565);
    if (!cam_buf || !cam_shared || !cam_tex_ok) {
        log_msg("qr: out of memory");
        ui_toast_set("Out of memory for the camera");
        cam_stop();
        return false;
    }
    C3D_TexSetFilter(&cam_tex, GPU_LINEAR, GPU_LINEAR);
    memset(cam_tex.data, 0, cam_tex.size);
    cam_sub = (Tex3DS_SubTexture){ CAM_W, CAM_H, 0.0f, 1.0f, CAM_W / 512.0f, 1.0f - CAM_H / 256.0f };
    cam_found = false;
    cam_seen = 0;
    cam_frame_no = cam_tex_frame = 0;
    qr_msg[0] = 0;
    LightLock_Init(&cam_lock);

    cam_running = true;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    cam_thread = threadCreate(cam_main, NULL, 64 * 1024, prio + 1, 2, false);
    if (!cam_thread) cam_thread = threadCreate(cam_main, NULL, 64 * 1024, prio + 1, -2, false);
    if (!cam_thread) { log_msg("qr: thread failed"); ui_toast_set("Couldn't start the camera"); cam_stop(); return false; }
    log_msg("qr: camera started");
    return true;
}

static bool qr_for_da_code;      /* the camera is reading a DeviantArt sign-in address */
static bool want_da_code_kb;

static void open_qr(void)
{
    qr_for_da_code = false;
    if (cam_start()) mode = MODE_QR;
}

static bool da_take_code(const char *text);

static void close_qr(Mode to)
{
    cam_stop();
    mode = to;
}

/* Main thread, after C3D_FrameBegin: upload the newest frame, handle a decoded code. */
static void qr_sync(void)
{
    if (mode != MODE_QR || !cam_tex_ok) return;
    LightLock_Lock(&cam_lock);
    if (cam_frame_no != cam_tex_frame) {
        cam_tex_frame = cam_frame_no;
        u16 *dst = (u16 *)cam_tex.data;
        const u16 *srcp = cam_shared;
        for (int y = 0; y < CAM_H; y++) {
            u32 rowtile = (u32)(y >> 3) * (512 >> 3);
            for (int x = 0; x < CAM_W; x++)
                dst[((rowtile + (x >> 3)) << 6) + morton8(x & 7, y & 7)] = *srcp++;
        }
        C3D_TexFlush(&cam_tex);
    }
    char payload[512] = "";
    bool got = cam_found;
    if (got) snprintf(payload, sizeof payload, "%s", cam_payload);
    LightLock_Unlock(&cam_lock);
    if (!got) return;

    log_msg("qr: decoded %u chars", (unsigned)strlen(payload));
    if (qr_for_da_code) {
        if (da_take_code(payload)) { close_qr(MODE_DALOGIN); return; }
        snprintf(qr_msg, sizeof qr_msg, "That QR code has no code= in it");
        LightLock_Lock(&cam_lock);
        cam_found = false;
        LightLock_Unlock(&cam_lock);
        return;
    }
    if (IS_PX) {
        if (px_take_cookie(payload, "QR code")) { close_qr(MODE_GRID); return; }
        snprintf(qr_msg, sizeof qr_msg, "That QR code isn't a PHPSESSID cookie");
        LightLock_Lock(&cam_lock);
        cam_found = false;
        LightLock_Unlock(&cam_lock);
        return;
    }
    char key[512] = "", uid[64] = "";
    bool raw_key = !KEY_NEEDS_UID;               /* Derpibooru: the QR can be just the key */
    if (raw_key) { snprintf(key, sizeof key, "%s", payload); creds_clean(key); }
    if ((creds_parse(payload, key, sizeof key, uid, sizeof uid) & CREDS_KEY) ||
        (raw_key && strlen(key) >= 12 && strlen(key) <= 64)) {
        close_qr(MODE_SETUP);
        take_cred_text(payload, "QR code", raw_key);
        return;
    }
    snprintf(qr_msg, sizeof qr_msg, !KEY_NEEDS_UID ? "That QR code doesn't look like an API key" :
             KEY_LEN == 32 ? "That QR code has no client_secret= in it" : "That QR code has no api_key= in it");
    LightLock_Lock(&cam_lock);
    cam_found = false;                             /* keep scanning */
    LightLock_Unlock(&cam_lock);
}

/* --- setup screens */

#define SBTN_X 20
#define SBTN_W 280
#define SBTN_H 36

static void draw_top_setup(void)
{
    char saved_to[96], buf2[96];
    snprintf(saved_to, sizeof saved_to, "Saved to %s", cfg_path);
    if (IS_DA) {
    text(16, 12, 0.7f, C_ACCENT, "DeviantArt app keys");
    text_wrap(16, 40, 0.42f, C_TEXT, 368,
              "DeviantArt's API needs your own free app. On a phone or PC, log in at "
              "deviantart.com/developers, choose Register Application (any name; a redirect "
              "URI like http://localhost is fine) and copy its client_id and client_secret.");
    text_wrap(16, 112, 0.42f, C_TEXT, 368,
              "Easiest: make a QR code of  client_id=...&client_secret=...  and scan it. "
              "Or type the secret (or that whole line), then the client ID.");
    char buf[128];
    snprintf(buf, sizeof buf, "Now: client_secret %u chars%s   client_id %s",
             (unsigned)strlen(cfg.api_key), cfg.api_key[0] && strlen(cfg.api_key) != KEY_LEN ? " (expected 32)" : "",
             cfg.user_id[0] ? cfg.user_id : "(none)");
    text(16, 196, 0.42f, cfg.api_key[0] && cfg.user_id[0] ? C_ACCENT : C_WARN, buf);
    text(16, 216, 0.4f, C_DIM, saved_to);
    return;
    }
    if (IS_PX) {
    text(16, 12, 0.7f, C_ACCENT, "pixiv login (optional)");
    text_wrap(16, 40, 0.42f, C_TEXT, 368,
              "Logged out you can search and see rankings; R-18 works and your bookmarks need "
              "your login cookie. In a PC browser logged in to pixiv.net, open developer tools > "
              "Application (or Storage) > Cookies > www.pixiv.net, and copy PHPSESSID.");
    text_wrap(16, 112, 0.42f, C_WARN, 368,
              "The cookie works like your password: keep it (and its QR code) private. "
              "Logging out of pixiv in the browser cancels it.");
    text_wrap(16, 150, 0.42f, C_TEXT, 368, "Scan a QR code of the value (or PHPSESSID=value), or type it.");
    snprintf(buf2, sizeof buf2, "Now: %s", cfg.api_key[0] ? "login cookie set" : "logged out");
    text(16, 196, 0.45f, cfg.api_key[0] ? C_ACCENT : C_DIM, buf2);
    text(16, 216, 0.4f, C_DIM, saved_to);
    return;
    }
    if (IS_E6) {
    text(16, 12, 0.7f, C_ACCENT, "e621 login (optional)");
    text_wrap(16, 44, 0.45f, C_TEXT, 368,
              "Without a login you browse as a guest, and e621 hides some posts. To log in, "
              "on a phone or PC open e621.net > Account > Manage API Access, create a key, "
              "and enter it here with your username.");
    text_wrap(16, 112, 0.45f, C_TEXT, 368,
              "Easiest: make a QR code of  login=YOURNAME&api_key=YOURKEY  and scan it. "
              "Or type the key, then your username.");
    char buf[128];
    snprintf(buf, sizeof buf, "Now: username %s   API key %s", cfg.username[0] ? cfg.username : "(none)",
             cfg.api_key[0] ? "set" : "not set");
    text(16, 196, 0.45f, cfg.api_key[0] && cfg.username[0] ? C_ACCENT : C_DIM, buf);
    text(16, 216, 0.4f, C_DIM, saved_to);
    return;
    }
    if (IS_DERPI) {
    text(16, 12, 0.7f, C_ACCENT, "Derpibooru API key (optional)");
    text_wrap(16, 44, 0.45f, C_TEXT, 368,
              "Without a key, searches use Derpibooru's default filter. With your key, they "
              "use your account's filters. On a phone or PC, open derpibooru.org > Account "
              "and copy the API Key.");
    text_wrap(16, 112, 0.45f, C_TEXT, 368,
              "Easiest: make a QR code of the key with any QR code generator and scan it. "
              "Or type it in.");
    char buf[128];
    snprintf(buf, sizeof buf, "Now: %s   filter %s", cfg.api_key[0] ? "API key set" : "no API key (default filter)",
             cfg.filter_id[0] ? cfg.filter_id : "default");
    text(16, 196, 0.45f, cfg.api_key[0] ? C_ACCENT : C_DIM, buf);
    text(16, 216, 0.4f, C_DIM, saved_to);
    return;
    }
    text(16, 12, 0.7f, C_ACCENT, "rule34.xxx API key");
    text_wrap(16, 44, 0.45f, C_TEXT, 368,
              "Searching needs your account's API key. On a phone or PC, open rule34.xxx "
              "> My Account > Options, and copy the line under API Access Credentials "
              "(&api_key=...&user_id=...).");
    text_wrap(16, 112, 0.45f, C_TEXT, 368,
              "Easiest: turn that line into a QR code with any QR code generator and scan it. "
              "Or type it in (the whole line, or just the key and then your user ID).");
    char buf[128];
    snprintf(buf, sizeof buf, "Now: api_key %u chars%s   user_id %s",
             (unsigned)strlen(cfg.api_key), cfg.api_key[0] && strlen(cfg.api_key) != 128 ? " (expected 128)" : "",
             cfg.user_id[0] ? cfg.user_id : "(none)");
    text(16, 196, 0.45f, cfg.api_key[0] && cfg.user_id[0] ? C_ACCENT : C_WARN, buf);
    text(16, 216, 0.4f, C_DIM, saved_to);
}

static void remove_key(void)
{
    cfg.api_key[0] = 0;
    int rc = IS_PX ? config_set(CFG_PATH, "phpsessid", "") : creds_save(CFG_PATH, "", cfg.user_id);
    px_cache_drop = true;
    log_msg("config: API key removed%s", rc ? " (SAVE FAILED)" : "");
    ui_toast_set(rc ? "Couldn't write config.ini" : IS_PX ? "Logged out of pixiv" : "API key removed - using the default filter");
    src = SRC_ONLINE;
    mode = MODE_GRID;
    request_list();
}

static int setup_buttons(void) { return IS_DA ? 5 : 4; }
static int setup_btn_h(void)   { return IS_DA ? 32 : SBTN_H; }
static int setup_btn_y(int i)  { return IS_DA ? 10 + i * 44 : 20 + i * (SBTN_H + 12); }
static void open_dalogin(void);

static void draw_bottom_setup(void)
{
    static const char *da_labels[4] = { "Scan QR code   (A)", "Type secret / whole line   (Y)",
                                        "Type client ID   (X)", "Back   (B)" };
    static const char *derpi_labels[4] = { "Scan QR code   (A)", "Type key   (Y)",
                                           "Remove key   (X)", "Back   (B)" };
    static const char *r34_labels[4] = { "Scan QR code   (A)", "Type key / whole line   (Y)",
                                         "Type user ID   (X)", "Back   (B)" };
    static const char *e6_labels[4] = { "Scan QR code   (A)", "Type API key / whole line   (Y)",
                                        "Type username   (X)", "Back   (B)" };
    static const char *px_labels[4] = { "Scan QR code   (A)", "Type PHPSESSID cookie   (Y)",
                                        "Log out (remove cookie)   (X)", "Back   (B)" };
    const char **labels = IS_DA ? da_labels : IS_DERPI ? derpi_labels : IS_E6 ? e6_labels : IS_PX ? px_labels : r34_labels;
    int nb = setup_buttons();
    for (int i = 0; i < nb; i++) {
        const char *l = labels[i];
        if (IS_DA && i == 3) l = cfg.da_refresh[0] ? "Your account: signed in   (R)" : "Sign in to your account   (R)";
        if (IS_DA && i == 4) l = labels[3];
        bool back = i == nb - 1;
        draw_button(SBTN_X, setup_btn_y(i), SBTN_W, setup_btn_h(), i == 0 ? C_ACCENT : back ? C_PANEL : C_CELL,
                    i == 0 ? C_BG : C_TEXT, l, 0.55f);
    }
}

static void input_setup(u32 down)
{
    int b = -1;
    if (down & KEY_A) b = 0;
    if (down & KEY_Y) b = 1;
    if (down & KEY_X) b = 2;
    if (down & KEY_B) b = setup_buttons() - 1;
    if ((down & KEY_R) && IS_DA) b = 3;
    if (down & KEY_START) want_quit = true;
    if (down & KEY_TOUCH) {
        touchPosition t; hidTouchRead(&t);
        for (int i = 0; i < setup_buttons(); i++) {
            int y = setup_btn_y(i);
            if (t.px >= SBTN_X && t.px < SBTN_X + SBTN_W && t.py >= y && t.py < y + setup_btn_h()) b = i;
        }
    }
    if (IS_DA && b == 3) { open_dalogin(); return; }
    if (IS_DA && b == 4) b = 3;
    switch (b) {
    case 0: open_qr(); break;
    case 1: want_key_kb = true; break;
    case 2: if (IS_DERPI || IS_PX) remove_key(); else if (IS_E6) { user_kb_faves = false; want_user_kb = true; } else want_uid_kb = true; break;
    case 3: mode = MODE_GRID; break;
    }
}

static void draw_top_qr(void)
{
    if (cam_tex_ok && cam_frame_no) {
        C2D_Image img = { &cam_tex, &cam_sub };
        C2D_DrawImageAt(img, 0, 0, 0.5f, NULL, 1.0f, 1.0f);
    } else {
        text_center(200, 110, 0.6f, C_DIM, "Starting camera...");
    }
    /* target box corners */
    u32 c = cam_seen ? C_ACCENT : C_TEXT;
    float x0 = 100, y0 = 20, s = 200, l = 28, t = 3;
    C2D_DrawRectSolid(x0, y0, 0.6f, l, t, c);         C2D_DrawRectSolid(x0, y0, 0.6f, t, l, c);
    C2D_DrawRectSolid(x0 + s - l, y0, 0.6f, l, t, c); C2D_DrawRectSolid(x0 + s - t, y0, 0.6f, t, l, c);
    C2D_DrawRectSolid(x0, y0 + s - t, 0.6f, l, t, c); C2D_DrawRectSolid(x0, y0 + s - l, 0.6f, t, l, c);
    C2D_DrawRectSolid(x0 + s - l, y0 + s - t, 0.6f, l, t, c); C2D_DrawRectSolid(x0 + s - t, y0 + s - l, 0.6f, t, l, c);
}

static void draw_bottom_qr(void)
{
    text(16, 14, 0.6f, C_ACCENT, "Scan QR code");
    text_wrap(16, 44, 0.45f, C_TEXT, 288,
              "Point the outer camera at the QR code so it fills the box on the top screen. "
              "Hold steady with good light; avoid glare on phone screens.");
    const char *st = qr_msg[0] ? qr_msg : cam_seen ? "QR code found - hold steady..." : "Looking for a QR code...";
    text(16, 120, 0.5f, qr_msg[0] ? C_WARN : cam_seen ? C_ACCENT : C_DIM, st);
    draw_button(20, 160, 130, 34, C_CELL, C_TEXT, "Type instead (Y)", 0.5f);
    draw_button(170, 160, 130, 34, C_PANEL, C_TEXT, "Cancel (B)", 0.5f);
}

static void input_qr(u32 down)
{
    int b = -1;
    if (down & KEY_Y) b = 0;
    if (down & KEY_B) b = 1;
    if (down & KEY_TOUCH) {
        touchPosition t; hidTouchRead(&t);
        if (t.py >= 160 && t.py < 194) b = t.px < 160 ? 0 : 1;
    }
    if (b == 0 && qr_for_da_code) { close_qr(MODE_DALOGIN); want_da_code_kb = true; return; }
    if (b == 0) { close_qr(MODE_SETUP); want_key_kb = true; }
    if (b == 1) close_qr(qr_for_da_code ? MODE_DALOGIN : MODE_SETUP);
}

/* ---------------------------------------------------------------- site tab (combined app)
 * Tapping cycles rule34 -> Derpibooru -> DeviantArt. Each site has its own folder
 * (config, gallery, folders), search and page, so switching back resumes where you were. */

static int  pending_site = -1;
static bool pending_announced;
static u64  pending_since;

static void site_tab_tap(void)
{
    int next = (pending_site >= 0 ? pending_site : CUR_SITE) + 1;
    pending_site = next % SITE_COUNT;
    pending_announced = false;
    pending_since = osGetTime();
}

static void draw_site_tab(void)
{
    int show = pending_site >= 0 ? pending_site : CUR_SITE;
    C2D_DrawRectSolid(SITE_TX, HELP_GY, 0.5f, SITE_TW, HELP_GH, C_CELL);
    C2D_DrawRectSolid(SITE_TX, HELP_GY, 0.5f, 3, HELP_GH, SITE_COLOR(show));
    char buf[48];
    snprintf(buf, sizeof buf, pending_site >= 0 ? "%s ..." : "%s  >", site_info[show].tab);
    text_center(SITE_TX + SITE_TW / 2 + 1, HELP_GY + 2, 0.42f, SITE_COLOR(show), buf);
}

static void load_site(void);
static void write_last_site(void);

/* Called every frame; switches once the worker is between items. */
static void site_switch_poll(void)
{
#ifdef SITE_MULTI
    if (pending_site < 0) return;
    if (mode != MODE_GRID) { pending_site = -1; return; }
    LightLock_Lock(&lock);
    list_gen++;                     /* cancel searches/thumbnails/images in flight */
    full_gen++;
    bool queued = req_save || req_delete;   /* let those finish in the current site's gallery */
    LightLock_Unlock(&lock);
    LightEvent_Signal(&wake);
    if (queued || LightLock_TryLock(&site_lock)) {
        if (!pending_announced && osGetTime() - pending_since > 400) {   /* a save still running */
            ui_toast_set("Finishing the current download...");
            pending_announced = true;
        }
        return;
    }
    int to = pending_site;
    pending_site = -1;
    log_msg("site: switching %s -> %s", site_info[g_site].name, site_info[to].name);
    g_site = to;
    load_site();
    open_after_load = -1;
    qb_n = 0;
    tl_id = 0xFFFFFFFFu;
    request_list();                 /* queued before the worker can run again */
    LightLock_Unlock(&site_lock);
    write_last_site();
    ui_toast_set(site_info[g_site].name);
    if (KEY_REQUIRED && net_ok && src == SRC_ONLINE && (!cfg.api_key[0] || (KEY_NEEDS_UID && !cfg.user_id[0])))
        mode = MODE_SETUP;
#endif
}

/* Faves button (DeviantArt): your favourites (asks for your username the first time). */
static void open_faves(void)
{
    char q[96], id[40];
    if (IS_PX) {
        if (!px_user_id(id, sizeof id)) { user_kb_faves = true; want_user_kb = true; return; }
        snprintf(q, sizeof q, "fav:%s", id);
        run_search(q);
        return;
    }
    if (!cfg.username[0]) { user_kb_faves = true; want_user_kb = true; return; }
    snprintf(q, sizeof q, "fav:%s", cfg.username);
    run_search(q);
}

static void open_user_kb(void)
{
    SwkbdState kb;
    char out[64];
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 40);
    swkbdSetHintText(&kb, IS_PX ? "your pixiv user ID (the number in your profile link)"
                        : IS_E6 ? "your e621 username" : "your DeviantArt username");
    swkbdSetInitialText(&kb, cfg.username);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (swkbdInputText(&kb, out, sizeof out) != SWKBD_BUTTON_CONFIRM) return;
    char *o = out;                                   /* usernames: letters, digits, - and _ */
    for (char *c = out; *c; c++)
        if ((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '-' || *c == '_' ||
            (IS_E6 && (*c == '.' || *c == '~'))) *o++ = *c;
    *o = 0;
    if (!out[0]) { ui_toast_set("That username can't be used"); return; }
    if (IS_PX && strspn(out, "0123456789") != strlen(out)) { ui_toast_set("pixiv needs the user ID number, e.g. 12345"); return; }
    snprintf(cfg.username, sizeof cfg.username, "%s", out);
    if (config_set(CFG_PATH, "username", cfg.username)) ui_toast_set("Couldn't save the username to config.ini");
    log_msg("config: username set to %s", cfg.username);
    if (user_kb_faves) open_faves();
    else { ui_toast_set("Username saved"); if (IS_E6 && cfg.api_key[0]) { mode = MODE_GRID; request_list(); } }
}

/* ---------------------------------------------------------------- DeviantArt sign-in
 * OAuth "authorization code": the app shows DeviantArt's sign-in page as a QR
 * code; you approve on a phone/PC, which then lands on <redirect_uri>?code=...
 * (a page that won't load - that's fine). Scan or type that address (or just
 * the code) and the worker swaps it for a refresh token, saved in config.ini.
 * Signed in, DeviantArt treats requests as yours: mature images unblurred
 * (if your account is age-verified and shows mature content). */

static Slot dalogin_qr;
static char dalogin_url[600];

static void url_encode(const char *in, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < n; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p)) out[o++] = *p;
        else { out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15]; }
    }
    out[o] = 0;
}

static void dalogin_build_qr(void)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(15)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(15)];
    char ru[300];
    url_encode(da_redirect(), ru, sizeof ru);
    snprintf(dalogin_url, sizeof dalogin_url,
             "https://www.deviantart.com/oauth2/authorize?response_type=code&client_id=%s&redirect_uri=%s&scope=browse%%20user&state=3ds",
             cfg.user_id, ru);
    log_msg("deviantart: sign-in link (open it in a browser if the QR code misbehaves): %s", dalogin_url);
    LightLock_Lock(&lock);
    slot_reset(&dalogin_qr);
    LightLock_Unlock(&lock);
    if (!qrcodegen_encodeText(dalogin_url, tmp, qr, qrcodegen_Ecc_LOW, 1, 15, qrcodegen_Mask_AUTO, true)) {
        log_msg("deviantart: sign-in link too long for a QR code");
        return;
    }
    int n = qrcodegen_getSize(qr), b = 3, w = n + 2 * b;          /* 1 pixel per module + quiet zone */
    uint8_t *px = malloc((size_t)w * w * 4);
    if (!px) return;
    for (int y = 0; y < w; y++)
        for (int x = 0; x < w; x++) {
            bool dark = x >= b && y >= b && x < b + n && y < b + n && qrcodegen_getModule(qr, x - b, y - b);
            uint8_t v = dark ? 0 : 255, *d = px + ((size_t)y * w + x) * 4;
            d[0] = d[1] = d[2] = v; d[3] = 255;
        }
    dalogin_qr.rgba = px; dalogin_qr.w = w; dalogin_qr.h = w;
    bool ok = slot_upload(&dalogin_qr);
    free(px);
    dalogin_qr.rgba = NULL;
    dalogin_qr.st = ok ? S_GPU : S_FAIL;
    if (ok) C3D_TexSetFilter(&dalogin_qr.tex[0], GPU_NEAREST, GPU_NEAREST);   /* crisp modules when scaled */
}

static void open_dalogin(void)
{
    if (!cfg.user_id[0] || !cfg.api_key[0]) { ui_toast_set("Enter your app's client_id and client_secret first"); return; }
    dalogin_build_qr();
    mode = MODE_DALOGIN;
}

/* "http://localhost/?code=abc123&state=3ds", or just "abc123" */
static bool da_take_code(const char *text)
{
    if (strstr(text, "error=access_denied")) { ui_toast_set("Access was denied on the sign-in page"); return true; }
    const char *p = strstr(text, "code=");
    p = p ? p + 5 : text;
    while (*p == ' ') p++;
    char code[200];
    size_t n = 0;
    while (*p && n + 1 < sizeof code && ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
        code[n++] = *p++;
    code[n] = 0;
    if (n < 8) return false;
    LightLock_Lock(&lock);
    snprintf(req_da_code, sizeof req_da_code, "%s", code);
    LightLock_Unlock(&lock);
    LightEvent_Signal(&wake);
    ui_toast_set("Signing in...");
    log_msg("deviantart: got a sign-in code (%u chars)", (unsigned)n);
    return true;
}

static void open_da_code_kb(void)
{
    SwkbdState kb;
    char out[512];
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 500);
    swkbdSetHintText(&kb, "the address with code=..., or just the code");
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (swkbdInputText(&kb, out, sizeof out) != SWKBD_BUTTON_CONFIRM) return;
    if (!da_take_code(out)) ui_toast_set("No code found in that text");
}

static void da_sign_out(void)
{
    cfg.da_refresh[0] = 0;
    da_user[0] = 0;
    da_reauth = true;
    int rc = config_set(CFG_PATH, "refresh_token", "");
    log_msg("deviantart: signed out%s", rc ? " (SAVE FAILED)" : "");
    ui_toast_set("Signed out - back to app-only access (mature images blurred)");
}

#define DL_N   6
#define DL_Y0  6
#define DL_ST  38
#define DL_H   30
static bool want_redirect_kb;

static const char *dalogin_label(int i, char *buf, size_t n)
{
    switch (i) {
    case 0: return "Scan the address QR   (A)";
    case 1: return "Type the code / address   (Y)";
    case 2: snprintf(buf, n, "Mature content: %s   (X)", cfg.mature ? "ON" : "OFF"); return buf;
    case 3: snprintf(buf, n, "Redirect: %.30s   (L)", da_redirect()); return buf;
    case 4: return cfg.da_refresh[0] ? "Sign out   (R)" : "Sign out (not signed in)";
    default: return "Back   (B)";
    }
}

static void open_redirect_kb(void)
{
    SwkbdState kb;
    char out[128];
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, sizeof out - 1);
    swkbdSetHintText(&kb, "redirect address, exactly as whitelisted in your app");
    swkbdSetInitialText(&kb, da_redirect());
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (swkbdInputText(&kb, out, sizeof out) != SWKBD_BUTTON_CONFIRM) return;
    trim(out);
    if (strncmp(out, "https://", 8) && strncmp(out, "http://", 7)) { ui_toast_set("It must start with https://"); return; }
    snprintf(cfg.redirect_uri, sizeof cfg.redirect_uri, "%s", out);
    config_set(CFG_PATH, "redirect_uri", cfg.redirect_uri);
    dalogin_build_qr();
    ui_toast_set("Redirect address saved - QR code updated");
}

static void draw_top_dalogin(void)
{
    char buf[200];
    text(12, 8, 0.65f, C_ACCENT, "Sign in to DeviantArt");
    if (cfg.da_refresh[0]) snprintf(buf, sizeof buf, "Signed in%s%s", da_user[0] ? " as " : "", da_user);
    else snprintf(buf, sizeof buf, "Not signed in (app-only: mature images blurred)");
    text(12, 30, 0.42f, cfg.da_refresh[0] ? C_ACCENT : C_WARN, buf);
    float tw = 220;
    text_wrap(12, 52, 0.4f, C_TEXT, tw, "1. Once: at deviantart.com/developers, edit your app and add this to its OAuth2 Redirect URI Whitelist:");
    snprintf(buf, sizeof buf, "%.60s", da_redirect());
    text(20, 92, 0.42f, C_ACCENT, buf);
    text_wrap(12, 108, 0.38f, C_TEXT, tw, "2. Log in to deviantart.com in your phone's browser, then scan the QR code and tap Allow. If it opens the DeviantArt app or the home page, open the link in the browser instead (the link is also in log.txt).");
    text_wrap(12, 170, 0.38f, C_TEXT, tw, "3. You land on an address that won't load, with code= in it. Scan it as a QR code or type it, within a few minutes.");
    text_wrap(12, 216, 0.34f, C_DIM, 376, "Mature images are only unblurred if your account is age-verified and set to show mature content.");
    if (dalogin_qr.st == S_GPU) {
        int isc = 156 / dalogin_qr.w;                      /* whole pixels per module: crisp */
        float sc = isc >= 2 ? isc : 156.0f / dalogin_qr.w, sz = dalogin_qr.w * sc;
        float qx = 236 + (158 - sz) / 2, qy = 40 + (158 - sz) / 2;
        C2D_DrawRectSolid(236, 40, 0.5f, 158, 158, C2D_Color32(255, 255, 255, 255));
        C2D_DrawImageAt(dalogin_qr.img[0], qx, qy, 0.5f, NULL, sc, sc);
    } else {
        text_wrap(240, 100, 0.4f, C_WARN, 150, "Couldn't make the QR code - see the log");
    }
}

static void draw_bottom_dalogin(void)
{
    char buf[64];
    for (int i = 0; i < DL_N; i++) {
        bool off = i == 4 && !cfg.da_refresh[0];
        draw_button(SBTN_X, DL_Y0 + i * DL_ST, SBTN_W, DL_H, i == 0 ? C_ACCENT : i == DL_N - 1 ? C_PANEL : C_CELL,
                    i == 0 ? C_BG : off ? C_DIM : C_TEXT, dalogin_label(i, buf, sizeof buf), 0.46f);
    }
}

static void input_dalogin(u32 down)
{
    int b = -1;
    if (down & KEY_A) b = 0;
    if (down & KEY_Y) b = 1;
    if (down & KEY_X) b = 2;
    if (down & KEY_L) b = 3;
    if (down & KEY_R) b = 4;
    if (down & KEY_B) b = 5;
    if (down & KEY_START) want_quit = true;
    if (down & KEY_TOUCH) {
        touchPosition t; hidTouchRead(&t);
        for (int i = 0; i < DL_N; i++) {
            int y = DL_Y0 + i * DL_ST;
            if (t.px >= SBTN_X && t.px < SBTN_X + SBTN_W && t.py >= y && t.py < y + DL_H) b = i;
        }
    }
    switch (b) {
    case 0: if (cam_start()) { qr_for_da_code = true; mode = MODE_QR; } break;
    case 1: want_da_code_kb = true; break;
    case 2:
        cfg.mature = !cfg.mature;
        config_set(CFG_PATH, "mature", cfg.mature ? "1" : "0");
        ui_toast_set(cfg.mature ? "Mature content on" : "Mature content off");
        break;
    case 3: want_redirect_kb = true; break;
    case 4: if (cfg.da_refresh[0]) da_sign_out(); break;
    case 5:
        mode = MODE_GRID;
        src = SRC_ONLINE;
        request_list();
        break;
    }
}

/* ---------------------------------------------------------------- options
 * Grid menu button. Theme (app-wide, settings.ini in the app's folder), the
 * current site's sort and favourite tags (in its folder), and resetting every
 * site's API keys. */

#define SETTINGS_PATH "sdmc:/3ds/" BUILD_NAME "/settings.ini"
#define OPT_ROW   26
#define OPT_GAP   7
#define OPT_Y     6
enum { OPT_THEME, OPT_CUSTOM, OPT_FAVS, OPT_KEYS, OPT_RESET, OPT_BACK, OPT_COUNT };

static int  opt_sel;
static u64  reset_armed_until;          /* "Reset all API keys" needs a second tap */
static bool want_custom_colors;

static void settings_save(void)
{
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) { log_msg("settings: can't write %s", SETTINGS_PATH); return; }
    fprintf(f, "# app-wide settings (written by the app)\ntheme=%s\ncustom_bg=%06X\ncustom_text=%06X\n",
            themes[th.idx].name, th.custom_bg, th.custom_text);
    if (th.custom_accent == 0xFFFFFFFFu) fprintf(f, "custom_accent=site\n");
    else fprintf(f, "custom_accent=%06X\n", th.custom_accent);
    fclose(f);
}

static bool parse_hex(const char *v, unsigned *out)
{
    while (*v == ' ' || *v == '#') v++;
    if (!strncasecmp(v, "0x", 2)) v += 2;
    char *e;
    unsigned long x = strtoul(v, &e, 16);
    if (e - v != 6) return false;
    *out = (unsigned)x;
    return true;
}

static void settings_load(void)
{
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            if (!eq || line[0] == '#') continue;
            *eq = 0;
            char *k = line, *v = eq + 1;
            trim(k); trim(v);
            if (!strcmp(k, "theme")) {
                for (int i = 0; i < THEME_COUNT; i++) if (!strcasecmp(v, themes[i].name)) th.idx = i;
            } else if (!strcmp(k, "custom_bg"))   parse_hex(v, &th.custom_bg);
            else if (!strcmp(k, "custom_text"))   parse_hex(v, &th.custom_text);
            else if (!strcmp(k, "custom_accent")) { if (!parse_hex(v, &th.custom_accent)) th.custom_accent = 0xFFFFFFFFu; }
        }
        fclose(f);
    }
    theme_apply();
    log_msg("settings: theme %s", themes[th.idx].name);
}

/* --- favourite tags: one search per line in the site's favtags.txt */
#define FAV_MAX 32
static char fav_tags[FAV_MAX][128];
static int  fav_n;
static float fav_scroll;
static bool want_fav_add;

static void fav_path(char *out, size_t n) { snprintf(out, n, "%s/favtags.txt", app_dir); }

static void fav_load(void)
{
    char path[96], line[160];
    fav_path(path, sizeof path);
    fav_n = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fav_n < FAV_MAX && fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        snprintf(fav_tags[fav_n++], sizeof fav_tags[0], "%s", line);
    }
    fclose(f);
}

static void fav_save(void)
{
    char path[96];
    fav_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) { ui_toast_set("Couldn't save favtags.txt"); return; }
    for (int i = 0; i < fav_n; i++) fprintf(f, "%s\n", fav_tags[i]);
    fclose(f);
}

static void open_fav_add_kb(void)
{
    SwkbdState kb;
    char out[128];
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, sizeof out - 1);
    swkbdSetHintText(&kb, "tags to save as a favourite");
    swkbdSetInitialText(&kb, nav[SRC_ONLINE].tags);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (swkbdInputText(&kb, out, sizeof out) != SWKBD_BUTTON_CONFIRM) return;
    trim(out);
    if (!out[0]) return;
    for (int i = 0; i < fav_n; i++) if (!strcmp(fav_tags[i], out)) { ui_toast_set("Already a favourite"); return; }
    if (fav_n >= FAV_MAX) { ui_toast_set("Favourite tags are full (32)"); return; }
    snprintf(fav_tags[fav_n++], sizeof fav_tags[0], "%s", out);
    fav_save();
    ui_toast_set("Added to favourite tags");
}

/* --- reset every site's API keys */
static void reset_all_keys(void)
{
    int done = 0;
    for (int i = 0; i < SITE_COUNT; i++) {
        char path[96];
        snprintf(path, sizeof path, "sdmc:/3ds/%s/config.ini", site_info[i].app);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        fclose(f);
        creds_set_names(i == SITE_DA ? "client_secret" : "api_key", i == SITE_DA ? "client_id" : "user_id");
        int rc = creds_save(path, "", "");
        if (i == SITE_E6) rc |= config_set(path, "username", "");   /* e621 login name goes with its key */
        if (i == SITE_PX) rc |= config_set(path, "phpsessid", "");  /* pixiv login cookie */
        if (i == SITE_DA) rc |= config_set(path, "refresh_token", "");   /* DeviantArt sign-in */
        log_msg("settings: reset keys in %s%s", path, rc ? " (FAILED)" : "");
        if (!rc) done++;
    }
    creds_set_names(IS_DA ? "client_secret" : "api_key", IS_DA ? "client_id" : "user_id");
    cfg.api_key[0] = 0;
    cfg.user_id[0] = 0;
    if (IS_E6) cfg.username[0] = 0;
    cfg.da_refresh[0] = 0;
    da_user[0] = 0;
    da_reauth = true;
    da_token[0] = 0;
    px_cache_drop = true;
    char msg[64];
    snprintf(msg, sizeof msg, "API keys cleared (%d site%s)", done, done == 1 ? "" : "s");
    ui_toast_set(msg);
}

static void open_options(void)
{
    mode = MODE_OPTIONS;
    opt_sel = 0;
    reset_armed_until = 0;
}

static void options_back(void)
{
    mode = MODE_GRID;
}

static void cycle_theme(int d)
{
    th.idx = (th.idx + d + THEME_COUNT) % THEME_COUNT;
    theme_apply();
    settings_save();
}

/* ---- sort picker (grid Sort button): one row per order the site offers */
#define SP_Y   28
#define SP_ROW 21
static int sort_sel;

static void open_sort(void)
{
    sort_sel = sort_index();
    mode = MODE_SORT;
}

static void set_sort(int i)
{
    int n;
    const SortOpt *o = site_sorts(&n);
    if (i < 0 || i >= n) return;
    mode = MODE_GRID;
    if (i == sort_index()) return;
    snprintf(cfg.sort, sizeof cfg.sort, "%s", o[i].value);
    ui_toast_set(o[i].label);
    if (config_set(CFG_PATH, "sort", cfg.sort)) ui_toast_set("Couldn't save the sort to config.ini");
    log_msg("settings: %s sort -> %s", SITE_NAME, o[i].label);
    nav[SRC_ONLINE].page = 0;
    if (src == SRC_ONLINE) request_list();
}

static void draw_bottom_sort(void)
{
    int n;
    const SortOpt *o = site_sorts(&n);
    char buf[64];
    snprintf(buf, sizeof buf, "Sort %s by", SITE_NAME);
    text(8, 6, 0.5f, C_ACCENT, buf);
    int cur = sort_index();
    for (int i = 0; i < n; i++) {
        float y = SP_Y + i * SP_ROW;
        C2D_DrawRectSolid(6, y, 0.5f, 308, SP_ROW - 2, i == sort_sel ? C_CELL : C_PANEL);
        if (i == sort_sel) C2D_DrawRectSolid(6, y, 0.5f, 3, SP_ROW - 2, C_ACCENT);
        text(16, y + 2, 0.45f, i == cur ? C_ACCENT : C_TEXT, o[i].label);
        if (i == cur) text(286, y + 2, 0.45f, C_ACCENT, "*");
    }
    if (n == 1) text_wrap(10, SP_Y + 30, 0.42f, C_DIM, 300, "DeviantArt's API has no other order for these listings.");
    draw_button(220, 2, 94, 22, C_PANEL, C_TEXT, "Back (B)", 0.42f);
}

static void draw_top_sort(void)
{
    int n;
    const SortOpt *o = site_sorts(&n);
    char buf[96];
    text(16, 12, 0.7f, C_ACCENT, "Sort");
    snprintf(buf, sizeof buf, "Now: %s", o[sort_index()].label);
    text(16, 44, 0.55f, C_TEXT, buf);
    text_wrap(16, 76, 0.45f, C_DIM, 368, "How this site's online searches are ordered. Only the orders the site's "
              "API offers are listed. Saved for this site; your gallery isn't affected.");
    text(16, 222, 0.4f, C_DIM, "Tap an order, or D-pad + A.   B: back");
}

static void input_sort(u32 down)
{
    int n;
    site_sorts(&n);
    if (down & KEY_B) { mode = MODE_GRID; return; }
    if (down & KEY_START) want_quit = true;
    if (down & KEY_DUP)   sort_sel = (sort_sel + n - 1) % n;
    if (down & KEY_DDOWN) sort_sel = (sort_sel + 1) % n;
    if (down & KEY_A) { set_sort(sort_sel); return; }
    if (down & KEY_TOUCH) {
        touchPosition t; hidTouchRead(&t);
        if (t.py < SP_Y - 2) { if (t.px >= 216) mode = MODE_GRID; return; }
        int i = (t.py - SP_Y) / SP_ROW;
        if (i >= 0 && i < n) set_sort(i);
    }
}

static void opt_activate(int i, int d)
{
    switch (i) {
    case OPT_THEME:  cycle_theme(d); break;
    case OPT_CUSTOM: want_custom_colors = true; break;
    case OPT_FAVS:   fav_load(); fav_scroll = 0; mode = MODE_FAVTAGS; break;
    case OPT_KEYS:   mode = MODE_SETUP; break;
    case OPT_RESET:
        if (osGetTime() < reset_armed_until) { reset_armed_until = 0; reset_all_keys(); }
        else { reset_armed_until = osGetTime() + 4000; ui_toast_set("Tap again to clear every site's API keys"); }
        break;
    case OPT_BACK:   options_back(); break;
    }
}

static void opt_label(int i, char *buf, size_t n)
{
    switch (i) {
    case OPT_THEME:  snprintf(buf, n, "Theme:  < %s >", themes[th.idx].name); break;
    case OPT_CUSTOM: snprintf(buf, n, "Custom theme colours..."); break;
    case OPT_FAVS:   snprintf(buf, n, "Favourite tags..."); break;
    case OPT_KEYS:   snprintf(buf, n, "%s key / login...", SITE_NAME); break;
    case OPT_RESET:  snprintf(buf, n, osGetTime() < reset_armed_until ? "Tap again to reset ALL keys" : "Reset all API keys"); break;
    default:         snprintf(buf, n, "Back   (B)"); break;
    }
}

static void draw_bottom_options(void)
{
    char buf[96];
    for (int i = 0; i < OPT_COUNT; i++) {
        float y = OPT_Y + i * (OPT_ROW + OPT_GAP);
        bool hi = i == opt_sel;
        u32 bg = i == OPT_RESET && osGetTime() < reset_armed_until ? C_WARN : hi ? C_CELL : C_PANEL;
        C2D_DrawRectSolid(10, y, 0.5f, 300, OPT_ROW, bg);
        if (hi) C2D_DrawRectSolid(10, y, 0.5f, 3, OPT_ROW, C_ACCENT);
        opt_label(i, buf, sizeof buf);
        u32 fg = i == OPT_RESET && osGetTime() < reset_armed_until ? C_BG : i == OPT_RESET ? C_WARN : C_TEXT;
        text_center(160, y + 5, 0.5f, fg, buf);
    }
}

static void draw_top_options(void)
{
    char buf[160];
    text(16, 12, 0.7f, C_ACCENT, "Options");
    snprintf(buf, sizeof buf, "Site: %s", SITE_NAME);
    text(250, 18, 0.45f, C_DIM, buf);
    static const char *about[OPT_COUNT] = {
        "Colours for the whole app. Left / Right or tap to change. Saved for every site.",
        "Pick your own background, text and accent colours (hex, like 1E2230). Switches the theme to Custom.",
        "Searches you use a lot, saved for this site. Tap one to run it.",
        "Enter or change this site's API key or login: scan a QR code or type it.",
        "Clears the saved API keys and logins of every site (tap twice). Galleries and settings stay.",
        "Back to the grid.",
    };
    text_wrap(16, 44, 0.5f, C_TEXT, 368, about[opt_sel]);
    /* theme preview */
    C2D_DrawRectSolid(16, 120, 0.5f, 368, 90, C_PANEL);
    for (int i = 0; i < 4; i++) C2D_DrawRectSolid(28 + i * 64, 132, 0.5f, 56, 44, i == 1 ? C_ACCENT : C_CELL);
    outline(28 + 64, 132, 56, 44, 2, C_ACCENT);
    text(28, 184, 0.45f, C_TEXT, "Text");
    text(78, 184, 0.45f, C_DIM, "Dim");
    text(118, 184, 0.45f, C_ACCENT, "Accent");
    text(178, 184, 0.45f, C_SAVED, "Gallery");
    text(238, 184, 0.45f, C_WARN, "Warning");
    text(16, 222, 0.4f, C_DIM, "D-pad: choose   A / Left / Right: change   B: back");
}

static void input_options(u32 down)
{
    if (down & KEY_B) { options_back(); return; }
    if (down & KEY_START) want_quit = true;
    if (down & KEY_DUP)   opt_sel = (opt_sel + OPT_COUNT - 1) % OPT_COUNT;
    if (down & KEY_DDOWN) opt_sel = (opt_sel + 1) % OPT_COUNT;
    if (down & KEY_A) opt_activate(opt_sel, +1);
    if ((down & KEY_DLEFT)  && opt_sel == OPT_THEME) opt_activate(opt_sel, -1);
    if ((down & KEY_DRIGHT) && opt_sel == OPT_THEME) opt_activate(opt_sel, +1);
    if (down & KEY_TOUCH) {
        touchPosition t; hidTouchRead(&t);
        for (int i = 0; i < OPT_COUNT; i++) {
            int y = OPT_Y + i * (OPT_ROW + OPT_GAP);
            if (t.py >= y && t.py < y + OPT_ROW && t.px >= 10 && t.px < 310) {
                opt_sel = i;
                /* theme / sort rows: left third goes back, the rest forward */
                opt_activate(i, i == OPT_THEME && t.px < 110 ? -1 : +1);
            }
        }
    }
}

/* custom colours: three keyboards in a row */
static bool ask_hex(const char *hint, unsigned cur, bool allow_site, unsigned *out)
{
    SwkbdState kb;
    char init[16], v[32];
    if (allow_site && cur == 0xFFFFFFFFu) snprintf(init, sizeof init, "site");
    else snprintf(init, sizeof init, "%06X", cur & 0xFFFFFF);
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 12);
    swkbdSetHintText(&kb, hint);
    swkbdSetInitialText(&kb, init);
    if (swkbdInputText(&kb, v, sizeof v) != SWKBD_BUTTON_CONFIRM) return false;
    trim(v);
    if (allow_site && (!v[0] || !strcasecmp(v, "site"))) { *out = 0xFFFFFFFFu; return true; }
    if (!parse_hex(v, out)) { ui_toast_set("Use 6 hex digits, like 1E2230"); return false; }
    return true;
}

static void open_custom_colors(void)
{
    unsigned bg = th.custom_bg, tx = th.custom_text, ac = th.custom_accent;
    if (!ask_hex("background colour (hex RRGGBB)", bg, false, &bg)) return;
    if (!ask_hex("text colour (hex RRGGBB)", tx, false, &tx)) return;
    if (!ask_hex("accent colour (hex), or 'site' for each site's colour", ac, true, &ac)) return;
    if (fabsf(luma(bg) - luma(tx)) < 0.35f) ui_toast_set("Low contrast - text may be hard to read");
    th.custom_bg = bg; th.custom_text = tx; th.custom_accent = ac;
    th.idx = THEME_CUSTOM;
    theme_apply();
    settings_save();
}

/* --- favourite tags screen */
#define FT_Y     30
#define FT_ROW   26
#define FT_H     (214 - FT_Y)

static void draw_bottom_favtags(void)
{
    for (int i = 0; i < fav_n; i++) {
        float y = FT_Y + i * FT_ROW - fav_scroll;
        if (y + FT_ROW < FT_Y || y > FT_Y + FT_H) continue;
        C2D_DrawRectSolid(0, y, 0.5f, 320, FT_ROW - 2, (i & 1) ? C_PANEL : C_CELL);
        char buf[64];
        snprintf(buf, sizeof buf, "%.40s%s", fav_tags[i], strlen(fav_tags[i]) > 40 ? "..." : "");
        text(8, y + 4, 0.47f, C_TEXT, buf);
        draw_button(288, y + 2, 28, FT_ROW - 6, C_BG, C_WARN, "x", 0.5f);
    }
    if (!fav_n) text_center(160, 100, 0.5f, C_DIM, "No favourite tags yet");
    C2D_DrawRectSolid(0, 0, 0.5f, 320, FT_Y - 2, C_BG);
    draw_button(4, 2, 200, 24, C_ACCENT, C_BG, "+ Add (current search)", 0.45f);
    draw_button(210, 2, 106, 24, C_CELL, C_TEXT, "Back (B)", 0.45f);
    C2D_DrawRectSolid(0, 216, 0.5f, 320, 24, C_BG);
    text(6, 222, 0.38f, C_DIM, "Tap a search to run it   x removes it   drag to scroll");
}

static void draw_top_favtags(void)
{
    char buf[96];
    snprintf(buf, sizeof buf, "Favourite tags - %s", SITE_NAME);
    text(16, 12, 0.7f, C_ACCENT, buf);
    text_wrap(16, 44, 0.5f, C_TEXT, 368,
              "Saved searches for this site. Tap one on the touch screen to search it online. "
              "+ Add saves the current search (you can edit it first). Up to 32.");
    char path[96];
    fav_path(path, sizeof path);
    snprintf(buf, sizeof buf, "%d saved   (%s)", fav_n, path);
    text(16, 216, 0.4f, C_DIM, buf);
}

static void input_favtags(u32 down, u32 held, u32 up)
{
    if (down & KEY_B) { mode = MODE_OPTIONS; return; }
    if (down & KEY_START) want_quit = true;
    float maxs = fav_n * FT_ROW - FT_H;
    if (maxs < 0) maxs = 0;
    touchPosition t;
    if (down & KEY_TOUCH) {
        hidTouchRead(&t);
        tp_active = true; tp_moved = false;
        tp_x0 = tp_x = t.px; tp_y0 = tp_y = t.py;
        tp_scroll0 = fav_scroll;
    } else if (tp_active && (held & KEY_TOUCH)) {
        hidTouchRead(&t);
        tp_x = t.px; tp_y = t.py;
        if (tp_y0 >= FT_Y && abs(tp_y - tp_y0) > 6) tp_moved = true;
        if (tp_moved) {
            fav_scroll = tp_scroll0 - (tp_y - tp_y0);
            if (fav_scroll < 0) fav_scroll = 0;
            if (fav_scroll > maxs) fav_scroll = maxs;
        }
    }
    if (!(tp_active && (up & KEY_TOUCH))) return;
    tp_active = false;
    if (tp_moved) return;
    int x = tp_x, y = tp_y;
    if (y < FT_Y - 2) {
        if (x < 206) want_fav_add = true;
        else mode = MODE_OPTIONS;
        return;
    }
    if (y >= 216) return;
    int i = (int)((y - FT_Y + fav_scroll) / FT_ROW);
    if (i < 0 || i >= fav_n) return;
    if (x >= 284) {                                  /* remove */
        memmove(fav_tags[i], fav_tags[i + 1], (fav_n - i - 1) * sizeof fav_tags[0]);
        fav_n--;
        fav_save();
        ui_toast_set("Removed");
        if (fav_scroll > maxs) fav_scroll = maxs > 0 ? maxs : 0;
        return;
    }
    char q[128];
    snprintf(q, sizeof q, "%s", fav_tags[i]);
    src = SRC_ONLINE;
    run_search(q);
}

/* ---------------------------------------------------------------- gallery folders
 * MODE_FOLDERS: list of folders (open one, rename, delete, + New).
 * MODE_FOLDERPICK: from the viewer, tick the folders the current image is in.
 * Folders only group post IDs (folders.txt); images stay in the gallery. */

#define FL_Y     26              /* list area */
#define FL_ROW   30
#define FL_BOT   210
#define FL_RENX  204             /* Rename / Delete buttons on folder rows */
#define FL_RENW  54
#define FL_DELX  262
#define FL_DELW  54

static float fl_scroll, fl_scroll0;
static bool  fl_active, fl_moved;
static int   fl_x, fl_y, fl_y0;
static int   fl_del_idx = -1;
static u64   fl_del_until;
static bool  want_new_folder, new_folder_add_cur;
static int   want_rename = -1;

static void fl_reset(void) { fl_scroll = 0; fl_active = false; fl_del_idx = -1; }

static void open_folders(void)     { fl_reset(); mode = MODE_FOLDERS; }
static void open_folder_pick(void) { fl_reset(); mode = MODE_FOLDERPICK; }

static void folders_store(void)
{
    if (folders_save(&folders, FOLDERS_PATH)) { log_msg("folders: save failed"); ui_toast_set("Couldn't save folders.txt"); }
}

/* Drag-to-scroll list touch handling. Returns true on a tap (release without drag). */
static bool fl_touch(u32 down, u32 held, u32 up, int rows)
{
    touchPosition t;
    if (down & KEY_TOUCH) {
        hidTouchRead(&t);
        fl_active = true; fl_moved = false;
        fl_x = t.px; fl_y = fl_y0 = t.py;
        fl_scroll0 = fl_scroll;
    } else if (fl_active && (held & KEY_TOUCH)) {
        hidTouchRead(&t);
        fl_x = t.px; fl_y = t.py;
        if (fl_y0 >= FL_Y && fl_y0 < FL_BOT && abs(fl_y - fl_y0) > 6) fl_moved = true;
        if (fl_moved) {
            float mx = rows * FL_ROW - (FL_BOT - FL_Y);
            fl_scroll = fl_scroll0 - (fl_y - fl_y0);
            if (fl_scroll > mx) fl_scroll = mx;
            if (fl_scroll < 0) fl_scroll = 0;
        }
    }
    if (fl_active && (up & KEY_TOUCH)) { fl_active = false; return !fl_moved; }
    return false;
}

static int fl_row_at(int y, int rows)
{
    if (y < FL_Y || y >= FL_BOT) return -1;
    int r = (int)((y - FL_Y + fl_scroll) / FL_ROW);
    return r < rows ? r : -1;
}

static void draw_fl_header(const char *title)
{
    C2D_DrawRectSolid(0, 0, 0.5f, 320, 24, C_PANEL);
    text(8, 4, 0.5f, C_SAVED, title);
    draw_button(244, 2, 74, 20, C_CELL, C_TEXT, "+ New", 0.45f);
}

static void draw_fl_footer(const char *label)
{
    C2D_DrawRectSolid(0, FL_BOT, 0.5f, 320, 240 - FL_BOT, C_BG);
    C2D_DrawRectSolid(0, FL_BOT, 0.5f, 320, 1, C_CELL);
    draw_button(100, FL_BOT + 5, 120, 22, C_CELL, C_TEXT, label, 0.45f);
}

static void folders_leave(void)
{
    mode = MODE_GRID;
    if (gallery_dirty) { gallery_dirty = false; cur_page = 0; request_list(); }
}

static void draw_top_folders(void)
{
    text(16, 12, 0.7f, C_SAVED, "Gallery folders");
    char buf[96];
    snprintf(buf, sizeof buf, "Showing: %s", cur_folder >= 0 && cur_folder < folders.n ? folders.f[cur_folder].name : "all images");
    text(16, 44, 0.5f, C_TEXT, buf);
    text_wrap(16, 74, 0.45f, C_DIM, 368,
              "Tap a folder to view it. To add an image to folders, open it from the gallery "
              "and tap the folder button next to Clear. Folders only group images: deleting a "
              "folder keeps its images, and deleting an image removes it from every folder.");
    snprintf(buf, sizeof buf, "%d folder%s   (saved in %s)", folders.n, folders.n == 1 ? "" : "s", folders_path);
    text(16, 220, 0.4f, C_DIM, buf);
}

static void draw_bottom_folders(void)
{
    char buf[96];
    int rows = folders.n + 1;
    for (int r = 0; r < rows; r++) {
        float y = FL_Y + r * FL_ROW - fl_scroll;
        if (y + FL_ROW < FL_Y || y > FL_BOT) continue;
        bool current = (r == 0 && cur_folder < 0) || (r > 0 && cur_folder == r - 1);
        C2D_DrawRectSolid(0, y, 0.5f, 320, FL_ROW - 2, current ? tint(C_PANEL, C_SAVED, 0.2f) : C_PANEL);
        draw_folder_icon(8, y + 8, 16, r == 0 ? C_DIM : C_SAVED);
        if (r == 0) {
            text(32, y + 7, 0.48f, C_TEXT, "All images");
            continue;
        }
        Folder *f = &folders.f[r - 1];
        snprintf(buf, sizeof buf, "%.22s%s (%d)", f->name, strlen(f->name) > 22 ? ".." : "", f->n);
        text(32, y + 7, 0.45f, C_TEXT, buf);
        draw_button(FL_RENX, y + 4, FL_RENW, FL_ROW - 10, C_CELL, C_TEXT, "Rename", 0.4f);
        bool armed = fl_del_idx == r - 1 && osGetTime() < fl_del_until;
        draw_button(FL_DELX, y + 4, FL_DELW, FL_ROW - 10, armed ? C_WARN : C_CELL, armed ? C_BG : C_WARN,
                    armed ? "Sure?" : "Delete", 0.4f);
    }
    draw_fl_header("Folders");
    draw_fl_footer("Back (B)");
}

static void input_folders(u32 down, u32 held, u32 up)
{
    if (down & KEY_B) { folders_leave(); return; }
    if (down & KEY_START) want_quit = true;
    int rows = folders.n + 1;
    if (!fl_touch(down, held, up, rows)) return;
    int x = fl_x, y = fl_y0;
    if (y < 24) { if (x >= 244) { want_new_folder = true; new_folder_add_cur = false; } return; }
    if (y >= FL_BOT) { if (x >= 100 && x < 220) folders_leave(); return; }
    int r = fl_row_at(y, rows);
    if (r < 0) return;
    int i = r - 1;
    if (r > 0 && x >= FL_DELX) {                    /* delete: tap twice */
        if (fl_del_idx != i || osGetTime() > fl_del_until) {
            fl_del_idx = i;
            fl_del_until = osGetTime() + 3000;
            ui_toast_set("Tap again to delete the folder (images are kept)");
            return;
        }
        log_msg("folders: deleted '%s' (%d images)", folders.f[i].name, folders.f[i].n);
        folders_delete(&folders, i);
        folders_store();
        fl_del_idx = -1;
        if (cur_folder == i) { cur_folder = -1; gallery_dirty = true; }
        else if (cur_folder > i) cur_folder--;
        ui_toast_set("Folder deleted");
        return;
    }
    if (r > 0 && x >= FL_RENX) { want_rename = i; return; }
    /* open: whole gallery or this folder */
    cur_folder = i;
    cur_tags[0] = 0;
    cur_page = 0;
    gallery_dirty = false;
    mode = MODE_GRID;
    request_list();
}

static void draw_bottom_pick(void)
{
    char buf[96];
    unsigned id = sel < ui_count ? ui_posts[sel].id : 0;
    for (int r = 0; r < folders.n; r++) {
        float y = FL_Y + r * FL_ROW - fl_scroll;
        if (y + FL_ROW < FL_Y || y > FL_BOT) continue;
        bool in = folders_has(&folders, r, id);
        C2D_DrawRectSolid(0, y, 0.5f, 320, FL_ROW - 2, in ? tint(C_PANEL, C_SAVED, 0.2f) : C_PANEL);
        C2D_DrawRectSolid(10, y + 7, 0.5f, 14, 14, in ? C_SAVED : C_CELL);         /* checkbox */
        if (in) { C2D_DrawRectSolid(14, y + 11, 0.5f, 6, 6, C_BG); }
        snprintf(buf, sizeof buf, "%.30s%s (%d)", folders.f[r].name, strlen(folders.f[r].name) > 30 ? ".." : "", folders.f[r].n);
        text(32, y + 7, 0.45f, in ? C_TEXT : C_DIM, buf);
    }
    if (!folders.n) {
        text_center(160, 90, 0.5f, C_DIM, "No folders yet");
        text_center(160, 112, 0.45f, C_DIM, "Tap + New to make one for this image");
    }
    snprintf(buf, sizeof buf, "Folders for #%u", id);
    draw_fl_header(buf);
    draw_fl_footer("Done (B)");
}

static void input_pick(u32 down, u32 held, u32 up)
{
    if (down & (KEY_B | KEY_A)) { mode = MODE_VIEW; return; }
    if (down & KEY_START) want_quit = true;
    if (!fl_touch(down, held, up, folders.n)) return;
    int x = fl_x, y = fl_y0;
    if (y < 24) { if (x >= 244) { want_new_folder = true; new_folder_add_cur = true; } return; }
    if (y >= FL_BOT) { if (x >= 100 && x < 220) mode = MODE_VIEW; return; }
    int r = fl_row_at(y, folders.n);
    if (r < 0 || sel >= ui_count) return;
    int res = folders_toggle(&folders, r, ui_posts[sel].id);
    if (res < 0) { ui_toast_set("Out of memory"); return; }
    folders_store();
    if (src == SRC_SAVED && cur_folder == r) gallery_dirty = true;
    char msg[80];
    snprintf(msg, sizeof msg, "%s %.30s", res ? "Added to" : "Removed from", folders.f[r].name);
    ui_toast_set(msg);
}

static void open_new_folder_kb(void)
{
    SwkbdState kb;
    char out[64];
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, FOLDER_NAME_MAX - 1);
    swkbdSetHintText(&kb, "new folder name");
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (swkbdInputText(&kb, out, sizeof out) != SWKBD_BUTTON_CONFIRM) return;
    int i = folders_add(&folders, out);
    if (i == -2) { ui_toast_set("A folder with that name already exists"); return; }
    if (i == -3) { ui_toast_set("Too many folders (64)"); return; }
    if (i < 0)   { ui_toast_set("That name can't be used"); return; }
    if (new_folder_add_cur && sel < ui_count) folders_toggle(&folders, i, ui_posts[sel].id);
    folders_store();
    log_msg("folders: created '%s'", folders.f[i].name);
    char msg[80];
    snprintf(msg, sizeof msg, new_folder_add_cur ? "Added to new folder %.30s" : "Created %.30s", folders.f[i].name);
    ui_toast_set(msg);
}

static void open_rename_kb(int i)
{
    if (i < 0 || i >= folders.n) return;
    SwkbdState kb;
    char out[64];
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, FOLDER_NAME_MAX - 1);
    swkbdSetHintText(&kb, "folder name");
    swkbdSetInitialText(&kb, folders.f[i].name);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (swkbdInputText(&kb, out, sizeof out) != SWKBD_BUTTON_CONFIRM) return;
    char old[FOLDER_NAME_MAX];
    snprintf(old, sizeof old, "%s", folders.f[i].name);
    int r = folders_rename(&folders, i, out);
    if (r == -2) { ui_toast_set("A folder with that name already exists"); return; }
    if (r < 0)   { ui_toast_set("That name can't be used"); return; }
    folders_store();
    log_msg("folders: renamed '%s' -> '%s'", old, folders.f[i].name);
    ui_toast_set("Folder renamed");
}

/* ---------------------------------------------------------------- controls screen
 * Opened with the "?" buttons; shows the controls for the screen it came from.
 * Tap anywhere or press B (or A) to go back. */

typedef struct { const char *key, *what; } HelpRow;

static void draw_help_rows(const HelpRow *rows, int n, float y, float keyw, float sz)
{
    for (int i = 0; i < n; i++, y += sz * 36) {
        text(12, y, sz, C_ACCENT, rows[i].key);
        text(12 + keyw, y, sz, C_TEXT, rows[i].what);
    }
}

static void draw_top_help(void)
{
    bool gal = src == SRC_SAVED;
    if (help_return == MODE_VIEW) {
        text(12, 8, 0.6f, C_ACCENT, "Controls - viewing an image");
        HelpRow r[] = {
            { "Y",                         "fit / fill the screen" },
            { "Circle Pad, C-stick",       "move the image" },
            { "D-pad Up/Down, ZR/ZL",      "zoom in / out" },
            { "A",                         "reset zoom (video: pause / play)" },
            { "D-pad Left/Right",          "previous / next post" },
            { "L or R (tap)",              "previous / next post" },
            { "R + D-pad Right",           "rotate 90 right" },
            { "L + D-pad Left",            "rotate 90 left" },
            { "X",                         gal ? "delete (press X twice)" : "save to gallery" },
            { "SELECT",                    gal ? "switch to online search" : "switch to gallery" },
            { "B",                         "back to the grid" },
            { "START",                     "quit" },
        };
        draw_help_rows(r, sizeof r / sizeof r[0], 34, 150, 0.42f);
    } else {
        text(12, 8, 0.6f, C_ACCENT, gal ? "Controls - gallery" : "Controls - browsing");
        HelpRow r[] = {
            { "D-pad",                     "select a post" },
            { "A",                         "open the selected post" },
            { "L / R",                     "previous / next page" },
            { "Y",                         gal ? "filter by tags or post ID" : "search tags (-tag excludes)" },
            { "X",                         gal ? "delete (press X twice)" : "save to gallery" },
            { "SELECT",                    gal ? "switch to online search" : "switch to gallery" },
            { "START",                     "quit" },
        };
        draw_help_rows(r, sizeof r / sizeof r[0], 40, 110, 0.5f);
    }
}

static void draw_bottom_help(void)
{
    text(12, 8, 0.55f, C_ACCENT, "Touch screen");
    if (help_return == MODE_VIEW) {
        HelpRow r[] = {
            { "+",            "include tag in search" },
            { "-",            "exclude tag" },
            { "tag name",     "same as +" },
            { "drag list",    "scroll the tags" },
            { "search line",  "edit it with the keyboard" },
            { "Search",       "run the search" },
            { "Clear / Back", "empty the search / grid" },
            { "disk/folder",  "save / add to a folder" },
            { "time bar",     "tap to jump (videos)" },
            { "two screens",  "2nd image on this screen" },
        };
        draw_help_rows(r, sizeof r / sizeof r[0], 30, 96, 0.42f);
        text(12, 186, 0.4f, C_DIM, "Picked tags carry over between posts.");
    } else {
        HelpRow r[10] = {
            { "post",         "select / tap again to open" },
            { "search bar",   "type a search" },
            { "< >",          "previous / next page" },
            { "page number",  "jump to a page" },
            { "Folders",      "gallery folders (gallery)" },
            { "album",        "gallery / back to online" },
            { "menu (=)",     "options: theme, favourites, keys" },
        };
        int n = 7;
        if (IS_DA || IS_E6 || IS_PX) r[n++] = (HelpRow){ "* Faves",  "your favourites (online)" };
        if (MULTI)  r[n++] = (HelpRow){ "site tab", "switch site (own gallery each)" };
        if (src == SRC_ONLINE) r[n++] = (HelpRow){ "arrows",   "sort order for this site" };
        else                   r[n++] = (HelpRow){ "shuffle",  "random order / normal order" };
        draw_help_rows(r, n, 36, 96, 0.45f);
    }
    draw_button(60, 204, 200, 30, C_ACCENT, C_BG, "Close (tap or B)", 0.5f);
}

static void input_help(u32 down)
{
    if (down & (KEY_B | KEY_A | KEY_TOUCH)) mode = help_return;
    if (down & KEY_START) want_quit = true;
}

/* ---------------------------------------------------------------- main */

static void log_startup(void)
{
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    u32 kv = osGetKernelVersion();
    log_msg(BUILD_NAME " %s, %s (built %s %s)", APP_VERSION, MULTI ? "all sites" : SITE_NAME, __DATE__, __TIME__);
    log_msg("system: %s, kernel %lu.%lu-%lu", n3ds ? "New 3DS" : "Old 3DS",
            (unsigned long)GET_VERSION_MAJOR(kv), (unsigned long)GET_VERSION_MINOR(kv),
            (unsigned long)GET_VERSION_REVISION(kv));
    log_mem("startup");
}

static void log_config(void)
{
    log_msg("site: %s, folder %s", SITE_NAME, app_dir);
    log_msg("config: api_key %u chars%s, user_id %s, verify_ssl %d, verbose_log %d",
            (unsigned)strlen(cfg.api_key), !KEY_LEN || strlen(cfg.api_key) == KEY_LEN || !cfg.api_key[0] ? "" : " (unexpected length!)",
            cfg.user_id[0] ? cfg.user_id : "(none)", cfg.verify_ssl, cfg.verbose_log);
    log_msg("config: default_tags '%s', extra_tags '%s'", cfg.default_tags, cfg.extra_tags);
}

/* Loads the current site's config and folders (startup, and on switching sites). */
static void load_site(void)
{
    static bool started;
    static bool nav_ready[SITE_COUNT];
    int verify = cfg.verify_ssl, verbose = cfg.verbose_log;
    site_paths();
    ensure_dirs();
    memset(&cfg, 0, sizeof cfg);
    cfg.verify_ssl = 1;
    load_config();
    if (started) {                       /* connection/log settings are app-wide once running */
        cfg.verify_ssl = verify;
        cfg.verbose_log = verbose;
    }
    started = true;
    creds_set_names(IS_DA ? "client_secret" : "api_key", IS_DA ? "client_id" : "user_id");
    if (IS_DA && !strcmp(cfg.redirect_uri, "http://localhost/")) {      /* old default: DeviantArt needs https */
        snprintf(cfg.redirect_uri, sizeof cfg.redirect_uri, "%s", DA_DEFAULT_REDIRECT);
        config_set(CFG_PATH, "redirect_uri", cfg.redirect_uri);
        log_msg("config: redirect_uri changed to %s", cfg.redirect_uri);
    }
    folders_load(&folders, FOLDERS_PATH);
    cur_folder = -1;
    gallery_dirty = false;
    da_token[0] = 0;                     /* DeviantArt login and paging state */
    da_user[0] = 0;
    da_query[0] = 0;
    if (!nav_ready[CUR_SITE]) {
        nav_ready[CUR_SITE] = true;
        snprintf(nav[SRC_ONLINE].tags, sizeof nav[SRC_ONLINE].tags, "%s", cfg.default_tags);
    }
    log_config();
    log_msg("folders: %d loaded from %s", folders.n, folders_path);
}

#define LAST_SITE_PATH "sdmc:/3ds/" BUILD_NAME "/site.txt"

static void __attribute__((unused)) write_last_site(void)
{
#ifdef SITE_MULTI
    FILE *f = fopen(LAST_SITE_PATH, "w");
    if (!f) return;
    fprintf(f, "%s\n", site_info[g_site].app);
    fclose(f);
#endif
}

static void read_last_site(void)
{
#ifdef SITE_MULTI
    char line[64] = "";
    FILE *f = fopen(LAST_SITE_PATH, "r");
    if (!f) return;
    if (fgets(line, sizeof line, f)) {
        trim(line);
        for (int i = 0; i < SITE_COUNT; i++)
            if (!strcmp(line, site_info[i].app)) g_site = i;
    }
    fclose(f);
#endif
}

int main(void)
{
    osSetSpeedupEnable(true);
    romfsInit();

    /* config first: it decides the top screen mode */
    read_last_site();
    site_paths();
    ensure_dirs();
    /* log in the build's own folder: sdmc:/3ds/artview (combined) or the site's folder */
    char log_old[64];
    snprintf(log_path, sizeof log_path, "sdmc:/3ds/%s/log.txt", BUILD_NAME);
    snprintf(log_old, sizeof log_old, "sdmc:/3ds/%s/log.old.txt", BUILD_NAME);
    log_open(log_path, log_old);
    log_startup();
    settings_load();
    load_site();
    log_set_verbose(cfg.verbose_log);
    fill_mode = cfg.fill_screen;

    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    /* citro2d enables a depth test by default; every text call uses z=0.5, so text
     * drawn over a shape with a higher z (toast bar, loading box, badges) was
     * rejected, leaving an empty translucent box. Everything here is drawn
     * back-to-front, so draw order alone decides what's on top. */
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    tbuf = C2D_TextBufNew(4096);

    LightLock_Init(&lock);
    LightLock_Init(&site_lock);
    LightEvent_Init(&wake, RESET_ONESHOT);

    net_ok = net_init(cfg.verify_ssl, neterr, sizeof neterr) == 0;
    if (!net_ok) {
        log_msg("network unavailable (%s), starting in gallery", neterr);
        src = SRC_SAVED;
    }

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread worker = threadCreate(worker_main, NULL, 64 * 1024, prio + 1, 2, false);   /* N3DS core 2 */
    if (!worker) {
        log_msg("worker: core 2 unavailable, using any core");
        worker = threadCreate(worker_main, NULL, 64 * 1024, prio + 1, -2, false);
    }
    if (!worker) log_msg("worker: threadCreate FAILED");
    request_list();
    if (KEY_REQUIRED && net_ok && (!cfg.api_key[0] || (KEY_NEEDS_UID && !cfg.user_id[0]))) {
        log_msg("config: no API credentials, opening key setup");
        mode = MODE_SETUP;
    }

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp();

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TextBufClear(tbuf);

        site_switch_poll();
        sync_worker();
        dual_sync();
        qr_sync();
        gif_sync();
        video_sync();
        switch (mode) {
        case MODE_GRID:  input_grid(down); break;
        case MODE_VIEW:  input_view(down, held, up); break;
        case MODE_SETUP: input_setup(down); break;
        case MODE_QR:    input_qr(down); break;
        case MODE_HELP:  input_help(down); break;
        case MODE_FOLDERS:    input_folders(down, held, up); break;
        case MODE_FOLDERPICK: input_pick(down, held, up); break;
        case MODE_OPTIONS:    input_options(down); break;
        case MODE_FAVTAGS:    input_favtags(down, held, up); break;
        case MODE_DALOGIN:    input_dalogin(down); break;
        case MODE_SORT:       input_sort(down); break;
        }

        C2D_TargetClear(top, C_BG);
        C2D_SceneBegin(top);
        C2D_ViewReset();
        switch (mode) {
        case MODE_GRID:  draw_top_grid(); break;
        case MODE_VIEW:  draw_top_view(); break;
        case MODE_SETUP: draw_top_setup(); break;
        case MODE_QR:    draw_top_qr(); break;
        case MODE_HELP:  draw_top_help(); break;
        case MODE_FOLDERS:    draw_top_folders(); break;
        case MODE_FOLDERPICK: draw_top_view(); break;
        case MODE_OPTIONS:    draw_top_options(); break;
        case MODE_FAVTAGS:    draw_top_favtags(); break;
        case MODE_DALOGIN:    draw_top_dalogin(); break;
        case MODE_SORT:       draw_top_sort(); break;
        }

        C2D_TargetClear(bot, C_BG);
        C2D_SceneBegin(bot);
        C2D_ViewReset();
        switch (mode) {
        case MODE_GRID:  draw_bottom_grid(); break;
        case MODE_VIEW:  if (dual) draw_bottom_dual(); else draw_bottom_view(); break;
        case MODE_SETUP: draw_bottom_setup(); break;
        case MODE_QR:    draw_bottom_qr(); break;
        case MODE_HELP:  draw_bottom_help(); break;
        case MODE_FOLDERS:    draw_bottom_folders(); break;
        case MODE_FOLDERPICK: draw_bottom_pick(); break;
        case MODE_OPTIONS:    draw_bottom_options(); break;
        case MODE_FAVTAGS:    draw_bottom_favtags(); break;
        case MODE_DALOGIN:    draw_bottom_dalogin(); break;
        case MODE_SORT:       draw_bottom_sort(); break;
        }
        draw_toast();

        C3D_FrameEnd(0);

        if (want_quit) break;
        if (want_search) { want_search = false; open_search(); }
        if (want_page_kb) { want_page_kb = false; if (mode == MODE_GRID) open_page_kb(); }
        if (want_qedit)  { want_qedit = false; open_query_edit(); }
        if (want_key_kb) { want_key_kb = false; open_key_kb(); }
        if (want_uid_kb) { want_uid_kb = false; open_uid_kb(); }
        if (want_new_folder) { want_new_folder = false; open_new_folder_kb(); }
        if (want_user_kb) { want_user_kb = false; open_user_kb(); }
        if (want_custom_colors) { want_custom_colors = false; open_custom_colors(); }
        if (want_fav_add) { want_fav_add = false; open_fav_add_kb(); }
        if (want_da_code_kb) { want_da_code_kb = false; open_da_code_kb(); }
        if (want_redirect_kb) { want_redirect_kb = false; open_redirect_kb(); }
        if (da_signin_lost) { da_signin_lost = false; ui_toast_set("DeviantArt sign-in expired - sign in again (key screen)"); }
        if (want_rename >= 0) { int i = want_rename; want_rename = -1; open_rename_kb(i); }
    }

    log_msg("exiting");
    cam_stop();
    gif_stop();
    video_stop();
    running = false;
    LightLock_Lock(&lock);
    list_gen++;          /* cancel any transfer */
    LightLock_Unlock(&lock);
    LightEvent_Signal(&wake);
    if (worker) { threadJoin(worker, U64_MAX); threadFree(worker); }

    for (int i = 0; i < PER_PAGE; i++) slot_reset(&thumbs[i]);
    slot_reset(&full);
    slot_reset(&full2);
    net_exit();
    log_close();
    C2D_TextBufDelete(tbuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    romfsExit();
    return 0;
}
