# A-Theme Installer (Switch Homebrew)

A native `.nro` homebrew app for Nintendo Switch that reads
[A-Theme/Tinfoil-Themes](https://github.com/A-Theme/Tinfoil-Themes)' own
existing `themes.json` manifest, lets you browse the full list with a
controller, **preview a theme's actual colors and layout**, **regenerate
its palette straight from its own background image**, and installs
whichever one you end up keeping directly onto your SD card — no PC
required once a theme is published.

This is **not** the full visual theme editor — that's intentionally a
browser/desktop/mobile tool (find it at
[A-Theme/Theme-App](https://github.com/A-Theme/Theme-App)), and there's a
real reason it stays that way: see **"Why this isn't a full editor"**
below. This app covers the on-console half of the workflow: browse,
preview, tweak the palette, and install, all with a controller.

## Status

This project has been through several rounds of real device testing, and
each round fixed something real:

1. First build — compiled clean on a real Windows devkitPro install.
2. Real install failed — the app looked for `theme.json` inside the zip,
   but A-Theme's zips package it as `settings.json`. Fixed: either name is
   now recognized and normalized to `theme.json` on the SD card.
3. Preview only showed a raw background image and **crashed** leaving the
   screen. Root cause: toggling between libnx's text console and SDL2 for
   just the preview. Fixed at the root — SDL2 now owns the display for the
   app's entire lifetime, menu included, so there's no handoff left to
   crash on.

All the JSON/color-parsing logic (manifest parsing, nested theme.json
field lookups, hex color decoding) and the new palette-generation logic
(k-means color clustering, in-place JSON color editing) have been compiled
and run for real on a PC against actual A-Theme content and synthetic test
data before being written in — see [`BUILD.md`](BUILD.md) for exactly
what that covered. The SDL2-only architecture and the palette feature's
Switch-specific glue (reading pixels from a real decoded `SDL_Surface`)
haven't had their own hardware test yet.

## What it does

1. Downloads `themes.json` on launch — your existing manifest, nothing new
   to maintain
2. Shows every theme as a scrollable menu, tagging community submissions
3. **A** installs the highlighted theme immediately
4. **Y** installs it, then shows a preview built from its real colors and
   assets — background, logo, an icon grid with a "selected" tile in the
   theme's actual selection colors, a border frame, a progress bar
5. **From the preview screen, X regenerates the theme's palette straight
   from its own background image** — real k-means color clustering finds
   the background's dominant colors and applies them to the theme's
   background/text/accent/border/progress-bar fields, written straight
   back into `theme.json` on the SD card. Press X again for a different
   result; **A** keeps whatever's currently applied, **B** discards the
   whole install.
6. Installed themes land in `sdmc:/switch/tinfoil/themes/<name>/`, ready
   to pick in Tinfoil's own theme settings

## Why this isn't a full editor

Porting the browser editor's complete capability — a color picker *and*
independent alpha slider for every field, live-updating preview on every
keystroke, free-text editing for URLs/paths — to a controller is a
different scale of project, not an incremental add-on:

- Most fields (colors, numbers) don't need a keyboard and could work with
  D-pad adjustment. But navigating ~20 nested fields with a controller
  still needs real UI design, not just "point at the JSON."
- The few text/URL fields (background image, logo, music) would need
  Switch's on-screen software keyboard (`swkbd`), a whole separate API
  surface this app doesn't currently touch at all.
- A live-updating preview on every single edit (rather than the current
  "apply a whole palette, see the result" model) means re-parsing and
  redrawing on every input tick, not just on a button press.

None of that is impossible — it's just genuinely a second app's worth of
work, and given how much of this project's value has come from testing
things for real rather than assuming they work, it felt more honest to
scope this round around what could actually be built and reasoned about
solidly (the palette feature) rather than stack a much larger UI project
on top of code that's still mid-way through its own hardware testing.

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
