# Building A-Theme Installer

> **Status, honestly:** this project has a real track record at this
> point — the original plain-text-manifest version was successfully built
> on a real Windows devkitPro install (confirmed working `.nro`, no
> compile errors), and the JSON-parsing logic in every version since has
> been compiled and run for real on a PC against actual A-Theme content
> before being written into this app.
>
> This version adds a **preview screen** (see and confirm a theme's look
> before keeping it installed), which needed real graphics for the first
> time — `switch-sdl2` + `switch-sdl2_image`. That part is new, Switch-
> specific, and **could not be tested** in the environment this was
> written in. It's flagged clearly in Troubleshooting below, with the
> exact spot most likely to need real debugging on hardware.

## 1. Install devkitPro

Follow the official installer for your OS: <https://devkitpro.org/wiki/Getting_Started>

Verify `DEVKITPRO` is set:

```bash
echo $DEVKITPRO
# should print something like /opt/devkitpro
```

## 2. Install the required packages

```bash
# Linux/macOS
sudo dkp-pacman -S switch-dev switch-curl switch-mbedtls switch-zlib switch-zziplib switch-sdl2 switch-sdl2_image

# Windows (the MSYS2 shell the devkitPro installer sets up)
pacman -S switch-dev switch-curl switch-mbedtls switch-zlib switch-zziplib switch-sdl2 switch-sdl2_image
```

- `switch-dev` — core libnx/devkitA64 toolchain
- `switch-curl` / `switch-mbedtls` / `switch-zlib` — HTTPS downloads
- `switch-zziplib` — reads the downloaded `.zip` theme archives
- `switch-sdl2` / `switch-sdl2_image` — **new in this version**, used only
  for the preview screen (decoding + displaying a theme's background
  image/logo)

## 3. Add the CA certificate bundle

```bash
mkdir -p romfs
curl -o romfs/cacert.pem https://curl.se/ca/cacert.pem
```

Needed so HTTPS downloads are actually verified rather than blindly
trusted. Gets embedded into the `.nro` via the `ROMFS` directory.

## 4. Build

```bash
make
```

Produces `a-theme-installer.nro` (plus `.nacp`/`.elf` artifacts you can
ignore) in this folder.

## 5. Install on your Switch

Copy the `.nro` to:

```
sdmc:/switch/a-theme-installer/a-theme-installer.nro
```

Launch from the Homebrew Menu.

## How it works

- **A** on a highlighted theme installs it immediately, no preview.
- **Y** installs it, then shows a fullscreen preview of its background
  image (or logo, if no background image is set) — **A** keeps it, **B**
  removes it again. Either way, the theme is briefly, actually installed
  during the preview (there's no separate "download just a thumbnail"
  path available from the current manifest, since it only lists full
  theme `.zip`s) — choosing not to keep it just deletes what was written.

The manifest itself (`themes.json`) is fetched fresh from
`A-Theme/Tinfoil-Themes` every launch — nothing to maintain separately for
this app to work.

## Troubleshooting notes for whoever compiles this first

Roughly in order of how likely each one is to actually come up:

- **The SDL2/console handoff (`show_preview_and_confirm()` in main.c) —
  the single riskiest piece in this app.** It calls `consoleExit()` to
  release libnx's text console, then `SDL_Init(SDL_INIT_VIDEO)` to hand
  the display to SDL2, and reverses that afterward. Both ultimately
  compete for the same underlying display surface. If the preview screen
  shows nothing, shows garbage, or the app hangs/crashes specifically
  when entering or leaving the preview (menu navigation working fine
  otherwise), start here. Likely fixes if it misbehaves:
  - Try removing the `consoleExit()`/`consoleInit()` calls around the
    preview and see if SDL2 can coexist with the console left "open" —
    some libnx SDL2 setups tolerate this fine, some don't.
  - Check `$DEVKITPRO/portlibs/switch/include/SDL2/` to confirm the
    actual installed API matches what's called here (`SDL_CreateWindowAndRenderer`,
    `IMG_Load_RW`, etc.) — these are standard SDL2 calls that shouldn't
    have changed, but versions do drift.
- **`pkg-config: command not found` / `aarch64-none-elf-pkg-config` not
  found** — confirm `switch-sdl2` actually installed and that
  devkitA64's `bin/` is on your `PATH` (the devkitPro installer usually
  handles this, but a fresh MSYS2 shell occasionally needs re-sourcing
  its profile).
- **`ZZIP_DIR` / `zzip_dir_open` not found** — confirm `switch-zziplib`
  installed (`pacman -Qs zziplib`), and check
  `$DEVKITPRO/portlibs/switch/include/zzip/` for the actual header name —
  some versions use `<zzip/lib.h>` instead of `<zzip/zzip.h>`.
- **Cert verification fails at runtime** (downloads fail even though the
  build succeeded) — `CURLOPT_CAINFO` is set to `"romfs:/cacert.pem"`,
  assuming libcurl's Switch port resolves `romfs:` paths the same way
  `sdmc:` is resolved. If not, `CURLOPT_CAINFO_BLOB` with the cert loaded
  into memory is the fallback.
- **`HidNpadButton_*` names don't resolve** — libnx has renamed/
  reorganized pad-input enums a few times. Check
  `switch/include/switch/services/hid.h` in your installed libnx for the
  current names.
- **Preview shows the wrong image, or a stretched/squished one** — the
  scaling math in `show_preview_and_confirm()` is simple
  aspect-preserving fit-to-screen; if something looks off, it's more
  likely a bug there than in SDL2 itself, and should be quick to spot by
  printing `surface->w`/`surface->h` before the fit calculation.

## Updating later

Nothing about this app needs rebuilding when themes are added, removed, or
changed — it reads `themes.json` and each theme's `theme.json` fresh over
the network every time, so your existing publishing workflow just works.
