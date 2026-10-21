# artview (r34view, derpiview, daview, e6view, pxview)

A rule34.xxx, Derpibooru, DeviantArt, e621 and pixiv browser for the New Nintendo 3DS (also runs on old 3DS, just slower).

## artview: all five sites in one app

`build.bat` / `make` now builds **artview**: one `.3dsx` / `.cia` (title ID `0004000000B3E700`) with all five sites.

- **Site tab:** the button in the middle of the grid's bottom row shows the current site in its colour (green rule34, purple Derpibooru, teal DeviantArt, gold e621, blue pixiv). Tap it to switch to the next one. The app remembers the last site you used.
- **Each site keeps its own everything:** login, settings, gallery and gallery folders, plus the search and page you were on.
  - It uses the same folders as the single-site apps (`sdmc:/3ds/r34view/`, `/derpiview/`, `/daview/`, `/e6view/`, `/pxview/`), so galleries and logins you already have show up as they are, with nothing to move. You can keep the old apps installed alongside it.
- **Keys:** switching to a site without its key opens that site's key screen. Press B to go back and use its gallery without one.
- **Gallery or online:** SELECT still switches between online and gallery, and the site tab works in both.
- **Downloads:** if you switch while a download is still saving, artview finishes saving it into the old site's gallery first.
- **Shared files:** artview keeps one log for all sites in `sdmc:/3ds/artview/log.txt` (plus `site.txt`, the last site). Connection and log settings (`verify_ssl`, `verbose_log`) come from the site you start on.
- **Art:** HOME Menu icon, banner and sound are in `meta-all/`.

**Single-site apps** still build too: `build-r34.bat`, `build-derpi.bat`, `build-da.bat`, `build-e6.bat`, `build-px.bat` (each also takes `cia` / `clean`), or `make SITE=rule34|derpibooru|deviantart|e621|pixiv [cia]`. The sections below describe each site. Everything said there applies inside artview as well; just switch to that site with the tab.

## Options (menu button)

Tap the **menu** button (three lines) in the grid's bottom row for theme, favourite tags and keys. Sort has its own button.

