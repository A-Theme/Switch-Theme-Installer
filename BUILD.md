# Building A-Theme Installer

> **What changed, and why:** an earlier version of this app used libnx's
> text console for the menu, and only brought up SDL2 for the preview
> screen — toggling between them with `consoleExit()`/`consoleInit()`.
> That was flagged as the single riskiest part of the app before it was
> even tested, and sure enough: it caused a real crash on real hardware
> right after leaving the preview screen. This version removes that risk
> at the root rather than patching around it — **SDL2 now owns the display
> for the app's entire lifetime**, menu included. There's no more console
> at all, so there's nothing left to hand off between.
>
> This version also draws an actual mockup of the Tinfoil layout during
> preview (icon grid, a "selected" tile using the theme's real selection
> colors, a border frame, a progress bar) using the theme's real parsed
> colors — not just the raw background image on its own.

## Status

- The JSON/color-parsing logic (manifest parsing, theme.json field
  lookups for background/selection/border/progressBar/logo, hex color
  decoding) has been compiled and run for real on a PC against actual,
  complete A-Theme `settings.json` content, including nested fields
  sitting next to sibling objects that could easily have caused
  index-tracking bugs. All of it came back correct — see the file header
  in `main.c` for exactly what was tested this way.
- The zip extraction (`switch-zziplib`) and previous console-handoff bug
  were both found via real device testing on this project already.
- **This SDL2-only rewrite itself has not yet been tested on hardware.**
  It's built specifically to eliminate the exact failure mode that was
  just found, and follows devkitPro's own official example patterns
  wherever there was a choice to make (see the Makefile's pkg-config
  comment), but "should be more robust" isn't the same as "confirmed
  working" — that needs a real build-and-run pass.

## 1. Install devkitPro

<https://devkitpro.org/wiki/Getting_Started> — verify with:

```bash
echo $DEVKITPRO
# should print something like /opt/devkitpro
```

## 2. Install the required packages

```bash
# Linux/macOS
sudo dkp-pacman -S switch-dev switch-curl switch-mbedtls switch-zlib switch-zziplib switch-sdl2 switch-sdl2_image switch-sdl2_ttf

# Windows (MSYS2 shell from the devkitPro installer)
pacman -S switch-dev switch-curl switch-mbedtls switch-zlib switch-zziplib switch-sdl2 switch-sdl2_image switch-sdl2_ttf
```

New in this version: **`switch-sdl2_ttf`** — text rendering, using
Nintendo's own shared system font (no font file needs bundling).

## 3. Add the CA certificate bundle

```bash
mkdir -p romfs
curl -o romfs/cacert.pem https://curl.se/ca/cacert.pem
```

## 4. Build

```bash
make
```

## 5. Install on your Switch

Copy `a-theme-installer.nro` to `sdmc:/switch/a-theme-installer/` and
launch it from the Homebrew Menu.

## How it works

- **A** installs the highlighted theme immediately.
- **Y** installs it, then shows a preview built from the theme's actual
  parsed colors and assets: background image (or background color if
  there's no image), logo top-left, an icon grid mockup with one tile
  shown in the theme's real selection colors, a border frame, and a
  progress bar. **A** keeps the theme, **B** deletes what was just
  written and returns to the menu.
- This mockup isn't pixel-identical to Tinfoil's actual layout (this app
  doesn't have Tinfoil's own layout code to reference) — it's built to
  give a genuine sense of the palette and assets working together, using
  the same real color values Tinfoil itself would use.

## Troubleshooting notes for whoever compiles this first

Roughly in likelihood order:

- **`plGetSharedFontByType` / `PlFontData` / `PlSharedFontType_Standard`
  not found, or fails at runtime** — this is genuinely new API surface
  for this project (accessing Nintendo's shared system font). If it
  doesn't compile, check `switch/include/switch/services/pl.h` in your
  installed libnx for the current names — this API has existed a long
  time but naming has drifted before. If it compiles but `plGetSharedFontByType`
  fails at runtime (check via the returned `Result` — `init_graphics()`
  reports this on-screen... except it can't, since graphics failed to
  init in the first place; in that case the app will just exit
  immediately with no visible message, so if that happens, this is the
  first thing to check by adding a temporary print/log). A bundled `.ttf`
  in `romfs` is the fallback if the shared font approach doesn't pan out
  — happy to add that if needed.
- **Blank/black screen, nothing renders at all** — check
  `SDL_CreateWindowAndRenderer`'s return value is actually 0 (it's
  checked in `init_graphics()`, but if this specific call is the failure
  point on your setup, the app exits silently rather than showing an
  error, for the same reason as above).
- **Colors look wrong (too opaque, or transparency not working)** —
  `SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND)` needs to
  actually take effect for the alpha channel in theme colors (many real
  theme colors are semi-transparent, e.g. `1178997A`) to blend rather
  than just multiply against black. If translucent UI elements look
  fully opaque, this is the first thing to check.
- **`ZZIP_DIR` / `zzip_dir_open` not found** — confirm `switch-zziplib`
  installed, check `$DEVKITPRO/portlibs/switch/include/zzip/` for the
  actual header name (`<zzip/zzip.h>` vs `<zzip/lib.h>` across versions).
- **Cert verification fails at runtime** — `CURLOPT_CAINFO` is set to
  `"romfs:/cacert.pem"`, assuming libcurl's Switch port resolves `romfs:`
  paths the same way `sdmc:` is resolved. `CURLOPT_CAINFO_BLOB` with the
  cert loaded into memory is the fallback if not.
- **`HidNpadButton_*` names don't resolve** — check
  `switch/include/switch/services/hid.h` in your installed libnx.
- **Preview image is stretched/squished** — background is intentionally
  stretched to fill the screen (matches how a Tinfoil background image
  actually gets used); the logo keeps its aspect ratio. If something
  looks proportionally wrong beyond that, check `surface->w`/`surface->h`
  right after decode.

## Updating later

Nothing needs rebuilding when themes are added/removed/changed — the
manifest and each theme's `theme.json` are fetched fresh every launch.
