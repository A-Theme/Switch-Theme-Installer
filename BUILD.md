# Building A-Theme Installer

> **Heads up:** this source was written carefully against the documented
> libnx/curl-for-Switch APIs, but it has **not been compiled or run on real
> hardware** — the sandboxed environment this was written in blocks access
> to devkitPro's servers entirely, so there was no way to test-build it here.
> Treat this as a solid first draft. Expect to fix a handful of small
> compile issues (a missing include, a slightly off API signature, etc.) on
> your first `make` — that's completely normal for homebrew and not a sign
> anything is fundamentally wrong with the approach.

## What this is

A minimal on-console app: it fetches a plain-text list of themes from your
GitHub repo, shows them as a menu, and installs whichever one you pick
straight onto the SD card in Tinfoil's expected folder layout
(`sdmc:/switch/tinfoil/themes/<folder>/theme.json`). It deliberately does
**not** try to reproduce the visual editor — that stays a browser/desktop
tool, since a console controller is a bad fit for color pickers and text
input. This app's whole job is browse → download → install.

## 1. Install devkitPro

Follow the official installer for your OS: <https://devkitpro.org/wiki/Getting_Started>

Make sure `DEVKITPRO` and `DEVKITA64` end up set in your environment (the
installer does this for you on most platforms). Verify with:

```bash
echo $DEVKITPRO
# should print something like /opt/devkitpro
```

## 2. Install the required packages

```bash
# Linux/macOS (dkp-pacman ships with the devkitPro installer)
sudo dkp-pacman -S switch-dev switch-curl switch-mbedtls switch-zlib

# Windows (MSYS2 shell that the devkitPro installer sets up)
pacman -S switch-dev switch-curl switch-mbedtls switch-zlib
```

- `switch-dev` — the core libnx/devkitA64 toolchain package group
- `switch-curl` — libcurl built for the Switch (what this app uses for HTTPS)
- `switch-mbedtls` / `switch-zlib` — curl's own dependencies on this platform

## 3. Add the CA certificate bundle

libcurl needs a certificate bundle to actually verify HTTPS connections
(rather than blindly trusting whatever responds) — this app is written to
require it rather than silently disabling verification, since it's writing
files to your SD card based on what it downloads.

```bash
mkdir -p romfs
curl -o romfs/cacert.pem https://curl.se/ca/cacert.pem
```

(Any current CA bundle works — that's just the standard, well-known source
Mozilla/curl publish.) This file gets embedded into the `.nro` at build time
via the `ROMFS` directory and is not something you need to ship separately.

## 4. Build

From this folder:

```bash
make
```

If it succeeds, you'll get `a-theme-installer.nro` in this same folder.

## 5. Install on your Switch

Copy the `.nro` to:

```
sdmc:/switch/a-theme-installer/a-theme-installer.nro
```

(any subfolder under `sdmc:/switch/` works — the Homebrew Menu scans all of
them). Launch it from the Homebrew Menu (via a jailed exploit or, if you're
running a CFW like Atmosphère, directly from the Switch's Album applet using
the Homebrew Menu takeover, exactly like you'd launch Tinfoil itself).

## Troubleshooting notes for whoever compiles this first

A few things worth checking if the build or the app itself misbehaves,
since I couldn't verify any of this against a real compiler or console:

- **`HidNpadButton_*` names** — libnx has renamed/reorganized its pad-input
  API a few times across versions. If these don't resolve, check
  `switch/include/switch/services/hid.h` in your installed libnx for the
  current enum names and swap them in.
- **`curl_easy_setopt(..., CURLOPT_CAINFO, "romfs:/cacert.pem")`** — this
  assumes libcurl's Switch port resolves `romfs:` paths through the same
  devoptab virtual filesystem `sdmc:` uses. If cert verification fails at
  runtime, this is the first thing to check — some builds may need an
  absolute in-memory path via `CURLOPT_CAINFO_BLOB` instead.
- **`strtok_r`** — should be available via devkitA64's newlib, but if the
  linker complains, swap in plain `strtok` (this app doesn't use threads,
  so it isn't strictly needed here — `strtok_r` was just used to be safe).
- **Network before Wi-Fi is actually connected** — if the Switch hasn't
  joined a network yet, `http_get()` will just fail and the app shows the
  "check your Wi-Fi" screen. That's expected behavior, not a bug.

## Updating the theme list later

Nothing in the app itself needs rebuilding when you add new themes — it
reads `themes.txt` fresh over the network every time it launches. Just edit
`https://github.com/A-Theme/Tinfoil-Themes/blob/main/themes.txt` (see
`manifest-example/themes.txt` in this folder for the format) and push.
