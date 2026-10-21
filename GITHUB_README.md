# artview

**An art and image browser for the New Nintendo 3DS.** Five sites in one homebrew app: rule34.xxx, Derpibooru, DeviantArt, e621 and pixiv. It also runs on the old 3DS/2DS, just slower.

> **18+ only.** rule34.xxx and e621 are adult sites. Derpibooru, DeviantArt and pixiv can return mature content depending on your search and account settings. Use it responsibly.

Search on the bottom screen, view full images on the top screen. Save what you like into an offline gallery with folders, tag filtering and shuffle. Animated GIFs and MP4 videos (with sound) play right on the 3DS.

## Features

- **Five sites, one app.** Tap the site tab on the grid to switch. Each site keeps its own login, settings, search, page, gallery and folders.
- **Touch-friendly browsing.** A 4×3 thumbnail grid on the bottom screen, a tag search bar, and a full-size viewer on the top screen with zoom, pan, fit/fill and 90° rotation.
- **Build searches from tags.** Tap **+** or **−** next to any tag while viewing a post, and the picks carry over between posts.
- **Sort options** for each site's API: newest, top score, most faves, hot, random and more.
- **Offline gallery.** Save originals (including videos) to the SD card, then browse with no Wi-Fi. Filter by tag, shuffle, delete, and sort images into renamable folders.
- **GIFs and MP4s play.** MP4 uses the New 3DS hardware H.264 decoder and a bundled AAC decoder for sound. Derpibooru, e621 and DeviantArt videos are saved as playable MP4s.
- **Second image on the bottom screen** while you view one on top.
- **QR-code key setup.** Scan API keys with the 3DS camera instead of typing them on a software keyboard.
- **Themes.** Slate, OLED black, Midnight blue, Forest, Plum, Light, or your own three hex colours.
- **Favourite tags** (saved searches) for each site.
- **Fast and stable.** Downloads and image decoding run on the New 3DS's extra core so the UI doesn't freeze. Downloads use `.part` files so an interrupted save never leaves a broken image.
- **Debug log** at `sdmc:/3ds/artview/log.txt`. It's flushed on every line, and API keys are masked, so it's safe to share when reporting a bug.

