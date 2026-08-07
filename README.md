# A-Theme Installer (Switch Homebrew)

A native `.nro` homebrew app for Nintendo Switch that reads
[A-Theme/Tinfoil-Themes](https://github.com/A-Theme/Tinfoil-Themes)' own
existing `themes.json` manifest, lets you browse the full list with a
controller, **preview a theme's look before committing to it**, and
installs whichever one you pick directly onto your SD card — no PC
required once a theme is published.

This is **not** the visual theme editor — that's intentionally a browser/
desktop/mobile tool, since building and tweaking a theme really wants a
mouse, keyboard, and a real display. Find it at
[A-Theme/Theme-App](https://github.com/A-Theme/Theme-App). This app covers
the other half of the workflow: once a theme exists, browse, preview, and
install it on-console with a controller, no computer needed.

## Status

This project has a real build track record: an earlier version was
successfully compiled on a real Windows devkitPro install with no errors,
and the JSON-parsing logic in every version since (manifest parsing, and
now theme.json field lookups for the preview) has been compiled and run
for real on a PC against actual A-Theme content before being written here.

This version adds **preview-before-install**, which needed real graphics
for the first time (`switch-sdl2` + `switch-sdl2_image`) — that specific
piece is new, Switch-specific, and could not be tested in the environment
this was written in. [`BUILD.md`](BUILD.md) has a troubleshooting section
aimed squarely at that one risk area.

If you build this and hit an issue, pull requests (or just pasting the
build error back for help) are very welcome.

## What it does

1. Downloads `themes.json` on launch — the same manifest already used
   elsewhere in the project, nothing new to maintain
2. Shows every theme as a scrollable menu, tagging community submissions
   (`Non_Aramaki_Themes/`) so they're easy to tell apart
3. **A** installs the highlighted theme immediately
4. **Y** installs it, then shows its background image (or logo) fullscreen
   — **A** keeps it, **B** removes it again. (There's currently no
   separate lightweight preview asset in the manifest — only full theme
   `.zip`s — so previewing does briefly install for real; declining just
   deletes what was written.)
5. Either way, installed themes land in
   `sdmc:/switch/tinfoil/themes/<name>/`, ready to pick in Tinfoil's own
   theme settings

## Building it yourself

See [`BUILD.md`](BUILD.md) — you'll need devkitPro/devkitA64, the
`switch-curl` / `switch-mbedtls` / `switch-zlib` / `switch-zziplib` /
`switch-sdl2` / `switch-sdl2_image` portlibs, and a CA certificate bundle
for HTTPS.

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
