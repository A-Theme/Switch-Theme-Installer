# A-Theme Installer (Switch Homebrew)

A native `.nro` homebrew app for Nintendo Switch that browses and installs
Tinfoil themes from [A-Theme/Tinfoil-Themes](https://github.com/A-Theme/Tinfoil-Themes)
directly onto your SD card — no PC required once a theme is published.

This is **not** the visual theme editor — that's intentionally a browser/
desktop/mobile tool, since building and tweaking a theme really wants a
mouse, keyboard, and a real display. Find it at
[A-Theme/Theme-App](https://github.com/A-Theme/Theme-App). This app covers
the other half of the workflow: once a theme exists, install it on-console
with a controller, no computer needed.

## ⚠️ Status: unverified first draft

This source was written against the documented libnx/curl-for-Switch APIs,
but **has not been compiled or run on real hardware or a real console**.
The environment it was written in has no access to devkitPro's toolchain
servers, so there was no way to build or test it there. See
[`BUILD.md`](BUILD.md) for the full build steps and a troubleshooting
section flagging the specific spots most likely to need small fixes on a
first real compile.

If you build this and get it working (or need to fix something), pull
requests very welcome.

## What it does

1. On launch, downloads a plain-text list of available themes
   (`themes.txt`) from the repo
2. Shows them as a scrollable on-screen menu (D-Pad/stick to move, A to
   install, + to exit)
3. Downloads the selected theme's `theme.json` (plus any local logo/audio
   assets it needs) straight into `sdmc:/switch/tinfoil/themes/<name>/`
4. From there, just pick it inside Tinfoil's own theme settings like any
   manually-installed theme

## Building it yourself

See [`BUILD.md`](BUILD.md) — you'll need devkitPro/devkitA64, the
`switch-curl`/`switch-mbedtls`/`switch-zlib` portlibs, and a CA certificate
bundle for HTTPS.

## Files in this folder

```
source/main.c          — the whole app
Makefile                — devkitA64/libnx build config
BUILD.md                — full setup + build instructions
manifest-example/themes.txt — example of the manifest format this expects
```

## Adding your own theme list

Add a `themes.txt` to the root of your themes repo, one line per theme:

```
Display Name|folder_name|optional,extra,assets
```

See `manifest-example/themes.txt` for a working example and format notes.
No app rebuild needed to add themes later — it fetches the list fresh
every launch.

## License

MIT — see [LICENSE](LICENSE).
