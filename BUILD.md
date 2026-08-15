# Building A-Theme Installer

## Status, honestly

This app has been through several real build-and-test cycles already, and
each one found something real to fix (see the README's "Status" section
for the full history). What's true right now:

- **Verified for real, compiled and run on a PC:** all manifest parsing,
  all nested theme-config field lookups, hex color decoding, k-means color
  clustering (tested against a synthetic image with 4 known dominant
  colors in known proportions — recovered all 4 exactly), and in-place
  JSON color editing (tested against real settings.json content — edited
  fields came back correct, untouched fields stayed untouched, and the
  result re-parsed as valid JSON).
- **Not yet tested on hardware:** the SDL2-only architecture (this
  eliminated a real crash found in the previous version, but is itself
  new) and the palette feature's Switch-specific glue — specifically,
  reading raw pixels out of a real `SDL_Surface` decoded by
  `SDL2_image` on an actual Switch, which is a different code path than
  anything tested before now.

## 1. Install devkitPro

<https://devkitpro.org/wiki/Getting_Started> — verify with:

```bash
echo $DEVKITPRO
```

## 2. Install the required packages

```bash
# Linux/macOS
sudo dkp-pacman -S switch-dev switch-curl switch-mbedtls switch-zlib switch-zziplib switch-sdl2 switch-sdl2_image switch-sdl2_ttf

# Windows (MSYS2 shell from the devkitPro installer)
pacman -S switch-dev switch-curl switch-mbedtls switch-zlib switch-zziplib switch-sdl2 switch-sdl2_image switch-sdl2_ttf
```

## 3. Check the romfs assets

The `romfs/` folder already contains everything the build needs —
`logo.png`, `logo_large.png`, and `cacert.pem` — all shipped with this
repo and baked into the `.nro`. Nothing goes on the SD card manually.

The bundled `cacert.pem` is a snapshot of Mozilla's CA list. If HTTPS
starts failing with certificate errors long after this release, refresh
it:

```bash
curl -o romfs/cacert.pem https://curl.se/ca/cacert.pem
```

If the logos are ever missing, the app still builds and runs fine — it
just falls back to a text-only header.

## 4. Build

```bash
make
```

## 5. Install on your Switch

Copy `a-theme-installer.nro` to `sdmc:/switch/a-theme-installer/` and
launch from the Homebrew Menu.

## How the palette feature works

On the preview screen, pressing **X**:

1. Takes the background image already decoded for the preview (no extra
   download — same image you're already looking at)
2. Samples an 80×80 grid of its pixels and runs k-means clustering to find
   5 dominant colors, sorted by how much of the image they cover
3. Maps them onto the theme's actual color fields — background, text,
   selection/accent (color + its own background + border), frame border,
   progress bar — using the same role-based logic the browser editor
   suggests by default
4. Edits `settings.json` **in place**: each color's hex text is overwritten
   with the new value at the exact same byte length (padding/alpha
   preserved), so the file's total size never changes and no JSON
   serializer is needed — just precise, verified string surgery
5. Writes the result straight back to the SD card and redraws the preview
   immediately with the new palette

Press **X** again for a different result (the clustering start point
shifts each time, so repeated presses can surface different groupings,
though very simple images may still converge to similar answers). **A**
keeps whatever's currently applied and finishes the install; **B** deletes
the whole thing.

## Troubleshooting notes for whoever compiles this first

Roughly in likelihood order:

- **Palette button (X) does nothing, or "Could not extract a palette"
  every time** — check that `bg_surface` is actually non-NULL where
  `show_theme_preview_and_confirm()` calls `load_theme_image_ex()`. If
  the background image itself fails to decode (bad URL, unsupported
  format, etc.), there's nothing to sample and this is expected — but if
  a background clearly loaded fine visually and this still fails, check
  `SDL_ConvertSurfaceFormat(..., SDL_PIXELFORMAT_RGBA32, 0)` succeeds on
  your SDL2_image build; some format edge cases (indexed/paletted PNGs in
  particular) are the most likely culprit.
- **Palette colors look wrong/inverted** — double check `conv->pitch` is
  being used correctly in `extract_palette_from_surface()`'s pixel
  indexing (`base + sy * conv->pitch + sx * 4`); pitch can differ from
  `width * 4` if the surface has row padding, which is exactly why pitch
  is used instead of assuming a fixed stride.
- **`plGetSharedFontByType` / shared font issues** — see the note in the
  previous version's notes: this is real API surface, not bundled, check
  `switch/include/switch/services/pl.h` in your installed libnx if it
  doesn't resolve.
- **Blank/black screen, nothing renders** — check
  `SDL_CreateWindowAndRenderer`'s return value; the app currently exits
  silently on graphics init failure rather than showing an error (nowhere
  to show it, since graphics is what failed).
- **Colors look too opaque / transparency not working** — confirm
  `SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND)` is
  actually taking effect; several real theme colors carry meaningful
  alpha (e.g. `1178997A`) and need real alpha blending, not just
  multiplication against black.
- **`ZZIP_DIR` not found** — confirm `switch-zziplib` installed, check
  `$DEVKITPRO/portlibs/switch/include/zzip/` for the actual header name.
- **Cert verification fails at runtime** — `CURLOPT_CAINFO` assumes
  `romfs:` paths resolve the same way `sdmc:` does for libcurl's Switch
  port; `CURLOPT_CAINFO_BLOB` is the fallback if not.

## Updating later

Nothing needs rebuilding when themes are added/removed/changed — the
manifest and each theme's `settings.json` are fetched fresh every launch.
