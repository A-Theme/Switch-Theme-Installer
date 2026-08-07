# A-Theme Installer (Switch Homebrew)

A native `.nro` homebrew app for Nintendo Switch that reads
[A-Theme/Tinfoil-Themes](https://github.com/A-Theme/Tinfoil-Themes)' own
existing `themes.json` manifest, lets you browse the full list with a
controller, **preview a theme's actual colors and layout before committing
to it**, and installs whichever one you pick directly onto your SD card —
no PC required once a theme is published.

This is **not** the visual theme editor — that's intentionally a browser/
desktop/mobile tool, since building and tweaking a theme really wants a
mouse, keyboard, and a real display. Find it at
[A-Theme/Theme-App](https://github.com/A-Theme/Theme-App). This app covers
the other half: once a theme exists, browse, preview, and install it
on-console with a controller.

## Status

This project has been through real device testing, and each round of
feedback fixed something real:

1. First build — compiled clean on a real Windows devkitPro install.
2. Real theme install failed — the app was looking for `theme.json`
   inside the zip, but A-Theme's actual zips package it as
   `settings.json`. Fixed: the app now recognizes either name, and always
   writes it to the SD card as `theme.json` (what Tinfoil itself expects).
3. Preview only showed a raw background image, no sense of the theme's
   actual look, and **crashed** leaving the preview screen. Root cause:
   toggling between libnx's text console and SDL2's video system for just
   the preview screen. Fixed at the root — this version uses SDL2 for the
   *entire* app, menu included, so there's no handoff left to crash on.

All the JSON/color-parsing logic has been compiled and run for real on a
PC against actual A-Theme theme content — see [`BUILD.md`](BUILD.md) for
exactly what that covered. The current SDL2-only architecture is new and
hasn't had its own hardware test yet.

## What it does

1. Downloads `themes.json` on launch — your existing manifest, nothing
   new to maintain
2. Shows every theme as a scrollable menu, tagging community submissions
   (`Non_Aramaki_Themes/`)
3. **A** installs the highlighted theme immediately
4. **Y** installs it, then shows a real preview: background image (or
   background color), logo, an icon grid with one tile in the theme's
   actual selection colors, a border frame, and a progress bar — all
   drawn from the theme's real parsed values. **A** keeps it, **B**
   removes it again.
5. Installed themes land in `sdmc:/switch/tinfoil/themes/<name>/`, ready
   to pick in Tinfoil's own theme settings

## Building it yourself

See [`BUILD.md`](BUILD.md) — devkitPro/devkitA64, the `switch-curl` /
`switch-mbedtls` / `switch-zlib` / `switch-zziplib` / `switch-sdl2` /
`switch-sdl2_image` / `switch-sdl2_ttf` portlibs, and a CA certificate
bundle for HTTPS.

## Files in this folder

```
source/main.c     — the whole app
include/jsmn.h    — bundled JSON parser (MIT, github.com/zserge/jsmn)
Makefile          — devkitA64/libnx build config
BUILD.md          — full setup + build + troubleshooting instructions
```

No manifest file needs to be created or maintained — it reads your repo's
existing `themes.json` directly.

## License

MIT — see [LICENSE](LICENSE).
