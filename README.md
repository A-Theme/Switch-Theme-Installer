<div align="center">

<img src="https://capsule-render.vercel.app/api?type=waving&color=0:ff3c50,50:9d4edd,100:00c2ff&height=200&section=header&text=A-THEME%20INSTALLER&fontSize=60&fontColor=ffffff&animation=fadeIn&fontAlignY=38&desc=native%20Switch%20homebrew%20%C2%B7%20browse%20%C2%B7%20preview%20%C2%B7%20install&descAlignY=58&descSize=18" width="100%"/>

<img src="https://raw.githubusercontent.com/A-Theme/Theme-App/main/assets/logo.png" width="90" height="90" alt="A-Theme logo"/>

<p>
  <img src="https://img.shields.io/badge/platform-Nintendo%20Switch-e60012?style=for-the-badge&logo=nintendoswitch&logoColor=white" alt="platform"/>
  <img src="https://img.shields.io/badge/toolchain-devkitA64%20%2F%20libnx-00c2ff?style=for-the-badge" alt="toolchain"/>
  <img src="https://img.shields.io/badge/license-MIT-9d4edd?style=for-the-badge" alt="license"/>
</p>

</div>

---

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

<div align="center">
<img src="https://github-readme-stats.vercel.app/api/pin/?username=A-Theme&repo=Switch-Theme-Installer&theme=radical&hide_border=true&bg_color=0b1420" alt="repo card"/>
</div>

---

## 🕹️ Status

This project has been through several rounds of real device testing, and
each round fixed something real:

1. First build — compiled clean on a real Windows devkitPro install.
2. Real install failed — the app looked for `theme.json` inside the zip,
   but A-Theme's zips package it as `settings.json`. Fixed: either name is
   now recognized inside a zip.
3. Preview only showed a raw background image and **crashed** leaving the
   screen. Root cause: toggling between libnx's text console and SDL2 for
   just the preview. Fixed at the root — SDL2 now owns the display for the
   app's entire lifetime, menu included, so there's no handoff left to
   crash on.
4. The palette generator was capped at a fixed 5 colors regardless of a
   theme's actual palette size. Fixed to match each theme's own distinct
   color count — and along the way, verification caught a real
   uninitialized-memory risk in the role-mapping logic before it ever
   reached hardware. See the [release notes](../../releases) for the full
   writeup.
5. Themes installed correctly but **Tinfoil still showed the default
   theme** — entry 2 above had the naming backwards. A real SD card
   settled it: 11 of 12 working theme folders use `settings.json`, and
   the one `theme.json` folder was created by this installer (and didn't
   load). Tinfoil reads `settings.json`, so that's what gets written now.

All the JSON/color-parsing logic (manifest parsing, nested theme.json
field lookups, hex color decoding) and the palette-generation logic
(k-means color clustering, in-place JSON color editing, dynamic color
counting) have been compiled and run for real on a PC against actual
A-Theme content and synthetic test data before being written in — see
[`BUILD.md`](BUILD.md) for exactly what that covered. The SDL2-only
architecture and the palette feature's Switch-specific glue (reading
pixels from a real decoded `SDL_Surface`) are the pieces that can only be
confirmed on real hardware.

---

## 🎨 What it does

<table>
<tr><td width="60"><b>—</b></td><td>Downloads <code>themes.json</code> on launch — your existing manifest, nothing new to maintain</td></tr>
<tr><td><b>—</b></td><td>Shows every theme as a scrollable menu, tagging community submissions</td></tr>
<tr><td><b>A</b></td><td>Installs the highlighted theme immediately</td></tr>
<tr><td><b>Y</b></td><td>Installs it, then shows a preview built from its real colors and assets — background, logo, an icon grid with a "selected" tile in the theme's actual selection colors, a border frame, a progress bar</td></tr>
<tr><td><b>X</b><br><i>(on preview)</i></td><td>Regenerates the theme's palette straight from its own background image — real k-means color clustering finds the dominant colors and writes them directly into <code>theme.json</code> on the SD card, sized to match the theme's own color count. Press again for a different result.</td></tr>
<tr><td><b>A</b> / <b>B</b><br><i>(on preview)</i></td><td>Keep whatever's currently applied, or discard the whole install</td></tr>
</table>

Installed themes land in `sdmc:/switch/tinfoil/themes/<name>/`, and after
each successful install the app **asks whether to make it your active
theme** — say yes and it updates Tinfoil's own settings so the theme is
already applied next time you open Tinfoil, no menu-diving required.

> Because Tinfoil's `options.json` also holds credentials, this is always
> a prompt, never automatic, and a backup is written to
> `sdmc:/switch/a-theme-installer/options.json.bak` before anything is
> changed. Close Tinfoil before setting an active theme — if it's running,
> it may overwrite the change when it exits.

---

## 🧭 Why this isn't a full editor

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

---

## 🛠️ Building it yourself

See [`BUILD.md`](BUILD.md) — devkitPro/devkitA64, the `switch-curl` /
`switch-mbedtls` / `switch-zlib` / `switch-zziplib` / `switch-sdl2` /
`switch-sdl2_image` / `switch-sdl2_ttf` portlibs, and a CA certificate
bundle for HTTPS.

## 📁 Files in this folder

```
source/main.c     — the whole app
include/jsmn.h    — bundled JSON parser (MIT, github.com/zserge/jsmn)
Makefile          — devkitA64/libnx build config
BUILD.md          — full setup + build + troubleshooting instructions
```

No manifest file needs to be created or maintained — it reads your repo's
existing `themes.json` directly.

---

## 🔗 Part of the A-Theme project

<div align="center">

[![Theme-App](https://img.shields.io/badge/Theme--App-visual%20editor-00c2ff?style=for-the-badge)](https://github.com/A-Theme/Theme-App)
[![Tinfoil-Themes](https://img.shields.io/badge/Tinfoil--Themes-theme%20database-ff3c50?style=for-the-badge)](https://github.com/A-Theme/Tinfoil-Themes)
[![A-Theme](https://img.shields.io/badge/A--Theme-org-9d4edd?style=for-the-badge)](https://github.com/A-Theme)

</div>

## 📄 License

MIT — see [LICENSE](LICENSE).

<div align="center">
<img src="https://capsule-render.vercel.app/api?type=waving&color=0:00c2ff,50:9d4edd,100:ff3c50&height=100&section=footer" width="100%"/>
</div>