| Site | Login |
|---|---|
| rule34.xxx | API key + user ID (required) |
| Derpibooru | API key (optional) |
| DeviantArt | Your own free API app: client ID + secret (required) |
| e621 | Username + API key (optional; guests can't see some posts) |
| pixiv | `PHPSESSID` cookie (optional; needed for R-18 works) |

> pixiv has no public API, so artview uses the same JSON requests the pixiv website makes. That's unofficial and could break if pixiv changes it.

## Requirements

- A New Nintendo 3DS / New 2DS XL (recommended), or an old 3DS/2DS (slower; MP4 video needs a New 3DS)
- Custom firmware, to run homebrew
- Wi-Fi to browse online. The gallery works offline.
- For video sound: `sdmc:/3ds/dspfirm.cdc` (the usual DSP firmware dump). Without it, videos play silently.

## Install

**As a `.3dsx` (Homebrew Launcher)**

1. Copy `artview.3dsx` to `sdmc:/3ds/` on your SD card.
2. Launch it from the Homebrew Launcher.

**As a `.cia` (HOME Menu icon)**

1. Copy `artview.cia` to your SD card.
2. Install it with FBI.

The `.cia` runs with 124 MB of RAM, 804 MHz and the L2 cache on New 3DS, so large images decode more reliably than the `.3dsx` under the Homebrew Launcher.

Settings and galleries live in `sdmc:/3ds/<site>/` (`r34view`, `derpiview`, `daview`, `e6view`, `pxview`), and shared settings and the log in `sdmc:/3ds/artview/`. They're the same for the `.3dsx` and the `.cia`.

## Set up your keys

Open the site's key screen from **Options → key / login**. It also opens by itself the first time you use a site that needs a key. You can scan a QR code, type it, or edit `config.ini` on your PC.

**rule34.xxx:** On the site, go to *My Account → Options → API Access Credentials* and copy the `&api_key=...&user_id=...` line. Make a QR code of it with any QR generator, choose **Scan QR code**, and point the outer camera at it. Or choose **Type key / whole line**.

**Derpibooru:** *Account → API Key*. QR code of just the key.

**DeviantArt:** At deviantart.com/developers, choose *Register Application* (any name; `http://localhost` works as the redirect URI). Make a QR code of `client_id=12345&client_secret=abcdef...`. Optional: sign in to your own account from the key screen to see mature images unblurred and private collections.

**e621:** *Account → Manage API Access*, then a QR code of `login=YOURNAME&api_key=YOURKEY`.

**pixiv (optional):** Copy the `PHPSESSID` cookie from a logged-in pixiv.net browser session and scan or type it. It works like a password, so keep it private.

Example `config.ini` for rule34.xxx:

```ini
api_key=your_key_here
user_id=123456
default_tags=
extra_tags=-ai_generated
verify_ssl=1
fill_screen=0
```

`extra_tags` is added to every search, which makes it handy as a blacklist.

## Basic controls

| Grid | |
|---|---|
| D-pad / tap | Select (tap again to open) |
| A | Open full image |
| L / R | Previous / next page |
| Y | Edit tags |
| X | Save to gallery (online) / delete (gallery) |
| SELECT | Switch between online and gallery |
| START | Quit |

| Viewer | |
|---|---|
| Y | Fit ↔ fill |
| Circle Pad / C-stick | Pan |
| D-pad Up/Down, ZL/ZR | Zoom |
| L/R, D-pad Left/Right | Previous / next post |
| L/R + D-pad Left/Right | Rotate 90° |
| A | Reset zoom (videos: pause / play) |
| B | Back to grid |

Tap **?** in the app for the full list.

## Building from source

You need [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the 3DS toolchain.

```sh
# Windows: devkitPro MSYS2 shell. Linux/macOS: dkp-pacman
pacman -S 3ds-dev 3ds-curl

make                      # artview.3dsx (all five sites)
make cia                  # artview.cia (see below)
make SITE=rule34          # single-site build: r34view
make SITE=derpibooru      # derpiview
make SITE=deviantart      # daview
make SITE=e621            # e6view
make SITE=pixiv           # pxview
```

On Windows you can also double-click `build.bat` (or `build-r34.bat`, `build-derpi.bat`, `build-da.bat`, `build-e6.bat`, `build-px.bat`).

For `.cia` builds, put [makerom](https://github.com/3DSGuy/Project_CTR/releases) and [bannertool](https://github.com/carstene1ns/3ds-bannertool/releases) in `tools/`.

Keep the project path free of spaces and apostrophes. The Mozilla CA bundle ships in `romfs/cacert.pem`, so HTTPS is verified and the build doesn't need internet.

Title IDs: artview `0004000000B3E700`, r34view `…B3E400`, derpiview `…B3E500`, daview `…B3E600`, e6view `…B3E800`, pxview `…B3E900`.

## Credits

- [Helix AAC decoder](https://github.com/pschatzmann/arduino-libhelix) by RealNetworks (RPSL/RCSL, see `source/helix-aac/LICENSE.txt`)
- [quirc](https://github.com/dlbeer/quirc) v1.1 for QR scanning (ISC)
- [QR Code generator](https://github.com/nayuki/QR-Code-generator) by Project Nayuki (MIT)
- [stb_image](https://github.com/nothings/stb) by Sean Barrett (public domain / MIT)
- Built with [devkitPro](https://devkitpro.org), libctru, citro2d/citro3d and libcurl

## Disclaimer

artview is an unofficial, fan-made client. It isn't affiliated with or endorsed by Nintendo, rule34.xxx, Derpibooru, DeviantArt, e621 or pixiv. You're responsible for following each site's terms of service and your local laws, and for keeping your API keys and cookies private.