- **Theme:** Slate (the original), OLED black, Midnight blue, Forest, Plum, Light, or **Custom**. Tap the row, or use Left/Right, to change it. It applies to the whole app, every site.
- **Custom theme colours:** type three hex colours (`RRGGBB`, like `1E2230`): background, text and accent. For the accent, `site` keeps each site's own colour. Panels and buttons are worked out from the background and text. Themes are saved in `settings.ini` in the app's folder (`sdmc:/3ds/artview/`).
- **Sort** (the up/down arrows button in the grid's bottom row, left of the site tab, while browsing online): pick how this site's online searches are ordered. The arrows turn the accent colour when a non-default order is on. Each site only lists the orders its API supports:

  | Site | Sort options |
  |---|---|
  | rule34.xxx | Newest, Highest score, Recently updated, Oldest, Random |
  | Derpibooru | Newest, Highest score, Best (Wilson score), Most faves, Most upvotes, Most comments, Biggest, Oldest, Random |
  | e621 | Newest, Highest score, Most favourites, Hot right now, Most comments, Biggest, Longest videos, Oldest, Random |
  | DeviantArt | Newest only: its API has no sort option for tag, gallery or favourites listings |
  | pixiv | Newest, Oldest, Newest all-ages only, Newest R-18 only (needs login), Popular (needs pixiv Premium) |

  The sort is saved per site (`sort=` in that site's `config.ini`).
- **Favourite tags:** saved searches for the current site (up to 32, in its `favtags.txt`).
  - **+ Add** saves the current search; you can edit it first.
  - Tap a saved search to run it; **x** removes it.
- **Key / login:** opens the current site's key screen (scan a QR code or type it).
- **Reset all API keys:** tap twice. This clears the saved keys and logins of every site, including the DeviantArt app keys and the e621 username. Galleries, favourites and other settings stay.

## e6view: e621

- **Login is optional.** As a guest you can browse, but e621 hides some posts from logged-out users (the log counts how many).
  - To log in, open e621.net → Account → **Manage API Access** on a phone or PC and create a key.
  - Then scan a QR code of `login=YOURNAME&api_key=YOURKEY`, or use Options → **e621 key / login** to type them.
- **Search** uses normal e621 syntax: `fox rating:s -animated`. The sort from Options is added as an `order:` tag, so leave `order:` out of your search.
- **Faves:** tap **\* Faves** for your favourites (`fav:yourname`). You can also search anyone's favourites with `fav:name`.
- **Videos:** saving a video (X / disk icon) downloads e621's MP4 version at up to 720p rather than the WebM, so it plays in the gallery on a New 3DS. Videos e621 has no small enough MP4 for save as the original file, preview only.
- Its own app: `build-e6.bat` / `make SITE=e621`, title ID `0004000000B3E800`, folder `sdmc:/3ds/e6view/`, art in `meta-e6/`.

## pxview: pixiv

pixiv has no public API, so this uses the same JSON requests pixiv's own website makes. That's unofficial: pixiv can change it without notice, which would break this until it's updated. Use it at your own discretion.

- **Without logging in** you can search tags and browse the rankings; R-18 works are hidden.
- **Logging in (optional)** uses your browser's login cookie:
  - In a PC browser logged in to pixiv.net, open developer tools → Application (Chrome) or Storage (Firefox) → Cookies → `https://www.pixiv.net` and copy the value of **PHPSESSID**.
  - Then scan a QR code of it, or type it under Options → **pixiv key / login**. You can also put `phpsessid=` in `sdmc:/3ds/pxview/config.ini`.
  - The cookie works like your password, so keep it and its QR code private. It's hidden in the log. Logging out of pixiv in that browser cancels it, and the key screen has **Log out**.
- **Search:** tags separated by spaces, all required; `-tag` excludes one. pixiv tags are mostly Japanese, but searching English words or romaji usually finds works too.
  - An empty search shows today's **ranking**. `ranking:weekly`, `ranking:monthly`, `ranking:rookie` and `ranking:original` show the others, and with the R-18 sort and a login you get the R-18 rankings.
  - `fav:12345` shows that user's public bookmarks.
- **Faves:** tap **\* Faves** for your bookmarks. With a login cookie, your user ID is read from it; otherwise it asks for the number in your profile link (`pixiv.net/users/12345`).
- **Viewing:** the viewer shows the first page of each work at up to 1200px. Multi-page works show `N_pages` at the start of their tags; the other pages aren't shown yet. Animated works (ugoira) show a still frame.
- **Saving:** X / disk icon saves the original file (PNG or JPEG) when pixiv allows it, otherwise the 1200px version.
- Its own app: `build-px.bat` / `make SITE=pixiv`, title ID `0004000000B3E900`, folder `sdmc:/3ds/pxview/`, art in `meta-px/`.

## The app

- Bottom screen: tag search bar and a 4×3 thumbnail grid (touch or D-pad)
- Tag picker: tap + / − next to a post's tags to build a new search
- Top screen: selected post's info and tags, or the full image with zoom and pan
- **Y** while viewing switches between fit (whole image) and fill (covers the whole screen)
- Downloads and decoding run on a background thread (the N3DS's extra core), so the UI never freezes
- Saves originals (including videos) to your gallery, `sdmc:/3ds/r34view/saved/<id>.<ext>`
- **Gallery** (SELECT): browse, filter, delete and sort into renamable folders what you've saved, fully offline
- Rotate images in 90° steps with L/R + D-pad
- Enter your API key by scanning a QR code with the camera, or typing it
- Debug log at `sdmc:/3ds/r34view/log.txt`

## derpiview: the Derpibooru version

The same app, built for Derpibooru instead of rule34.xxx. Everything carries over (grid, viewer, fit/fill, rotation, tag picker, gallery, folders, GIF and MP4 playback, QR key setup, debug log).

**Build:** `build-derpi.bat` (or `build-derpi.bat cia`), or `make SITE=derpibooru [cia]`. You get `derpiview.3dsx` / `derpiview.cia`.

- **Separate app:** title ID `0004000000B3E500`, its own folder `sdmc:/3ds/derpiview/` (config, gallery, folders, log), so it installs alongside r34view. Its own icon/banner/sound live in `meta-derpi/`.
- **API key is optional.** Without one, searches use Derpibooru's default filter. With your key (derpibooru.org → Account → API Key), they use your account's filters. Scan it as a QR code of just the key, type it, or put it in `config.ini`; the key screen also has **Remove key**. `filter_id=` in `config.ini` picks a specific filter.
- **Search syntax:** type Derpibooru syntax with commas (`safe, twilight sparkle, -pony`), or space-separated tags with `_` for spaces (`safe twilight_sparkle -pony`), which is what the tag picker builds. A tag that really contains an underscore needs the comma form.
- **Videos play.** Derpibooru keeps an H.264 MP4 copy of every WebM upload, so saving a video (X / disk icon) downloads the MP4 (up to 800×600) and it plays in the gallery on a New 3DS. GIFs save as GIFs and animate; SVGs save as PNG.
- The grid shows the real page count (`3/120`), since Derpibooru reports the total.

## daview: the DeviantArt version

The same app again, for DeviantArt, with your favourites one tap away. Grid, viewer, fit/fill, rotation, tag picker, gallery, folders, GIF and MP4 playback, QR setup and the log all carry over. The info screen also shows each deviation's title.

**Build:** `build-da.bat` (or `build-da.bat cia`), or `make SITE=deviantart [cia]`. You get `daview.3dsx` / `daview.cia`.

- **Separate app:** title ID `0004000000B3E600`, folder `sdmc:/3ds/daview/`, art in `meta-da/`. It installs alongside the other two.
- **Needs your own free API app.** DeviantArt doesn't give out plain API keys. On a phone or PC, log in at **deviantart.com/developers** and choose **Register Application**. Any name works, and a redirect URI like `http://localhost` is fine. Copy the app's **client_id** (a number) and **client_secret** (32 characters).
  - Then either make a QR code of `client_id=12345&client_secret=abcdef...` and scan it on the key screen, type them in (Y = secret or that whole line, X = client ID), or put `client_id=` / `client_secret=` in `config.ini`.
  - The app fetches its access token itself and renews it hourly. Tokens and secrets are masked in the log.
- **Faves:** the **\* Faves** button at the bottom left of the grid opens your favourites, all collections combined.
  - The first time, it asks for your DeviantArt username and saves it as `username=` in `config.ini`.
  - An empty search also opens your favourites when a username is set. Otherwise it shows the Daily Deviations.
- **Search syntax** (one thing per search):

  | Search | Shows |
  |---|---|
  | `fav:someone` | someone's favourites |
  | `by:someone` | someone's gallery |
  | `landscape` | the newest deviations tagged #landscape |

  - Each deviation's tags show `by:<artist>`, so **+** on that in the tag picker jumps to the artist's gallery.
  - DeviantArt's API can't combine tags. With several, the first one is used.
- **Mature content** is hidden unless you turn it on (sign-in screen, or `mature=1` in `config.ini`). With app-only access, DeviantArt sends mature images **blurred**. To get them unblurred, sign in as yourself, below.
- **Sign in to your account (optional):** this makes DeviantArt treat the app as you. You get mature images unblurred, if your account is age-verified and set to show mature content, and private collections in Faves.
  1. Once: at deviantart.com/developers, edit your app and add `https://localhost/` (https) to its **OAuth2 Redirect URI Whitelist**. To use a different address, change it with **Redirect** on the sign-in screen so the two match exactly.
  2. On the 3DS: key screen (or Options → DeviantArt key / login) → **Sign in to your account (R)**. The top screen shows a QR code of DeviantArt's sign-in page.
  3. Log in to deviantart.com in your phone's browser first, then scan the QR code and tap **Allow**. If it opens the DeviantArt app or just the home page, open the link in the browser instead; the full link is also written to `log.txt`, so you can open it on a PC. You then land on an address that won't load, like `https://localhost/?code=…`. That's expected.
  4. Within a few minutes, bring that address to the 3DS: **Scan the address QR (A)** (most phone browsers can show the current address as a QR code via Share), or **Type the code / address (Y)**.
  - The app saves a refresh token (`refresh_token=` in `config.ini`, hidden in the log) and renews it automatically. It's equivalent to a login, so don't share your config.
  - **Sign out (R)** on the same screen removes it, as does Options → Reset all API keys. If DeviantArt cancels it, the app says so and goes back to app-only access.
- **Limits:** the app signs in as your app, not as your account. Favourites and galleries must be public. Some items that need a logged-in user, like private collections, won't show. You can't fave things from the 3DS. Literature/journal entries are skipped because they have no image.
- **Saving:** saving (X / disk icon) gets the largest image the API serves. That's often a bit smaller than the original upload, because the 3DS can't use DeviantArt's logged-in download link. Film deviations save as MP4 (up to 720p) and play in the gallery.
- If DeviantArt says you're going too fast (HTTP 429), wait a minute and try again.

## Build

### Windows (MSYS2)

1. Install devkitPro with the [graphical installer](https://github.com/devkitPro/installer/releases) and tick **3DS Development**.
2. Open **devkitPro → MSYS2** from the Start menu and install curl for 3DS:
   ```sh
   pacman -S 3ds-dev 3ds-curl
   ```
3. Build, either way:
   - double-click `build.bat`, or
   - in the MSYS2 shell: `cd /c/path/to/r34view && make`

Keep the project in a path without spaces or apostrophes (e.g. `C:\dev\r34view`). If you clone it with git, `.gitattributes` keeps the Makefile's LF line endings intact.

### Linux / macOS

```sh
sudo dkp-pacman -S 3ds-dev 3ds-curl
make
```

Output is `artview.3dsx` (single-site builds: `make SITE=rule34` → `r34view.3dsx`, etc.). The Mozilla CA bundle ships in `romfs/cacert.pem`, so HTTPS is verified and the build doesn't need internet.

### .cia (installable to the HOME Menu)

1. Download two tools and put them in `tools/`:
   - **makerom** from [Project_CTR releases](https://github.com/3DSGuy/Project_CTR/releases) (Windows: `makerom.exe` from the `win_x86_64` zip)
   - **bannertool** from [3ds-bannertool releases](https://github.com/carstene1ns/3ds-bannertool/releases) (Windows: `bannertool.exe`)
2. Run `build.bat cia` (Windows) or `make cia`. For a single-site app: `build-r34.bat cia`, `build-derpi.bat cia`, `build-da.bat cia`.
3. Copy `artview.cia` (or `r34view.cia` etc.) to the SD card and install it with **FBI**.

The CIA has its own HOME Menu icon, banner and banner sound (`meta/icon.png`, `meta/banner.png` 256×128 with transparency, `meta/audio.wav` under 3 s; replace them freely; artview uses `meta-all/`). On New 3DS it runs with 124 MB of RAM, 804 MHz and the L2 cache, so large images decode more reliably than the `.3dsx` under the Homebrew Launcher. Title IDs: artview `0004000000B3E700`, r34view `0004000000B3E400`, derpiview `…B3E500`, daview `…B3E600`. Settings and galleries stay in the site folders for both the .3dsx and the .cia.

## Install & set up

1. Copy `artview.3dsx` (or a single-site `.3dsx`) to `sdmc:/3ds/`. The steps below are for rule34.xxx; see the Derpibooru and DeviantArt sections for theirs.
2. Launch it once; it creates `sdmc:/3ds/r34view/config.ini`.
3. rule34.xxx requires API credentials. On the site go to **My Account → Options → API Access Credentials** and copy the `&api_key=...&user_id=...` line. Then either:
   - **Scan it:** turn the line into a QR code with any QR code generator, and in the app choose **Scan QR code** and point the outer camera at it, or
   - **Type it:** choose **Type key / whole line** and enter the whole line (or just the key, after which it asks for your user ID), or
   - **Edit the config** on your PC, below.

   The key setup screen opens by itself on first launch when no key is set, and from the **Set API key** button whenever an online search fails (e.g. after you regenerate your key). Whatever you enter is saved into `config.ini`, keeping your other settings.

   `config.ini` looks like this:

```ini
api_key=your_key_here
user_id=123456
default_tags=
extra_tags=-ai_generated
verify_ssl=1
fill_screen=0
```

`fill_screen=1` makes the viewer start in fill mode.

You can also paste the whole `&api_key=...&user_id=...` string from the site onto one line; it's split up automatically. If a search fails, the bottom of the top screen shows how many key characters were loaded (should be 128).

`extra_tags` is appended to every search (handy as a blacklist).

## Controls

Tap **?** (bottom-right of the grid, or top-right while viewing an image) to see these on the 3DS. The **album** button next to it on the grid opens the gallery; tap it again to go back online (same as SELECT). The **menu** button (three lines) opens Options.

### Second image on the bottom screen

While viewing an image, tap the **two-screens** button in the bottom screen's header (left of **?**). Another post from the same page then fills the bottom screen, while the top screen keeps working as usual.

- **Arrows:** tap **◀ ▶** at the sides to step through the page.
- **Fit / fill:** tap the middle of the image to switch between them.
- **Close:** tap **x** (top right), or press **B**, which returns to the grid.

It starts on the post after the one on top. It shows still images; for a video post it shows the preview frame.

| Grid | |
|---|---|
| D-pad / tap | select (tap again to open) |
| A | open full image |
| L / R, or tap `<` `>` | previous / next page |
| tap the page number (top right) | type a page to jump to |
| Y, or tap the search bar | edit tags (software keyboard) |
| X | save original to the gallery (online) / delete, press twice (gallery) |
| SELECT | switch between online search and the gallery |
| START | quit |

| Viewer | |
|---|---|
| Y | fit whole image ↔ fill the screen |
| Circle Pad / C-stick | pan |
| D-pad Up/Down, ZR/ZL | zoom |
| A | reset zoom and position (videos: pause / play) |
| L/R (tap), D-pad Left/Right | previous / next post (crosses pages) |
| R + D-pad Right | rotate 90° right |
| L + D-pad Left | rotate 90° left |
| X | save original (online) / delete, press twice (gallery) |
| SELECT | switch online / gallery |
| B | back to grid |

Rotation stays as you move between posts (handy when holding the system sideways for tall images) and works in fit and fill mode. Holding L or R and pressing the D-pad rotates without changing post; L or R on its own changes post when released.

### Building a search from tags

While viewing a post, the bottom screen lists its tags:

- **+** includes a tag, **−** excludes it (tapping the tag name is the same as +). Tap the lit button again to remove it; tap the other one to flip it.
- Picks carry over as you move between posts (L/R), so you can combine tags from several posts. Picked tags are highlighted green (include) or orange (exclude).
- Drag the list to scroll.
- The line above the buttons shows the search being built; tap it to edit it with the keyboard first.
- **Search** runs it (online search, or the gallery filter); **Clear** empties it. Up to 32 tags.
- The **disk icon** next to Clear saves the post to your gallery (same as X). It's dimmed in the gallery, where posts are already saved.

## Gallery (offline)

Press **SELECT** to switch to your gallery of saved images; the search bar turns blue and reads `GALLERY:`. It works with Wi-Fi off, and the app opens straight into it if networking can't start.

- Newest posts (highest ID) first, 12 per page, with page count.
- **Shuffle:** the crossing-arrows button in the bottom row, next to Folders, shows the gallery in a random order. It lights up while on. Paging through a shuffle keeps the same order, and it works with filters and folders. Tap it again for the normal order; tap twice for a fresh shuffle. It works in every site's gallery.
- **Y** filters by tags, e.g. `cat -red_hair`, or a post ID. Tags come from the `<id>.txt` info file written when you save, so files saved by version 1.0 or copied in by hand only match by ID.
- **X** twice deletes the image, its info file and its thumbnail.
- **GIFs play animated** in the gallery viewer (online GIFs show their first frame). The first loop plays while decoding; GIFs that fit in 24 MB (about 100 screen-sized frames) then loop smoothly from memory, longer ones keep decoding as they play.
- **MP4 videos play** in the gallery viewer on a **New 3DS**, with sound (see below). WebM and online videos show their thumbnail.

### Folders

Organise the gallery into folders (an image can be in several):

- **Add an image:** open it from the gallery and tap the **folder button** next to Clear. Tick the folders it belongs in, or tap **+ New** to make a folder and add the image in one go.
- **Browse / manage:** tap **Folders** at the bottom-left of the gallery grid. Tap a folder to show only its images (the top bar shows the folder name; Y still filters by tag inside it), or **All images** to go back. Each folder has **Rename** and **Delete** (tap Delete twice).
- Folders only group images: deleting a folder keeps its images, and deleting an image removes it from every folder.
- Stored in `sdmc:/3ds/r34view/folders.txt` (`[Folder name]` followed by post IDs), which you can also edit on a PC.

## Video playback (gallery MP4s)

Save a video post with X, then open it from the gallery (SELECT).

- **Starting:** videos don't start by themselves. Opening one asks **"Play this video?"**: press **A** or tap **Play** to start it. **B** / **Not now** leaves its preview up, and **A** plays it later.
- **While playing:** **A** pauses / plays; tap the **time bar** at the top of the bottom screen to jump. Videos loop.
- Fit/fill (Y), zoom, panning and rotation work the same as on images.

What works and what doesn't:

| | |
|---|---|
| H.264 video (almost all `.mp4`) | decoded by the New 3DS's hardware video decoder |
| AAC sound | decoded by the bundled Helix AAC decoder and played through the DSP |
| Old 3DS / 2DS | no hardware decoder, so MP4s say they need a New 3DS |
| WebM (VP8/VP9) | can't play: the 3DS can't decode these fast enough in software |
| Bigger than 720p (e.g. 1080p) | refused with a message: larger videos can lock up the hardware decoder |
| HEVC/H.265 MP4s, fragmented MP4s | not supported; the viewer says why |
| Sound needs `sdmc:/3ds/dspfirm.cdc` | the usual DSP firmware dump (DSP1); without it videos play silently |

The hardware decoder is documented to handle H.264 up to level 3.2 (720p class), so videos larger than 1280×720 are refused rather than risk locking it up. If the decoder ever stops responding anyway, leaving the video gives up on it after 3 seconds instead of freezing the app. Videos are then off until you restart the app. The log (`log.txt`) records each video's size, profile and level, what the decoder said, and how long decoding took. Send it over if a video misbehaves.

Folder layout:

```
sdmc:/3ds/r34view/saved/
  12345678.jpg          original
  12345678.txt          tags, score, rating, size
  .thumbs/12345678.jpg  grid thumbnail
```

Downloads go to `<name>.part` first and are renamed when complete, so an interrupted save never shows up as a broken image.

## Debug log

Every run writes `sdmc:/3ds/r34view/log.txt`; the previous run is kept as `log.old.txt`. It's flushed after every line, so it survives crashes. It records:

- version, Old/New 3DS, kernel version, free memory
- what was loaded from the config (key **length** only, never the key)
- every request with HTTP status, size, content type and time, and the start of the response body when it fails
- API errors, image decode failures, texture failures, saves and deletes

The API key is masked as `api_key=<128 chars>` everywhere in the log, so you can share it when asking for help.

For connection or certificate problems set `verbose_log=1` in `config.ini` to also log curl's full connection, TLS and header trace.

## Notes

- MP4 audio uses the [Helix AAC decoder](https://github.com/pschatzmann/arduino-libhelix) by RealNetworks (RPSL/RCSL, see `source/helix-aac/LICENSE.txt`).
- The DeviantArt sign-in QR code is made with [QR Code generator by Project Nayuki](https://github.com/nayuki/QR-Code-generator) (MIT licence, in `source/qrgen/`).
- QR scanning uses [quirc](https://github.com/dlbeer/quirc) v1.1 (ISC licence, `source/quirc/LICENSE`). Later quirc versions fail to recognise tilted codes, so v1.1 is used on purpose.

- The viewer loads the site's `sample` image (~850 px wide), at up to 1024 px wide and 3072 px tall, so zooming in keeps detail. Tall images are split into 1024-row GPU textures. In fill mode, tall images open at the top; scroll down with the Circle Pad or C-stick. Videos show their preview frame; use X to save the actual video.
- GIFs show the first frame.
- Images over 12 MP are skipped when decoding to stay inside 3DS memory.
