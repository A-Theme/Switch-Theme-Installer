#!/usr/bin/env bash
# Pre-commit / pre-release secret scanner for A-Theme Installer.
#
#   ./scripts/scan-secrets.sh            scan working tree + tracked files
#   ./scripts/scan-secrets.sh --staged   scan only what git has staged
#
# Exit 0 = clean, 1 = something sensitive found.
#
# What this project's secrets actually look like: this app patches Tinfoil's
# own options.json in place on the SD card to set the active theme. A copy of
# that file pulled off a console for testing carries a real shop's live
# credentials - linkedUserSig, fingerprint, googleApiKey, saved username and
# password (see source/main.c's field-preservation comments). There are no
# console-unique keys here (no prod.keys, no NSP signing) - that's a
# romm-switch-client-style concern, not this one.

set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

RED=$'\033[0;31m'; GRN=$'\033[0;32m'; YEL=$'\033[0;33m'; RST=$'\033[0m'
FAIL=0
note()  { printf '%s\n' "$*"; }
bad()   { printf '%s[LEAK]%s %s\n' "$RED" "$RST" "$*"; FAIL=1; }
warn()  { printf '%s[warn]%s %s\n' "$YEL" "$RST" "$*"; }
ok()    { printf '%s[ ok ]%s %s\n' "$GRN" "$RST" "$*"; }

STAGED=0
[[ "${1:-}" == "--staged" ]] && STAGED=1

# A real captured options.json contains these as JSON string values with real
# data behind them - not just the bare word showing up in a code comment or
# README prose, which is why the match requires a quoted value 8+ chars long.
CRED_FIELD='"(linkedUserSig|fingerprint|googleApiKey)"[[:space:]]*:[[:space:]]*"[^"]{8,}"'
# Saved username/password fields Tinfoil itself writes into options.json.
LOGIN_FIELD='"(username|password)"[[:space:]]*:[[:space:]]*"[^"]+"'

# General-purpose tokens, in case a future script or CI file embeds one.
GENERIC='(ghp_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]{22,}|AKIA[0-9A-Z]{16})'

HAS_GIT=0
git rev-parse --is-inside-work-tree >/dev/null 2>&1 && HAS_GIT=1

note "=== 1. Tinfoil credential files in the working tree ==="
FOUND=0
for f in options.json options.json.bak; do
    [[ -f "$f" ]] || continue
    FOUND=1
    if [[ $HAS_GIT -eq 1 ]] && git check-ignore -q "$f" 2>/dev/null; then
        warn "$f present but gitignored - fine to keep locally, just don't force-add it"
    else
        bad "$f present and NOT ignored by git - this is a real console's Tinfoil shop credentials"
    fi
done
if [[ -f _download.zip ]]; then
    warn "_download.zip present - test debris, not secret, but should stay untracked"
fi
[[ $FOUND -eq 0 ]] && ok "no options.json / options.json.bak in the working tree"

note ""
note "=== 2. Credential-shaped content in tracked/staged files ==="
if [[ $STAGED -eq 1 ]]; then
    SCAN=$(git diff --cached --name-only --diff-filter=ACM 2>/dev/null)
else
    SCAN=$(git ls-files 2>/dev/null || find source include romfs Makefile README.md BUILD.md -type f 2>/dev/null)
fi

HITS=0
while IFS= read -r f; do
    [[ -z "$f" || ! -f "$f" ]] && continue
    # Skip the scanner's own source - it has to contain these patterns to
    # look for them, so it would otherwise flag itself.
    case "$f" in scripts/scan-secrets.sh|scripts/pre-commit) continue ;; esac
    if LC_ALL=C grep -qE "$CRED_FIELD" "$f" 2>/dev/null; then
        bad "Tinfoil credential field (linkedUserSig/fingerprint/googleApiKey) with a real value in: $f"; HITS=1
    fi
    if LC_ALL=C grep -qE "$LOGIN_FIELD" "$f" 2>/dev/null; then
        bad "saved username/password field with a real value in: $f"; HITS=1
    fi
    if LC_ALL=C grep -qE "$GENERIC" "$f" 2>/dev/null; then
        bad "what looks like a GitHub token or AWS key in: $f"; HITS=1
    fi
    if grep -q -- "-----BEGIN [A-Z ]*PRIVATE KEY-----" "$f" 2>/dev/null; then
        bad "private key block in tracked file: $f"; HITS=1
    fi
done <<< "$SCAN"
[[ $HITS -eq 0 ]] && ok "no credential-shaped content in tracked files"

note ""
if [[ $FAIL -eq 0 ]]; then
    printf '%s=== CLEAN ===%s\n' "$GRN" "$RST"
else
    printf '%s=== FAILED - do not commit or publish ===%s\n' "$RED" "$RST"
fi
exit $FAIL
