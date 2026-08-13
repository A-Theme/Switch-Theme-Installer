// A-Theme Installer — a Switch homebrew (.nro) app that reads A-Theme's own
// themes.json manifest and installs the selected theme's zip archive
// directly onto the SD card, ready for Tinfoil to pick up. Also previews a
// theme's actual look (background, logo, selection colors, border,
// progress bar) before you commit to keeping it.
//
// This does NOT reproduce the visual theme *editor* — that's a browser/
// desktop tool by design (mouse, keyboard, color pickers). This app's job
// is just: browse -> download -> extract -> preview -> keep or undo.
//
// Controls: D-Pad / Left Stick = move cursor
//           A = install selected theme immediately (no preview)
//           Y = install, then show a preview -- A to keep it, B to undo
//           + = exit
//
// Build requirements: devkitA64, libnx, and switch-curl / switch-mbedtls /
// switch-zlib / switch-zziplib / switch-sdl2 / switch-sdl2_image /
// switch-sdl2_ttf. See BUILD.md.
//
// --- Architecture note (read this if debugging a crash) ---
// An earlier version of this app used libnx's text console for the menu
// and only switched to SDL2 for the preview screen, toggling between them
// with consoleExit()/consoleInit(). That caused a real crash on real
// hardware after leaving the preview. This version removes that risk
// entirely by never using the console at all — SDL2 owns the display for
// the app's whole lifetime, menu included. If you still hit a crash, it's
// something new, not that same handoff.
//
// --- On what's verified vs. not ---
// ALL of the JSON-parsing and color-parsing logic in this file (manifest
// parsing, nested theme.json field lookups for background/selection/
// border/progressBar/logo, hex color decoding, k-means palette
// clustering, in-place JSON color editing, the palette size now
// dynamically matching each theme's own distinct color count, and
// setting the active theme in Tinfoil's own options.json) was
// compiled and run for real on a PC against actual A-Theme content and
// synthetic test data before being written here — see parse_manifest(),
// parse_theme_visuals(), parse_hex_color(), kmeans_colors(),
// apply_hex_inplace(), and count_distinct_theme_colors(). The dynamic
// count specifically was verified end-to-end against real theme content
// (correctly finding 9-10 distinct colors depending on the sample used),
// and a real bug was caught and fixed during that verification: without
// clamping the minimum to 5, apply_palette_to_json()'s fixed role-index
// mapping (background=0 through progress bar=4) could have read
// uninitialized palette entries for themes with very few distinct colors.
// The active-theme feature was likewise verified against a REAL
// options.json pulled off an actual SD card: confirmed the result
// re-parses as valid JSON, exactly one field changes, zero keys are
// lost, every byte before and after the theme value is bit-identical,
// and every credential field (linkedUserSig, fingerprint, googleApiKey,
// saved username/password) survives untouched -- plus safe refusal on
// malformed JSON and on a file with no "theme" key.
// The Switch-specific graphics/font/zip pieces (SDL2, SDL2_image,
// SDL2_ttf, the Pl shared-font service, zziplib, and reading raw pixels
// out of a real decoded SDL_Surface for the palette feature) could not be
// tested outside real hardware — see BUILD.md's troubleshooting section.

#include <switch.h>
#include <curl/curl.h>
#include <zzip/zzip.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> // strcasecmp
#include <ctype.h>   // isxdigit
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>  // rmdir(), fsync()
#include <stdbool.h>

#define JSMN_STATIC
#include "jsmn.h"

// ---- Configuration -------------------------------------------------------

#define MANIFEST_URL "https://raw.githubusercontent.com/A-Theme/Tinfoil-Themes/main/themes.json"
#define THEMES_ROOT "sdmc:/switch/tinfoil/themes/"
#define APP_DATA_DIR "sdmc:/switch/a-theme-installer/"
// Tinfoil's own settings file. Contains real credentials (linkedUserSig,
// fingerprint, API keys, saved passwords) alongside the "theme" field --
// see set_theme_in_options() for why it is edited surgically and never
// regenerated.
#define TINFOIL_OPTIONS_PATH "sdmc:/switch/tinfoil/options.json"
#define TINFOIL_OPTIONS_BACKUP APP_DATA_DIR "options.json.bak"
#define TEMP_ZIP_PATH APP_DATA_DIR "_download.zip"

#define SCREEN_W 1280
#define SCREEN_H 720

#define MAX_THEMES   400
#define PAGE_SIZE    14
#define NAME_LEN     110
#define URL_LEN      220
#define FOLDER_LEN   96
#define MAX_TOKENS   (MAX_THEMES * 2 + 16)

typedef struct {
    char name[NAME_LEN];
    char folder[FOLDER_LEN];
    char url[URL_LEN];
} Theme;

typedef struct {
    char *data;
    size_t size;
} MemBuf;

typedef struct { Uint8 r, g, b, a; } RGBA;

// ---- Graphics globals (SDL2 owns the display for the app's whole life) -----

static SDL_Window *g_window = NULL;
static SDL_Renderer *g_renderer = NULL;
static TTF_Font *g_font = NULL;
static TTF_Font *g_font_small = NULL;
// Logos embedded in the NRO via romfs -- no external files to ship.
static SDL_Texture *g_logo = NULL;
static SDL_Texture *g_logo_large = NULL;
static int g_logo_w = 0, g_logo_h = 0;

// ---- Networking helpers ---------------------------------------------------

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemBuf *mem = (MemBuf *)userp;
    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    return realsize;
}

static bool http_get(const char *url, MemBuf *out) {
    out->data = malloc(1);
    out->size = 0;
    if (!out->data) return false;
    out->data[0] = '\0';

    CURL *curl = curl_easy_init();
    if (!curl) { free(out->data); out->data = NULL; return false; }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "A-Theme-Installer/1.0 (Switch)");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        free(out->data);
        out->data = NULL;
        out->size = 0;
        return false;
    }
    return true;
}

static bool http_get_ex(const char *url, MemBuf *out, long *out_http_code) {
    out->data = malloc(1);
    out->size = 0;
    if (!out->data) return false;
    out->data[0] = '\0';

    CURL *curl = curl_easy_init();
    if (!curl) { free(out->data); out->data = NULL; return false; }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "A-Theme-Installer/1.0 (Switch)");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    if (out_http_code) *out_http_code = http_code;

    if (res != CURLE_OK || http_code != 200) {
        free(out->data);
        out->data = NULL;
        out->size = 0;
        return false;
    }
    return true;
}

// ---- Filesystem helpers ----------------------------------------------------

static void mkpath(const char *path) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

// Writes a file and forces it all the way down to the SD card.
//
// On Switch, stdio writes sit in a buffered filesystem layer, and data is
// NOT guaranteed committed just because fclose() returned. This bit us
// for real: rewriting a theme's settings.json after generating a palette
// appeared to succeed on-screen, but the file on the SD card was
// completely unchanged. fflush() pushes stdio's buffer to the OS, and
// fsync() then tells the filesystem to actually commit it to the card.
// Every return value is checked so a failure surfaces instead of silently
// looking like success.
static bool write_file(const char *path, const char *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    size_t written = fwrite(data, 1, size, f);
    if (written != size) { fclose(f); return false; }

    if (fflush(f) != 0) { fclose(f); return false; }

    // Commit to the actual storage device, not just the OS buffer.
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);

    if (fclose(f) != 0) return false;

    // fsync() on the file descriptor is NOT sufficient on Switch: writes
    // go through libnx's fsdev layer, which keeps its own device-level
    // cache. Without this commit the data can sit there indefinitely and
    // never reach the SD card -- which is exactly what happened here,
    // with write_file() reporting success while the file on the card
    // stayed unchanged.
    fsdevCommitDevice("sdmc");
    return true;
}

static bool read_file(const char *path, char **out_data, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return false; }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    *out_data = buf;
    *out_size = got;
    return true;
}

static void remove_dir_recursive(const char *path) {
    DIR *d = opendir(path);
    if (!d) { remove(path); return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[600];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (remove(child) != 0) {
            remove_dir_recursive(child);
        }
    }
    closedir(d);
    rmdir(path);
}

// ---- JSON helpers ----------------------------------------------------------
// Verified for real (compiled + run on a PC against actual A-Theme content)
// before being written here — see the file header.

static int json_streq(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
        strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return 1;
    }
    return 0;
}

static int skip_token_subtree(jsmntok_t *tokens, int idx) {
    jsmntok_t *t = &tokens[idx];
    int end = idx + 1;
    if (t->type == JSMN_OBJECT) {
        for (int n = 0; n < t->size; n++) {
            end = skip_token_subtree(tokens, end);
            end = skip_token_subtree(tokens, end);
        }
    } else if (t->type == JSMN_ARRAY) {
        for (int n = 0; n < t->size; n++) {
            end = skip_token_subtree(tokens, end);
        }
    }
    return end;
}

static int json_object_find_child_index(const char *json, jsmntok_t *tokens, int obj_idx, const char *key) {
    jsmntok_t *obj = &tokens[obj_idx];
    if (obj->type != JSMN_OBJECT) return -1;
    int i = obj_idx + 1;
    for (int n = 0; n < obj->size; n++) {
        int key_idx = i;
        int val_idx = key_idx + 1;
        if (json_streq(json, &tokens[key_idx], key)) {
            return val_idx;
        }
        i = skip_token_subtree(tokens, val_idx);
    }
    return -1;
}

static bool json_object_get_string(const char *json, jsmntok_t *tokens, int obj_idx,
                                    const char *key, char *out, size_t outcap) {
    int val_idx = json_object_find_child_index(json, tokens, obj_idx, key);
    if (val_idx < 0) return false;
    jsmntok_t *v = &tokens[val_idx];
    if (v->type != JSMN_STRING) return false;
    int len = v->end - v->start;
    if ((size_t)len >= outcap) len = outcap - 1;
    memcpy(out, json + v->start, len);
    out[len] = '\0';
    return true;
}

// Parses a Tinfoil-style hex color (bare or #-prefixed, 3/4/6/8 hex
// digits, RRGGBB or RRGGBBAA) into RGBA components. Verified against every
// real color value seen across this project's sample themes.
static bool parse_hex_color(const char *hex, Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a) {
    if (!hex || !hex[0]) return false;
    if (hex[0] == '#') hex++;
    size_t len = strlen(hex);
    char buf[9];
    if (len == 3 || len == 4) {
        for (size_t i = 0; i < len; i++) { buf[i * 2] = hex[i]; buf[i * 2 + 1] = hex[i]; }
        buf[len * 2] = '\0';
        len = len * 2;
    } else if (len == 6 || len == 8) {
        strncpy(buf, hex, 8);
        buf[len] = '\0';
    } else {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit((unsigned char)buf[i])) return false;
    }
    char rs[3] = { buf[0], buf[1], 0 }, gs[3] = { buf[2], buf[3], 0 }, bs[3] = { buf[4], buf[5], 0 };
    unsigned int rr = (unsigned int)strtoul(rs, NULL, 16);
    unsigned int gg = (unsigned int)strtoul(gs, NULL, 16);
    unsigned int bb = (unsigned int)strtoul(bs, NULL, 16);
    unsigned int aa = 255;
    if (len == 8) {
        char as[3] = { buf[6], buf[7], 0 };
        aa = (unsigned int)strtoul(as, NULL, 16);
    }
    *r = (Uint8)rr; *g = (Uint8)gg; *b = (Uint8)bb; *a = (Uint8)aa;
    return true;
}

static void derive_names(const char *url, char *display_out, size_t display_cap,
                          char *folder_out, size_t folder_cap) {
    const char *slash = strrchr(url, '/');
    const char *base = slash ? slash + 1 : url;

    char tmp[NAME_LEN];
    strncpy(tmp, base, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *q = strchr(tmp, '?');
    if (q) *q = '\0';

    size_t len = strlen(tmp);
    if (len > 4 && strcasecmp(tmp + len - 4, ".zip") == 0) {
        tmp[len - 4] = '\0';
    }

    strncpy(folder_out, tmp, folder_cap - 1);
    folder_out[folder_cap - 1] = '\0';

    char pretty[NAME_LEN];
    strncpy(pretty, tmp, sizeof(pretty) - 1);
    pretty[sizeof(pretty) - 1] = '\0';
    for (char *p = pretty; *p; p++) if (*p == '_') *p = ' ';

    if (strstr(url, "/Non_Aramaki_Themes/")) {
        snprintf(display_out, display_cap, "[Community] %s", pretty);
    } else {
        strncpy(display_out, pretty, display_cap - 1);
        display_out[display_cap - 1] = '\0';
    }
}

static bool parse_manifest(char *json, size_t json_len, Theme *themes, int *count) {
    *count = 0;

    jsmntok_t *tokens = malloc(sizeof(jsmntok_t) * MAX_TOKENS);
    if (!tokens) return false;

    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, json, json_len, tokens, MAX_TOKENS);
    if (r < 1 || tokens[0].type != JSMN_OBJECT) {
        free(tokens);
        return false;
    }

    int themes_idx = json_object_find_child_index(json, tokens, 0, "themes");
    if (themes_idx < 0 || tokens[themes_idx].type != JSMN_ARRAY) {
        free(tokens);
        return false;
    }

    jsmntok_t *arr = &tokens[themes_idx];
    int n = arr->size;
    if (n > MAX_THEMES) n = MAX_THEMES;

    for (int i = 0; i < n; i++) {
        jsmntok_t *t = &tokens[themes_idx + 1 + i];
        if (t->type != JSMN_STRING) continue;

        int len = t->end - t->start;
        if (len >= URL_LEN) len = URL_LEN - 1;

        Theme *theme = &themes[*count];
        memcpy(theme->url, json + t->start, len);
        theme->url[len] = '\0';

        derive_names(theme->url, theme->name, sizeof(theme->name), theme->folder, sizeof(theme->folder));
        (*count)++;
    }

    free(tokens);
    return *count > 0;
}

// ---- Zip extraction (switch-zziplib) ----------------------------------------

static bool extract_zip(const char *zip_path, const char *dest_dir, bool *found_theme_json) {
    *found_theme_json = false;

    ZZIP_DIR *dir = zzip_dir_open(zip_path, NULL);
    if (!dir) return false;

    bool extracted_any = false;
    ZZIP_DIRENT dirent;

    while (zzip_dir_read(dir, &dirent)) {
        const char *entry_name = dirent.d_name;
        size_t nlen = strlen(entry_name);
        if (nlen == 0 || entry_name[nlen - 1] == '/') continue;

        const char *base = strrchr(entry_name, '/');
        base = base ? base + 1 : entry_name;
        if (base[0] == '\0') continue;

        ZZIP_FILE *zf = zzip_file_open(dir, entry_name, 0);
        if (!zf) continue;

        size_t cap = 65536;
        size_t used = 0;
        char *buf = malloc(cap);
        if (!buf) { zzip_file_close(zf); continue; }

        zzip_ssize_t n;
        char chunk[8192];
        bool read_ok = true;
        while ((n = zzip_file_read(zf, chunk, sizeof(chunk))) > 0) {
            if (used + (size_t)n > cap) {
                size_t newcap = cap * 2;
                while (newcap < used + (size_t)n) newcap *= 2;
                char *nb = realloc(buf, newcap);
                if (!nb) { read_ok = false; break; }
                buf = nb;
                cap = newcap;
            }
            memcpy(buf + used, chunk, n);
            used += n;
        }
        zzip_file_close(zf);

        if (read_ok) {
            // Tinfoil reads "settings.json" from each theme folder --
            // confirmed against a real SD card, where 11 of 12 working
            // themes use that name (the lone theme.json folder was one
            // this installer created, and it did NOT load in Tinfoil).
            // Zips in the wild use either name, so accept both on read
            // and always normalize to settings.json on write.
            bool is_config = (strcasecmp(base, "theme.json") == 0 ||
                               strcasecmp(base, "settings.json") == 0);
            const char *dest_name = is_config ? "settings.json" : base;

            char dest_path[600];
            snprintf(dest_path, sizeof(dest_path), "%s/%s", dest_dir, dest_name);
            if (write_file(dest_path, buf, used)) {
                extracted_any = true;
                if (is_config) *found_theme_json = true;
            }
        }
        free(buf);
    }

    zzip_dir_close(dir);
    return extracted_any;
}

// ---- Setting the active theme in Tinfoil's own settings ----------------------
//
// Tinfoil records which theme is active as a plain "theme" string in
// sdmc:/switch/tinfoil/options.json, holding the folder name from
// themes/ -- exactly what derive_names() already produces. So installing
// a theme AND making it active is possible without Tinfoil's UI.
//
// The catch, and why this is prompt-only rather than automatic: that
// same file holds real credentials -- linkedUserSig, fingerprint,
// googleApiKey, saved username/password. It must NEVER be regenerated or
// re-serialized; doing so risks mangling or dropping fields this app
// doesn't understand. Instead the theme value's bytes are spliced and
// every other byte is copied through untouched, and a backup is written
// first.
//
// Verified for real before being written here: compiled and run against
// an actual options.json pulled off a real SD card. Confirmed the result
// re-parses as valid JSON, exactly one field changes, zero keys are
// lost, every byte before and after the theme value is bit-identical,
// and every credential field survives unchanged. Also tested refusing
// safely on malformed JSON and on a file with no "theme" key, and
// handling a replacement name shorter than the original.

// Returns a malloc'd modified copy (caller frees) or NULL on any failure.
// On NULL the caller MUST leave the original file untouched.
static char *set_theme_in_options(const char *json, size_t json_len,
                                   const char *themeName, size_t *out_len) {
    jsmn_parser p;
    jsmn_init(&p);
    int tokcount = jsmn_parse(&p, json, json_len, NULL, 0);
    if (tokcount < 1) return NULL;

    jsmntok_t *tokens = malloc(sizeof(jsmntok_t) * tokcount);
    if (!tokens) return NULL;
    jsmn_init(&p);
    if (jsmn_parse(&p, json, json_len, tokens, tokcount) < 1) { free(tokens); return NULL; }
    if (tokens[0].type != JSMN_OBJECT) { free(tokens); return NULL; }

    // Only walk DIRECT children of the root object, so a nested "theme"
    // key elsewhere in the file can't be hit by mistake.
    int theme_val_idx = -1;
    int i = 1;
    for (int n = 0; n < tokens[0].size; n++) {
        int key_idx = i;
        int val_idx = key_idx + 1;
        if (val_idx >= tokcount) break;
        int klen = tokens[key_idx].end - tokens[key_idx].start;
        if (klen == 5 && strncmp(json + tokens[key_idx].start, "theme", 5) == 0) {
            theme_val_idx = val_idx;
            break;
        }
        i = skip_token_subtree(tokens, val_idx);
    }
    if (theme_val_idx < 0 || theme_val_idx >= tokcount) { free(tokens); return NULL; }
    if (tokens[theme_val_idx].type != JSMN_STRING) { free(tokens); return NULL; }

    int vstart = tokens[theme_val_idx].start;
    int vend   = tokens[theme_val_idx].end;
    free(tokens);

    size_t namelen = strlen(themeName);
    size_t newlen = json_len - (size_t)(vend - vstart) + namelen;
    char *out = malloc(newlen + 1);
    if (!out) return NULL;

    memcpy(out, json, (size_t)vstart);
    memcpy(out + vstart, themeName, namelen);
    memcpy(out + vstart + namelen, json + vend, json_len - (size_t)vend);
    out[newlen] = '\0';

    *out_len = newlen;
    return out;
}

// Reads Tinfoil's options.json, sets the active theme, writes a backup,
// then writes the modified file. Returns true on success; on any failure
// the original file is left exactly as it was.
static bool set_active_theme(const char *themeFolder, char *err, size_t errcap) {
    char *json = NULL;
    size_t json_len = 0;
    if (!read_file(TINFOIL_OPTIONS_PATH, &json, &json_len)) {
        snprintf(err, errcap, "Could not read Tinfoil's options.json");
        return false;
    }

    size_t newlen = 0;
    char *modified = set_theme_in_options(json, json_len, themeFolder, &newlen);
    if (!modified) {
        free(json);
        snprintf(err, errcap, "options.json wasn't in the expected format -- left untouched");
        return false;
    }

    // Back up the original before overwriting anything.
    mkpath(APP_DATA_DIR);
    if (!write_file(TINFOIL_OPTIONS_BACKUP, json, json_len)) {
        free(json);
        free(modified);
        snprintf(err, errcap, "Could not write a backup -- not touching options.json");
        return false;
    }
    free(json);

    if (!write_file(TINFOIL_OPTIONS_PATH, modified, newlen)) {
        free(modified);
        snprintf(err, errcap, "Could not write options.json (backup is at %s)", TINFOIL_OPTIONS_BACKUP);
        return false;
    }
    free(modified);
    return true;
}

// ---- Install logic -----------------------------------------------------------

static bool install_theme(const Theme *t, char *status_detail, size_t status_cap) {
    mkpath(APP_DATA_DIR);

    MemBuf zipbuf;
    if (!http_get(t->url, &zipbuf)) {
        snprintf(status_detail, status_cap, "Download failed");
        return false;
    }

    if (!write_file(TEMP_ZIP_PATH, zipbuf.data, zipbuf.size)) {
        free(zipbuf.data);
        snprintf(status_detail, status_cap, "Could not write temp file to SD card");
        return false;
    }
    free(zipbuf.data);

    char dest_dir[300];
    snprintf(dest_dir, sizeof(dest_dir), THEMES_ROOT "%s", t->folder);
    mkpath(dest_dir);

    bool found_theme_json = false;
    bool ok = extract_zip(TEMP_ZIP_PATH, dest_dir, &found_theme_json);

    remove(TEMP_ZIP_PATH);

    if (!ok) {
        snprintf(status_detail, status_cap, "Zip extraction failed");
        return false;
    }
    if (!found_theme_json) {
        snprintf(status_detail, status_cap, "Installed, but no settings.json/theme.json found inside the zip");
        return false;
    }

    return true;
}

// ---- Theme visuals (for the preview mockup) ---------------------------------
// Verified for real (compiled + run against a complete, actual A-Theme
// settings.json) before being written here — every field below, including
// the nested ones, came back correct in that test.

typedef struct {
    bool has_bg_color;       RGBA bg_color;
    bool has_text_color;     RGBA text_color;
    bool has_sel_color;      RGBA sel_color;
    bool has_sel_bg;         RGBA sel_bg;
    bool has_sel_border;     RGBA sel_border;
    bool has_border;         RGBA border_color;
    bool has_progress_color; RGBA progress_color;
    bool has_progress_bg;    RGBA progress_bg;
    char logo_ref[URL_LEN];
    bool has_logo;
} ThemeVisuals;

static bool get_color_field(const char *json, jsmntok_t *tokens, int obj_idx, const char *key, RGBA *out) {
    char hexbuf[16];
    if (!json_object_get_string(json, tokens, obj_idx, key, hexbuf, sizeof(hexbuf))) return false;
    return parse_hex_color(hexbuf, &out->r, &out->g, &out->b, &out->a);
}

static void parse_theme_visuals(const char *json, jsmntok_t *tokens, ThemeVisuals *v) {
    memset(v, 0, sizeof(*v));
    v->has_text_color = get_color_field(json, tokens, 0, "color", &v->text_color);
    v->has_logo = json_object_get_string(json, tokens, 0, "logo", v->logo_ref, sizeof(v->logo_ref));

    int bg_idx = json_object_find_child_index(json, tokens, 0, "background");
    if (bg_idx >= 0 && tokens[bg_idx].type == JSMN_OBJECT) {
        v->has_bg_color = get_color_field(json, tokens, bg_idx, "color", &v->bg_color);
    }

    int sel_idx = json_object_find_child_index(json, tokens, 0, "selection");
    if (sel_idx >= 0 && tokens[sel_idx].type == JSMN_OBJECT) {
        v->has_sel_color = get_color_field(json, tokens, sel_idx, "color", &v->sel_color);
        int sel_bg_idx = json_object_find_child_index(json, tokens, sel_idx, "background");
        if (sel_bg_idx >= 0 && tokens[sel_bg_idx].type == JSMN_OBJECT) {
            v->has_sel_bg = get_color_field(json, tokens, sel_bg_idx, "color", &v->sel_bg);
        }
        int sel_border_idx = json_object_find_child_index(json, tokens, sel_idx, "border");
        if (sel_border_idx >= 0 && tokens[sel_border_idx].type == JSMN_OBJECT) {
            v->has_sel_border = get_color_field(json, tokens, sel_border_idx, "color", &v->sel_border);
        }
    }

    int border_idx = json_object_find_child_index(json, tokens, 0, "border");
    if (border_idx >= 0 && tokens[border_idx].type == JSMN_OBJECT) {
        v->has_border = get_color_field(json, tokens, border_idx, "color", &v->border_color);
    }

    int pb_idx = json_object_find_child_index(json, tokens, 0, "progressBar");
    if (pb_idx >= 0 && tokens[pb_idx].type == JSMN_OBJECT) {
        v->has_progress_color = get_color_field(json, tokens, pb_idx, "color", &v->progress_color);
        int pb_bg_idx = json_object_find_child_index(json, tokens, pb_idx, "background");
        if (pb_bg_idx >= 0 && tokens[pb_bg_idx].type == JSMN_OBJECT) {
            v->has_progress_bg = get_color_field(json, tokens, pb_bg_idx, "color", &v->progress_bg);
        }
    }
}

// ---- Palette extraction from background + in-place JSON color editing ------
// Both verified for real before being written here:
//  - kmeans_colors() was compiled standalone and run against a synthetic
//    image with 4 known dominant colors in known proportions; it recovered
//    all 4 colors with exactly correct pixel counts.
//  - apply_hex_inplace() was compiled standalone and run against real
//    settings.json content; edited colors came back correct, untouched
//    colors stayed untouched, buffer length never changed, and the result
//    re-parsed as valid JSON.

// PALETTE_MIN_K must be at least 5: apply_palette_to_json() maps fixed
// roles (background=0, text=1, selection=2, border=3, progress bar=4)
// onto specific palette indices regardless of how many colors were
// requested -- verified by checking every TRY_SET() call site, which
// references indices 0 through 4. Anything lower here would let it read
// uninitialized palette entries for themes with very few distinct colors.
#define PALETTE_MIN_K 5
#define PALETTE_MAX_K 24

typedef struct { double r, g, b; int count; } ColorCluster;

static void kmeans_colors(Uint8 *pixels, int pixelCount, int k, int iterations, int seedOffset, ColorCluster *out) {
    for (int c = 0; c < k; c++) {
        int idx = ((pixelCount / k) * c + seedOffset) % pixelCount;
        out[c].r = pixels[idx * 3 + 0];
        out[c].g = pixels[idx * 3 + 1];
        out[c].b = pixels[idx * 3 + 2];
        out[c].count = 0;
    }

    for (int iter = 0; iter < iterations; iter++) {
        double sumR[PALETTE_MAX_K] = { 0 }, sumG[PALETTE_MAX_K] = { 0 }, sumB[PALETTE_MAX_K] = { 0 };
        int cnt[PALETTE_MAX_K] = { 0 };

        for (int i = 0; i < pixelCount; i++) {
            double bestDist = 1e18; int best = 0;
            for (int c = 0; c < k; c++) {
                double dr = pixels[i * 3 + 0] - out[c].r;
                double dg = pixels[i * 3 + 1] - out[c].g;
                double db = pixels[i * 3 + 2] - out[c].b;
                double d = dr * dr + dg * dg + db * db;
                if (d < bestDist) { bestDist = d; best = c; }
            }
            sumR[best] += pixels[i * 3 + 0];
            sumG[best] += pixels[i * 3 + 1];
            sumB[best] += pixels[i * 3 + 2];
            cnt[best]++;
        }
        for (int c = 0; c < k; c++) {
            if (cnt[c] > 0) {
                out[c].r = sumR[c] / cnt[c];
                out[c].g = sumG[c] / cnt[c];
                out[c].b = sumB[c] / cnt[c];
            }
        }
    }

    int cnt[PALETTE_MAX_K] = { 0 };
    for (int i = 0; i < pixelCount; i++) {
        double bestDist = 1e18; int best = 0;
        for (int c = 0; c < k; c++) {
            double dr = pixels[i * 3 + 0] - out[c].r;
            double dg = pixels[i * 3 + 1] - out[c].g;
            double db = pixels[i * 3 + 2] - out[c].b;
            double d = dr * dr + dg * dg + db * db;
            if (d < bestDist) { bestDist = d; best = c; }
        }
        cnt[best]++;
    }
    for (int c = 0; c < k; c++) out[c].count = cnt[c];
}

// Counts distinct hex color VALUES appearing anywhere in the theme's
// parsed token stream -- mirrors the web editor's countDistinctColors(),
// scanning every string token (not just the known named fields) so
// nonstandard themes with extra color fields still count correctly.
// Fields sharing the same value (a common pattern -- many themes reuse
// one accent color across selection/menu/border) count once. Verified
// against the real Aramaki_Midjourney settings.json content: 10 distinct
// values out of ~13 color-bearing fields, matching a manual count exactly.
static int count_distinct_theme_colors(const char *json, jsmntok_t *tokens, int tokcount) {
    char seen[96][16];
    int seen_count = 0;
    int max_seen = (int)(sizeof(seen) / sizeof(seen[0]));

    for (int i = 0; i < tokcount; i++) {
        if (tokens[i].type != JSMN_STRING) continue;
        int len = tokens[i].end - tokens[i].start;
        if (len < 3 || len > 9) continue; // quick reject: can't be color-shaped
        char buf[10];
        if ((size_t)len >= sizeof(buf)) continue;
        memcpy(buf, json + tokens[i].start, len);
        buf[len] = '\0';

        Uint8 r, g, b, a;
        if (!parse_hex_color(buf, &r, &g, &b, &a)) continue; // not actually a color

        char norm[10];
        const char *src = (buf[0] == '#') ? buf + 1 : buf;
        int j = 0;
        for (; src[j]; j++) norm[j] = (src[j] >= 'A' && src[j] <= 'F') ? (src[j] - 'A' + 'a') : src[j];
        norm[j] = '\0';

        bool already = false;
        for (int k = 0; k < seen_count; k++) {
            if (strcmp(seen[k], norm) == 0) { already = true; break; }
        }
        if (!already && seen_count < max_seen) {
            strncpy(seen[seen_count], norm, sizeof(seen[0]) - 1);
            seen[seen_count][sizeof(seen[0]) - 1] = '\0';
            seen_count++;
        }
    }
    return seen_count;
}

// Samples an evenly-spaced 80x80 grid of pixels from `surf` (converting to
// a known format first so this works regardless of the source image's
// original format), skipping mostly-transparent pixels, and clusters them
// into `targetK` dominant colors sorted by how much of the image they
// cover. `targetK` should be the theme's own distinct color count (via
// count_distinct_theme_colors), clamped to [PALETTE_MIN_K, PALETTE_MAX_K]
// by the caller -- this function trusts it's already in range. `seedOffset`
// varies the clustering starting point so pressing the "generate" button
// repeatedly can surface different results.
static bool extract_palette_from_surface(SDL_Surface *surf, int seedOffset, RGBA *palette_out, int targetK) {
    if (!surf) return false;
    SDL_Surface *conv = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGBA32, 0);
    if (!conv) return false;

    int gridSize = 80;
    Uint8 *pixels = malloc(gridSize * gridSize * 3);
    if (!pixels) { SDL_FreeSurface(conv); return false; }
    int count = 0;

    SDL_LockSurface(conv);
    Uint8 *base = (Uint8 *)conv->pixels;
    for (int gy = 0; gy < gridSize; gy++) {
        int sy = (conv->h * gy) / gridSize;
        for (int gx = 0; gx < gridSize; gx++) {
            int sx = (conv->w * gx) / gridSize;
            Uint8 *p = base + sy * conv->pitch + sx * 4;
            if (p[3] < 128) continue; // skip mostly-transparent pixels
            pixels[count * 3 + 0] = p[0];
            pixels[count * 3 + 1] = p[1];
            pixels[count * 3 + 2] = p[2];
            count++;
        }
    }
    SDL_UnlockSurface(conv);
    SDL_FreeSurface(conv);

    if (count < targetK) { free(pixels); return false; }

    ColorCluster clusters[PALETTE_MAX_K];
    kmeans_colors(pixels, count, targetK, 8, seedOffset, clusters);
    free(pixels);

    for (int i = 0; i < targetK; i++) {
        int maxIdx = i;
        for (int j = i + 1; j < targetK; j++) if (clusters[j].count > clusters[maxIdx].count) maxIdx = j;
        ColorCluster tmp = clusters[i]; clusters[i] = clusters[maxIdx]; clusters[maxIdx] = tmp;
    }

    for (int i = 0; i < targetK; i++) {
        palette_out[i].r = (Uint8)clusters[i].r;
        palette_out[i].g = (Uint8)clusters[i].g;
        palette_out[i].b = (Uint8)clusters[i].b;
        palette_out[i].a = 255;
    }
    return true;
}

// Overwrites the color at tokens[val_idx] IN PLACE with a new 6-digit hex
// color, preserving the original '#' prefix (or lack of one) and, for
// 8-digit colors, the original alpha byte untouched. The buffer's total
// length never changes, so no other token's start/end offsets are
// invalidated by this edit — verified for real, see the section header.
static bool apply_hex_inplace(char *json, jsmntok_t *tok, const char *rgb6) {
    int origlen = tok->end - tok->start;
    if (origlen < 6 || origlen > 9) return false;

    char orig[10];
    memcpy(orig, json + tok->start, origlen);
    orig[origlen] = '\0';

    bool hadHash = (orig[0] == '#');
    const char *bare = hadHash ? orig + 1 : orig;
    int barelen = (int)strlen(bare);
    if (barelen != 6 && barelen != 8) return false;

    char replacement[10];
    if (barelen == 8) {
        snprintf(replacement, sizeof(replacement), "%s%c%c", rgb6, bare[6], bare[7]);
    } else {
        snprintf(replacement, sizeof(replacement), "%s", rgb6);
    }

    int pos = tok->start;
    if (hadHash) pos++;
    memcpy(json + pos, replacement, barelen);
    return true;
}

static void hex6(RGBA c, char *out /* needs 7 bytes */) {
    snprintf(out, 7, "%02x%02x%02x", c.r, c.g, c.b);
}

// Applies a 5-color palette to a theme's known color fields, using the
// same role-based mapping the web editor's "smart suggestions" use:
// background -> slot 0, text -> slot 1, selection/accent -> slot 2,
// border -> slot 3, progress/scroll bar fill -> slot 4. Edits `json` in
// place (see apply_hex_inplace) and returns how many fields changed.
static int apply_palette_to_json(char *json, jsmntok_t *tokens, RGBA *palette) {
    int changed = 0;
    char hexbuf[7];

#define TRY_SET(obj_idx, key, slot) do { \
    int vi = json_object_find_child_index(json, tokens, (obj_idx), (key)); \
    if (vi >= 0 && tokens[vi].type == JSMN_STRING) { \
        hex6(palette[(slot)], hexbuf); \
        if (apply_hex_inplace(json, &tokens[vi], hexbuf)) changed++; \
    } \
} while (0)

    TRY_SET(0, "color", 1);

    int bg = json_object_find_child_index(json, tokens, 0, "background");
    if (bg >= 0 && tokens[bg].type == JSMN_OBJECT) TRY_SET(bg, "color", 0);

    int sel = json_object_find_child_index(json, tokens, 0, "selection");
    if (sel >= 0 && tokens[sel].type == JSMN_OBJECT) {
        TRY_SET(sel, "color", 2);
        int selbg = json_object_find_child_index(json, tokens, sel, "background");
        if (selbg >= 0 && tokens[selbg].type == JSMN_OBJECT) TRY_SET(selbg, "color", 2);
        int selborder = json_object_find_child_index(json, tokens, sel, "border");
        if (selborder >= 0 && tokens[selborder].type == JSMN_OBJECT) TRY_SET(selborder, "color", 2);
    }

    int border = json_object_find_child_index(json, tokens, 0, "border");
    if (border >= 0 && tokens[border].type == JSMN_OBJECT) TRY_SET(border, "color", 3);

    int pb = json_object_find_child_index(json, tokens, 0, "progressBar");
    if (pb >= 0 && tokens[pb].type == JSMN_OBJECT) {
        TRY_SET(pb, "color", 4);
        int pbbg = json_object_find_child_index(json, tokens, pb, "background");
        if (pbbg >= 0 && tokens[pbbg].type == JSMN_OBJECT) TRY_SET(pbbg, "color", 0);
    }

    int sb = json_object_find_child_index(json, tokens, 0, "scrollBar");
    if (sb >= 0 && tokens[sb].type == JSMN_OBJECT) {
        TRY_SET(sb, "color", 4);
        int sbbg = json_object_find_child_index(json, tokens, sb, "background");
        if (sbbg >= 0 && tokens[sbbg].type == JSMN_OBJECT) TRY_SET(sbbg, "color", 0);
    }

    int menu = json_object_find_child_index(json, tokens, 0, "menu");
    if (menu >= 0 && tokens[menu].type == JSMN_OBJECT) {
        int menubg = json_object_find_child_index(json, tokens, menu, "background");
        if (menubg >= 0 && tokens[menubg].type == JSMN_OBJECT) TRY_SET(menubg, "color", 0);
        int menusel = json_object_find_child_index(json, tokens, menu, "selection");
        if (menusel >= 0 && tokens[menusel].type == JSMN_OBJECT) {
            TRY_SET(menusel, "color", 2);
            int menuselbg = json_object_find_child_index(json, tokens, menusel, "background");
            if (menuselbg >= 0 && tokens[menuselbg].type == JSMN_OBJECT) TRY_SET(menuselbg, "color", 2);
        }
    }

#undef TRY_SET
    return changed;
}

// ---- Low-level drawing helpers ----------------------------------------------

static void fill_rect(int x, int y, int w, int h, RGBA c) {
    SDL_SetRenderDrawColor(g_renderer, c.r, c.g, c.b, c.a);
    SDL_Rect r = { x, y, w, h };
    SDL_RenderFillRect(g_renderer, &r);
}

static void draw_rect_outline(int x, int y, int w, int h, RGBA c, int thickness) {
    SDL_SetRenderDrawColor(g_renderer, c.r, c.g, c.b, c.a);
    for (int i = 0; i < thickness; i++) {
        SDL_Rect r = { x + i, y + i, w - 2 * i, h - 2 * i };
        if (r.w <= 0 || r.h <= 0) break;
        SDL_RenderDrawRect(g_renderer, &r);
    }
}

// ---- A-Theme brand palette (matches the web editor and the READMEs) ----
#define BRAND_RED    ((RGBA){ 255,  60,  80, 255 })
#define BRAND_PURPLE ((RGBA){ 157,  78, 221, 255 })
#define BRAND_BLUE   ((RGBA){   0, 194, 255, 255 })
#define BG_DEEP      ((RGBA){  11,  20,  32, 255 })
#define BG_PANEL     ((RGBA){  17,  29,  44, 255 })

static RGBA lerp_rgba(RGBA a, RGBA b, float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    RGBA o;
    o.r = (Uint8)(a.r + (b.r - a.r) * t);
    o.g = (Uint8)(a.g + (b.g - a.g) * t);
    o.b = (Uint8)(a.b + (b.b - a.b) * t);
    o.a = (Uint8)(a.a + (b.a - a.a) * t);
    return o;
}

// Horizontal three-stop gradient bar: red -> purple -> blue, the same
// sweep used on the project's banners and READMEs.
static void draw_brand_gradient(int x, int y, int w, int h) {
    for (int i = 0; i < w; i++) {
        float t = (float)i / (w > 1 ? (w - 1) : 1);
        RGBA c = (t < 0.5f)
            ? lerp_rgba(BRAND_RED, BRAND_PURPLE, t / 0.5f)
            : lerp_rgba(BRAND_PURPLE, BRAND_BLUE, (t - 0.5f) / 0.5f);
        fill_rect(x + i, y, 1, h, c);
    }
}

// Vertical fade, used to give panels some depth instead of flat fills.
static void draw_vgradient(int x, int y, int w, int h, RGBA top, RGBA bottom) {
    for (int i = 0; i < h; i++) {
        float t = (float)i / (h > 1 ? (h - 1) : 1);
        fill_rect(x, y + i, w, 1, lerp_rgba(top, bottom, t));
    }
}

static void draw_text_ex(TTF_Font *font, int x, int y, const char *text, SDL_Color color) {
    if (!font || !text || !text[0]) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(g_renderer, surf);
    if (tex) {
        SDL_Rect dst = { x, y, surf->w, surf->h };
        SDL_RenderCopy(g_renderer, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

static void draw_text(int x, int y, const char *text, SDL_Color color) {
    draw_text_ex(g_font, x, y, text, color);
}
static void draw_text_small(int x, int y, const char *text, SDL_Color color) {
    draw_text_ex(g_font_small, x, y, text, color);
}

// Loads an image from a local extracted file (bare filename or a full
// sdmc: path — only the basename is used, matching how extract_zip()
// flattened everything) or a remote http(s) URL. Returns a texture, or
// NULL if unavailable/undecodable — callers treat that as "just skip it",
// never as a hard failure. If `out_surface` is non-NULL, the decoded
// surface is handed back too (caller must SDL_FreeSurface it) instead of
// being freed here — used for the background image, whose pixels the
// palette generator needs to read.
static SDL_Texture *load_theme_image_ex(const char *dest_dir, const char *ref, SDL_Surface **out_surface) {
    if (!ref || !ref[0]) return NULL;

    char *data = NULL;
    size_t size = 0;
    bool have_data = false;

    if (strncmp(ref, "http://", 7) == 0 || strncmp(ref, "https://", 8) == 0) {
        MemBuf buf;
        if (http_get(ref, &buf)) { data = buf.data; size = buf.size; have_data = true; }
    } else {
        const char *slash = strrchr(ref, '/');
        const char *basename = slash ? slash + 1 : ref;
        char local_path[500];
        snprintf(local_path, sizeof(local_path), "%s/%s", dest_dir, basename);
        have_data = read_file(local_path, &data, &size);
    }
    if (!have_data) return NULL;

    SDL_RWops *rw = SDL_RWFromConstMem(data, (int)size);
    SDL_Surface *surf = rw ? IMG_Load_RW(rw, 1) : NULL;
    SDL_Texture *tex = surf ? SDL_CreateTextureFromSurface(g_renderer, surf) : NULL;
    free(data);

    if (out_surface) {
        *out_surface = surf; // caller now owns it
    } else if (surf) {
        SDL_FreeSurface(surf);
    }
    return tex;
}

static SDL_Texture *load_theme_image(const char *dest_dir, const char *ref) {
    return load_theme_image_ex(dest_dir, ref, NULL);
}

// Defined further down with the other prompt screens; declared here
// because the preview needs it too.
static void wait_for_button_release(void);

// ---- Preview screen -----------------------------------------------------------
// Builds a rough mockup of the real Tinfoil layout — icon grid, a
// "selected" tile using the theme's actual selection colors, a border
// frame, and a progress bar — using the theme's real colors, background,
// and logo. Not pixel-identical to Tinfoil (this app doesn't have
// Tinfoil's actual layout code to reference), but enough to see whether a
// theme's palette and assets actually work together before installing it
// for real. Also lets you generate a palette from the background image
// (X) and apply it directly to the theme's colors, right here.
//
// Returns true if the user pressed A (keep), false for B (undo).
// `allowDelete` controls what B means. On a FRESH install, B is an undo:
// it removes the theme that was just downloaded. But when previewing a
// theme that was already on the SD card (the user declined to
// re-download), there is nothing to undo -- B there just means "go back",
// and deleting would destroy work the user explicitly chose to keep.
// That distinction cost a user their edited theme, so it's explicit now.
static bool show_theme_preview_and_confirm(const char *dest_dir, const char *theme_name, bool allowDelete) {
    char theme_json_path[350];
    snprintf(theme_json_path, sizeof(theme_json_path), "%s/settings.json", dest_dir);

    char *json = NULL;
    size_t json_len = 0;
    bool have_json = read_file(theme_json_path, &json, &json_len);
    if (!have_json) {
        // Fall back to theme.json -- covers folders left behind by an
        // earlier build of this installer, which wrote that name before
        // real-hardware testing showed Tinfoil actually reads
        // settings.json.
        snprintf(theme_json_path, sizeof(theme_json_path), "%s/theme.json", dest_dir);
        have_json = read_file(theme_json_path, &json, &json_len);
    }

    jsmntok_t tokens[512];
    int tokcount = 0;
    if (have_json) {
        jsmn_parser p;
        jsmn_init(&p);
        tokcount = jsmn_parse(&p, json, json_len, tokens, 512);
        if (tokcount < 1 || tokens[0].type != JSMN_OBJECT) tokcount = 0;
    }

    ThemeVisuals visuals;
    memset(&visuals, 0, sizeof(visuals));
    char bg_ref[URL_LEN] = "";
    bool has_bg_image = false;

    if (tokcount > 0) {
        parse_theme_visuals(json, tokens, &visuals);
        int bg_idx = json_object_find_child_index(json, tokens, 0, "background");
        if (bg_idx >= 0 && tokens[bg_idx].type == JSMN_OBJECT) {
            has_bg_image = json_object_get_string(json, tokens, bg_idx, "image", bg_ref, sizeof(bg_ref));
        }
    }

    // Background is loaded with its surface kept alive (not just a
    // texture) so the palette generator can read its actual pixels.
    SDL_Surface *bg_surface = NULL;
    SDL_Texture *bg_tex = has_bg_image ? load_theme_image_ex(dest_dir, bg_ref, &bg_surface) : NULL;
    SDL_Texture *logo_tex = visuals.has_logo ? load_theme_image(dest_dir, visuals.logo_ref) : NULL;

    RGBA bg_fallback     = visuals.has_bg_color ? visuals.bg_color : (RGBA){ 15, 28, 43, 255 };
    RGBA text_color      = visuals.has_text_color ? visuals.text_color : (RGBA){ 234, 241, 250, 255 };
    RGBA sel_color       = visuals.has_sel_color ? visuals.sel_color : (RGBA){ 255, 255, 255, 255 };
    RGBA sel_bg          = visuals.has_sel_bg ? visuals.sel_bg : (RGBA){ 255, 255, 255, 40 };
    RGBA sel_border      = visuals.has_sel_border ? visuals.sel_border : sel_color;
    RGBA border_color    = visuals.has_border ? visuals.border_color : (RGBA){ 255, 255, 255, 30 };
    RGBA progress_color  = visuals.has_progress_color ? visuals.progress_color : (RGBA){ 80, 200, 120, 255 };
    RGBA progress_bg     = visuals.has_progress_bg ? visuals.progress_bg : (RGBA){ 255, 255, 255, 25 };

    SDL_Color text_sdl = { text_color.r, text_color.g, text_color.b, 255 };
    SDL_Color hint_sdl = { 200, 210, 225, 255 };
    SDL_Color status_sdl = { 130, 225, 165, 255 };

    char paletteStatus[300] = ""; // long enough for a full sdmc: path -- 80 truncated it mid-filename
    int paletteStatusTimer = 0;
    int paletteSeed = 0;
    bool canGeneratePalette = (bg_surface != NULL && tokcount > 0);

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    bool keep = true;
    bool waiting = true;
    while (appletMainLoop() && waiting) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        if (kDown & HidNpadButton_A) { keep = true; waiting = false; }
        // B only discards on a fresh install. For an already-installed
        // theme there's nothing to undo, so B is simply "back" -- it must
        // never delete a theme the user chose to keep.
        if (kDown & HidNpadButton_B) { keep = allowDelete ? false : true; waiting = false; }

        if ((kDown & HidNpadButton_X) && canGeneratePalette) {
            RGBA palette[PALETTE_MAX_K];
            int targetK = count_distinct_theme_colors(json, tokens, tokcount);
            if (targetK < PALETTE_MIN_K) targetK = PALETTE_MIN_K;
            if (targetK > PALETTE_MAX_K) targetK = PALETTE_MAX_K;
            paletteSeed += 17;
            if (extract_palette_from_surface(bg_surface, paletteSeed, palette, targetK)) {
                int changed = apply_palette_to_json(json, tokens, palette);
                if (changed > 0) {
                    // Check the write -- silently ignoring a failure here
                    // makes a failed save look identical to a successful
                    // one, which is exactly the kind of thing that wastes
                    // an hour of debugging later.
                    bool wrote = write_file(theme_json_path, json, json_len);
                    // Token byte-ranges are still valid (apply_hex_inplace
                    // never changes buffer length), so re-deriving visuals
                    // from the same tokens against the now-edited buffer
                    // is enough — no need to re-parse.
                    parse_theme_visuals(json, tokens, &visuals);
                    if (visuals.has_bg_color) bg_fallback = visuals.bg_color;
                    if (visuals.has_text_color) text_color = visuals.text_color;
                    if (visuals.has_sel_color) sel_color = visuals.sel_color;
                    if (visuals.has_sel_bg) sel_bg = visuals.sel_bg;
                    sel_border = visuals.has_sel_border ? visuals.sel_border : sel_color;
                    if (visuals.has_border) border_color = visuals.border_color;
                    if (visuals.has_progress_color) progress_color = visuals.progress_color;
                    if (visuals.has_progress_bg) progress_bg = visuals.progress_bg;
                    text_sdl = (SDL_Color){ text_color.r, text_color.g, text_color.b, 255 };

                    if (wrote) {
                        // Don't trust write_file's return value alone --
                        // it already claimed success once while the file
                        // on the SD card stayed unchanged. Read the file
                        // back and compare against what we meant to
                        // write.
                        //
                        // NOTE: a read-back can still be served from the
                        // filesystem's own cache, so a match here does
                        // NOT prove the data reached the card. That's why
                        // the exact path is shown on screen and a
                        // separate proof file is written alongside it --
                        // between them we can tell "nothing was written"
                        // from "written to the wrong place" from "written
                        // correctly and reverted afterwards".
                        char *check = NULL;
                        size_t check_len = 0;
                        bool verified = false;
                        if (read_file(theme_json_path, &check, &check_len)) {
                            verified = (check_len == json_len) &&
                                       (memcmp(check, json, json_len) == 0);
                            free(check);
                        }

                        // Independent proof file: if this shows up on the
                        // SD card but settings.json is unchanged, the
                        // problem is specific to that file, not to
                        // writing in general.
                        char proof_path[400];
                        snprintf(proof_path, sizeof(proof_path), "%s/_athemeproof.txt", dest_dir);
                        char proof[600];
                        int plen = snprintf(proof, sizeof(proof),
                            "wrote: %s\nbytes: %u\nfirst color now: %.8s\nchanged fields: %d\n",
                            theme_json_path, (unsigned)json_len,
                            json_len > 12 ? json + 12 : json, changed);
                        write_file(proof_path, proof, (size_t)plen);

                        if (verified) {
                            snprintf(paletteStatus, sizeof(paletteStatus),
                                "Saved %d color%s to %s", changed,
                                changed == 1 ? "" : "s", theme_json_path);
                        } else {
                            snprintf(paletteStatus, sizeof(paletteStatus),
                                "WROTE %s but read-back does NOT match.", theme_json_path);
                        }
                    } else {
                        snprintf(paletteStatus, sizeof(paletteStatus),
                            "Palette shown here, but COULD NOT SAVE to the theme file.");
                    }
                } else {
                    snprintf(paletteStatus, sizeof(paletteStatus), "No color fields found to update.");
                }
            } else {
                snprintf(paletteStatus, sizeof(paletteStatus), "Could not extract a palette from this background.");
            }
            paletteStatusTimer = 200;
        }

        SDL_SetRenderDrawColor(g_renderer, bg_fallback.r, bg_fallback.g, bg_fallback.b, 255);
        SDL_RenderClear(g_renderer);
        if (bg_tex) {
            SDL_RenderCopy(g_renderer, bg_tex, NULL, NULL); // stretch to fill the screen
        }

        // Dim overlay so the mockup UI stays legible regardless of the
        // background image's own brightness/contrast.
        fill_rect(0, 0, SCREEN_W, SCREEN_H, (RGBA){ 0, 0, 0, 70 });

        if (logo_tex) {
            int lw = 0, lh = 0;
            SDL_QueryTexture(logo_tex, NULL, NULL, &lw, &lh);
            if (lh > 0) {
                float s = 80.0f / lh;
                SDL_Rect dst = { 40, 30, (int)(lw * s), 80 };
                SDL_RenderCopy(g_renderer, logo_tex, NULL, &dst);
            }
        }

        int gridX = 40, gridY = 150, gridW = SCREEN_W - 80, gridH = 420;
        draw_rect_outline(gridX, gridY, gridW, gridH, border_color, 2);

        int tileW = 170, tileH = 170, gap = 20, tilesPerRow = 6;
        for (int i = 0; i < 12; i++) {
            int col = i % tilesPerRow;
            int row = i / tilesPerRow;
            int tx = gridX + 20 + col * (tileW + gap);
            int ty = gridY + 20 + row * (tileH + gap);
            if (i == 4) { // one tile shown "selected", using the theme's real selection colors
                fill_rect(tx, ty, tileW, tileH, sel_bg);
                draw_rect_outline(tx, ty, tileW, tileH, sel_border, 3);
            } else {
                fill_rect(tx, ty, tileW, tileH, (RGBA){ 255, 255, 255, 18 });
            }
        }

        int pbX = 40, pbY = 600, pbW = SCREEN_W - 80, pbH = 14;
        fill_rect(pbX, pbY, pbW, pbH, progress_bg);
        fill_rect(pbX, pbY, (int)(pbW * 0.64f), pbH, progress_color);

        draw_text(40, 630, theme_name, text_sdl);
        if (paletteStatusTimer > 0) {
            draw_text_small(40, 668, paletteStatus, status_sdl);
            paletteStatusTimer--;
        } else if (canGeneratePalette) {
            draw_text_small(40, 668, allowDelete
                ? "A = Keep    B = Discard this download    X = New palette from background"
                : "A = Done    B = Back    X = New palette from background", hint_sdl);
        } else {
            draw_text_small(40, 668, allowDelete
                ? "A = Keep this theme        B = Discard this download"
                : "A = Done        B = Back", hint_sdl);
        }

        SDL_RenderPresent(g_renderer);
    }

    if (bg_tex) SDL_DestroyTexture(bg_tex);
    if (bg_surface) SDL_FreeSurface(bg_surface);
    if (logo_tex) SDL_DestroyTexture(logo_tex);
    if (json) free(json);

    wait_for_button_release();
    return keep;
}

// ---- Menu screen --------------------------------------------------------------

// A simple yes/no prompt drawn over the current screen. Returns true for
// A (yes), false for B (no). Used to ask before touching Tinfoil's
// options.json, since that file holds credentials and shouldn't be
// modified without the user explicitly saying so.
// Blocks until no face buttons are held, then drains one more frame.
//
// Without this, chained screens steal each other's input: confirm_prompt
// returns the instant B goes down, the next screen's loop starts while B
// is STILL physically held, and reads it as a fresh press. That caused
// real data loss -- answering "No" to the reinstall prompt immediately
// registered as "B = remove theme" on the preview screen and deleted the
// very theme the user was trying to keep.
static void wait_for_button_release(void) {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    const u64 watched = HidNpadButton_A | HidNpadButton_B | HidNpadButton_X |
                        HidNpadButton_Y | HidNpadButton_Plus;
    int settled = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtons(&pad) & watched) {
            settled = 0;
        } else if (++settled >= 2) { // two clean frames, guards against bounce
            break;
        }
        // Deliberately does NOT present here: the back buffer holds
        // whatever was drawn two frames ago on a double-buffered display,
        // so presenting again would flicker between stale frames. The
        // last presented frame simply stays on screen while we wait.
        svcSleepThread(8000000ULL); // ~8ms, keeps this from spinning hot
    }
}

static bool confirm_prompt(const char *line1, const char *line2, const char *line3) {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    SDL_Color white  = { 234, 241, 250, 255 };
    SDL_Color dim    = { 150, 168, 190, 255 };
    SDL_Color accent = { 60, 200, 255, 255 };

    bool result = false;
    bool waiting = true;
    while (appletMainLoop() && waiting) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        if (kDown & HidNpadButton_A) { result = true; waiting = false; }
        if (kDown & HidNpadButton_B) { result = false; waiting = false; }

        // Redraw a base each frame -- a semi-transparent overlay drawn
        // repeatedly over its own previous output compounds and fades to
        // black, so the backdrop has to be re-established every frame.
        draw_vgradient(0, 0, SCREEN_W, SCREEN_H, (RGBA){ 14, 25, 39, 255 }, BG_DEEP);
        draw_brand_gradient(0, 0, SCREEN_W, 4);
        fill_rect(0, 0, SCREEN_W, SCREEN_H, (RGBA){ 6, 12, 20, 170 });

        int boxW = 900, boxH = 280;
        int boxX = (SCREEN_W - boxW) / 2, boxY = (SCREEN_H - boxH) / 2;

        draw_vgradient(boxX, boxY, boxW, boxH, (RGBA){ 24, 40, 60, 255 }, BG_PANEL);
        draw_brand_gradient(boxX, boxY, boxW, 3);
        draw_rect_outline(boxX, boxY, boxW, boxH, (RGBA){ 60, 200, 255, 140 }, 1);

        draw_text(boxX + 38, boxY + 44, line1, white);
        if (line2 && line2[0]) draw_text_small(boxX + 38, boxY + 104, line2, dim);
        if (line3 && line3[0]) draw_text_small(boxX + 38, boxY + 134, line3, dim);

        // button hints, visually separated from the message
        fill_rect(boxX + 38, boxY + boxH - 68, boxW - 76, 1, (RGBA){ 50, 74, 100, 255 });
        draw_text_small(boxX + 38, boxY + boxH - 48, "A = Yes", accent);
        draw_text_small(boxX + 150, boxY + boxH - 48, "B = No", dim);

        SDL_RenderPresent(g_renderer);
    }
    // Don't hand control to the next screen while the button is still
    // physically down -- it would read it as a fresh press. See
    // wait_for_button_release().
    wait_for_button_release();
    return result;
}

// A branded loading screen shown while the manifest downloads. Takes a
// frame counter so the progress bar animates rather than sitting frozen
// during a slow network fetch.
static void draw_splash(const char *message, int frame) {
    draw_vgradient(0, 0, SCREEN_W, SCREEN_H, (RGBA){ 16, 28, 44, 255 }, BG_DEEP);
    draw_brand_gradient(0, 0, SCREEN_W, 4);
    draw_brand_gradient(0, SCREEN_H - 4, SCREEN_W, 4);

    SDL_Color white = { 234, 241, 250, 255 };
    SDL_Color dim   = { 130, 150, 175, 255 };

    if (g_logo_large) {
        int size = 200;
        SDL_Rect dst = { (SCREEN_W - size) / 2, 160, size, size };
        SDL_RenderCopy(g_renderer, g_logo_large, NULL, &dst);
    }

    draw_text(SCREEN_W / 2 - 135, 410, "A-Theme Installer", white);
    draw_text_small(SCREEN_W / 2 - 110, 452, message, dim);

    // Indeterminate progress bar -- a gradient block sliding back and
    // forth, since the manifest's size isn't known ahead of time.
    int barW = 420, barH = 6;
    int barX = (SCREEN_W - barW) / 2, barY = 505;
    fill_rect(barX, barY, barW, barH, (RGBA){ 34, 52, 74, 255 });

    int chunk = 130;
    int span = barW - chunk;
    if (span < 1) span = 1;
    int pos = frame % (span * 2);
    if (pos > span) pos = span * 2 - pos; // bounce back at the ends
    draw_brand_gradient(barX + pos, barY, chunk, barH);

    SDL_RenderPresent(g_renderer);
}

static void draw_menu(Theme *themes, int count, int cursor, int scroll, const char *status) {
    // Subtle vertical fade rather than a flat fill -- gives the screen
    // some depth on a large TV without being distracting.
    draw_vgradient(0, 0, SCREEN_W, SCREEN_H, (RGBA){ 14, 25, 39, 255 }, BG_DEEP);

    SDL_Color white  = { 234, 241, 250, 255 };
    SDL_Color dim    = { 120, 138, 158, 255 };
    SDL_Color accent = { 60, 200, 255, 255 };
    SDL_Color tagcol = { 150, 120, 210, 255 };

    // --- header band, with the brand gradient as a top edge ---
    draw_vgradient(0, 0, SCREEN_W, 104, (RGBA){ 20, 34, 52, 255 }, (RGBA){ 13, 23, 36, 255 });
    draw_brand_gradient(0, 0, SCREEN_W, 4);

    // Logo, if it loaded. Text shifts right to make room; if the logo is
    // missing the layout falls back to the original left margin.
    int textX = 40;
    if (g_logo && g_logo_h > 0) {
        int lh = 64;
        int lw = (int)((float)g_logo_w / g_logo_h * lh);
        SDL_Rect dst = { 34, 18, lw, lh };
        SDL_RenderCopy(g_renderer, g_logo, NULL, &dst);
        textX = 34 + lw + 20;
    }

    draw_text(textX, 24, "A-Theme Installer", white);
    draw_text_small(textX + 2, 62, "Tinfoil theme downloader", dim);

    // theme count, right-aligned in the header
    if (count > 0) {
        char cbuf[48];
        snprintf(cbuf, sizeof(cbuf), "%d themes", count);
        draw_text_small(SCREEN_W - 150, 62, cbuf, dim);
    }

    if (count == 0) {
        draw_text(40, 150, "No themes found in the manifest.", dim);
    } else {
        int listTop = 118;
        int rowH = 34;
        int end = scroll + PAGE_SIZE;
        if (end > count) end = count;

        int y = listTop;
        for (int i = scroll; i < end; i++) {
            bool isSel = (i == cursor);
            bool isCommunity = (strncmp(themes[i].name, "[Community]", 11) == 0);
            const char *label = isCommunity ? themes[i].name + 12 : themes[i].name;

            if (isSel) {
                // Layered highlight: a wide soft glow, a brighter core,
                // then a solid accent bar on the leading edge. Cheap to
                // draw and reads much better on a TV than a flat tint.
                fill_rect(26, y - 8, SCREEN_W - 52, rowH + 6, (RGBA){ 0, 194, 255, 14 });
                fill_rect(30, y - 5, SCREEN_W - 60, rowH, (RGBA){ 0, 194, 255, 32 });
                fill_rect(30, y - 5, 4, rowH, BRAND_BLUE);
                draw_text(52, y, label, accent);
            } else {
                draw_text(52, y, label, white);
            }

            // community themes get a small tag on the right
            if (isCommunity) {
                draw_text_small(SCREEN_W - 190, y + 5, "community", tagcol);
            }
            y += rowH;
        }

        // --- scrollbar, showing where you are in the full list ---
        if (count > PAGE_SIZE) {
            int trackX = SCREEN_W - 46;
            int trackY = listTop - 4;
            int trackH = PAGE_SIZE * rowH;
            fill_rect(trackX, trackY, 4, trackH, (RGBA){ 40, 60, 85, 255 });

            int thumbH = (int)((float)PAGE_SIZE / count * trackH);
            if (thumbH < 24) thumbH = 24;
            int maxScroll = count - PAGE_SIZE;
            float prog = maxScroll > 0 ? (float)scroll / maxScroll : 0.0f;
            int thumbY = trackY + (int)((trackH - thumbH) * prog);
            fill_rect(trackX, thumbY, 4, thumbH, BRAND_BLUE);

            char buf[32];
            snprintf(buf, sizeof(buf), "%d / %d", cursor + 1, count);
            draw_text_small(40, trackY + trackH + 10, buf, dim);
        }
    }

    // --- footer ---
    int footY = SCREEN_H - 78;
    draw_vgradient(0, footY, SCREEN_W, SCREEN_H - footY, (RGBA){ 13, 23, 36, 255 }, (RGBA){ 18, 30, 46, 255 });
    fill_rect(0, footY, SCREEN_W, 1, (RGBA){ 40, 60, 85, 255 });

    if (status && status[0]) draw_text_small(40, footY + 12, status, accent);
    draw_text_small(40, SCREEN_H - 32,
        "A = Install     Y = Preview / edit     + = Exit", dim);

    draw_brand_gradient(0, SCREEN_H - 3, SCREEN_W, 3);

    SDL_RenderPresent(g_renderer);
}

// ---- Graphics init/shutdown ---------------------------------------------------

static bool init_graphics(char *err, size_t errcap) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        snprintf(err, errcap, "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    if (TTF_Init() != 0) {
        snprintf(err, errcap, "TTF_Init failed: %s", TTF_GetError());
        return false;
    }
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);

    if (SDL_CreateWindowAndRenderer(SCREEN_W, SCREEN_H, 0, &g_window, &g_renderer) != 0) {
        snprintf(err, errcap, "SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);

    // Use Nintendo's own shared system font rather than bundling one — no
    // extra file to ship, and it matches what the OS already renders
    // elsewhere.
    Result rc = plInitialize(PlServiceType_User);
    if (R_FAILED(rc)) {
        snprintf(err, errcap, "plInitialize failed: 0x%x", rc);
        return false;
    }
    static PlFontData fontData;
    rc = plGetSharedFontByType(&fontData, PlSharedFontType_Standard);
    if (R_FAILED(rc)) {
        snprintf(err, errcap, "plGetSharedFontByType failed: 0x%x", rc);
        return false;
    }

    g_font = TTF_OpenFontRW(SDL_RWFromConstMem(fontData.address, fontData.size), 0, 26);
    g_font_small = TTF_OpenFontRW(SDL_RWFromConstMem(fontData.address, fontData.size), 0, 18);
    if (!g_font || !g_font_small) {
        snprintf(err, errcap, "TTF_OpenFontRW failed: %s", TTF_GetError());
        return false;
    }

    // Logos come from romfs, so they're baked into the .nro itself.
    // Deliberately non-fatal: if either fails to load the app still runs
    // perfectly, it just draws a text-only header. A missing decoration
    // should never stop someone installing themes.
    g_logo = IMG_LoadTexture(g_renderer, "romfs:/logo.png");
    if (g_logo) SDL_QueryTexture(g_logo, NULL, NULL, &g_logo_w, &g_logo_h);
    g_logo_large = IMG_LoadTexture(g_renderer, "romfs:/logo_large.png");

    return true;
}

static void shutdown_graphics(void) {
    if (g_logo) SDL_DestroyTexture(g_logo);
    if (g_logo_large) SDL_DestroyTexture(g_logo_large);
    if (g_font) TTF_CloseFont(g_font);
    if (g_font_small) TTF_CloseFont(g_font_small);
    plExit();
    TTF_Quit();
    IMG_Quit();
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
}

// ---- Entry point ------------------------------------------------------------

int main(int argc, char **argv) {
    romfsInit(); // mounts the bundled cacert.pem for TLS verification
    socketInitializeDefault();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    char graphics_err[160] = "";
    if (!init_graphics(graphics_err, sizeof(graphics_err))) {
        // Nothing to render without graphics — nowhere good to show this
        // error, so there's genuinely nothing more this app can do.
        shutdown_graphics();
        curl_global_cleanup();
        socketExit();
        romfsExit();
        return 1;
    }

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    // Show the splash for a few frames so the logo actually registers
    // before the (blocking) manifest download begins.
    for (int f = 0; f < 30; f++) {
        draw_splash("Fetching theme list...", f * 4);
        svcSleepThread(16000000ULL); // ~16ms, roughly one frame
    }

    Theme *themes = malloc(sizeof(Theme) * MAX_THEMES);
    int themeCount = 0;

    MemBuf manifest;
    long http_code = 0;
    bool gotManifest = http_get_ex(MANIFEST_URL, &manifest, &http_code);
    bool parsedOk = false;
    if (gotManifest) {
        parsedOk = parse_manifest(manifest.data, manifest.size, themes, &themeCount);
        free(manifest.data);
    }

    if (!gotManifest || !parsedOk || themeCount == 0) {
        char msg[200];
        if (gotManifest && !parsedOk) {
            snprintf(msg, sizeof(msg), "Manifest format not recognized. Press + to exit.");
        } else if (!gotManifest && http_code != 0) {
            snprintf(msg, sizeof(msg), "Server error %ld fetching theme list. Press + to exit.", http_code);
        } else {
            snprintf(msg, sizeof(msg), "Could not connect. Check Wi-Fi, then press + to exit.");
        }

        bool waiting = true;
        while (appletMainLoop() && waiting) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) waiting = false;
            draw_menu(NULL, 0, 0, 0, msg);
        }
    } else {
        int cursor = 0, scroll = 0, statusTimer = 0;
        char statusMsg[220] = "";
        bool running = true;

        while (appletMainLoop() && running) {
            padUpdate(&pad);
            u64 kDown = padGetButtonsDown(&pad);

            if (kDown & HidNpadButton_Plus) running = false;

            if (kDown & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
                cursor = (cursor + 1) % themeCount;
            }
            if (kDown & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
                cursor = (cursor - 1 + themeCount) % themeCount;
            }

            if (kDown & (HidNpadButton_A | HidNpadButton_Y)) {
                bool wantsPreview = (kDown & HidNpadButton_Y) != 0;
                Theme *t = &themes[cursor];

                char dest_dir[300];
                snprintf(dest_dir, sizeof(dest_dir), THEMES_ROOT "%s", t->folder);

                // Is this theme already on the SD card? Re-extracting over
                // an existing install silently destroys any palette edits
                // made here previously -- the zip's original settings.json
                // overwrites the edited one. That cost several rounds of
                // debugging: the palette saves were working correctly every
                // time, and the NEXT install was wiping them.
                char existing_cfg[400];
                snprintf(existing_cfg, sizeof(existing_cfg), "%s/settings.json", dest_dir);
                char *existing = NULL;
                size_t existing_len = 0;
                bool already_installed = read_file(existing_cfg, &existing, &existing_len);
                if (existing) free(existing);

                bool ok = true;
                bool skipped_download = false;

                if (already_installed) {
                    bool redownload = confirm_prompt(
                        "This theme is already installed.",
                        "Download a fresh copy? That REPLACES it, discarding any palette",
                        "changes you saved here. Choose No to keep what's on your SD card.");
                    if (redownload) {
                        snprintf(statusMsg, sizeof(statusMsg), "Reinstalling \"%s\"...", t->name);
                        draw_menu(themes, themeCount, cursor, scroll, statusMsg);
                        char detail[128] = "";
                        ok = install_theme(t, detail, sizeof(detail));
                        if (!ok) snprintf(statusMsg, sizeof(statusMsg), "Failed: %s -- \"%s\"", detail, t->name);
                    } else {
                        skipped_download = true;
                    }
                } else {
                    snprintf(statusMsg, sizeof(statusMsg), "Installing \"%s\"...", t->name);
                    draw_menu(themes, themeCount, cursor, scroll, statusMsg);
                    char detail[128] = "";
                    ok = install_theme(t, detail, sizeof(detail));
                    if (!ok) snprintf(statusMsg, sizeof(statusMsg), "Failed: %s -- \"%s\"", detail, t->name);
                }

                if (ok) {
                    bool keep = true;
                    if (wantsPreview) {
                        // Deleting is only offered as an undo for a
                        // download that just happened. If we kept the
                        // user's existing copy, B must not destroy it.
                        keep = show_theme_preview_and_confirm(dest_dir, t->name, !skipped_download);
                        if (!keep) {
                            remove_dir_recursive(dest_dir);
                            snprintf(statusMsg, sizeof(statusMsg), "Removed \"%s\".", t->name);
                        }
                    }

                    if (keep) {
                        // Offer to make it the active theme. This edits
                        // Tinfoil's own options.json, which also holds
                        // credentials -- so it's always asked, never
                        // assumed, and a backup is written first.
                        char prompt_line[220];
                        snprintf(prompt_line, sizeof(prompt_line), "Make \"%s\" your active theme?", t->name);
                        bool setActive = confirm_prompt(
                            prompt_line,
                            "This updates Tinfoil's own settings so it loads next time you open it.",
                            "Close Tinfoil first, or it may overwrite this when it exits.");

                        if (setActive) {
                            char err[160] = "";
                            if (set_active_theme(t->folder, err, sizeof(err))) {
                                snprintf(statusMsg, sizeof(statusMsg),
                                    "%s \"%s\" and set it active -- restart Tinfoil to see it.",
                                    skipped_download ? "Kept your existing" : "Installed", t->name);
                            } else {
                                snprintf(statusMsg, sizeof(statusMsg),
                                    "\"%s\" ready, but couldn't set it active: %s", t->name, err);
                            }
                        } else {
                            snprintf(statusMsg, sizeof(statusMsg),
                                "%s \"%s\" -- pick it in Tinfoil's theme settings.",
                                skipped_download ? "Kept your existing" : "Installed", t->name);
                        }
                    }
                }
                statusTimer = 240;
            }

            if (cursor < scroll) scroll = cursor;
            if (cursor >= scroll + PAGE_SIZE) scroll = cursor - PAGE_SIZE + 1;

            if (statusTimer > 0) {
                statusTimer--;
                if (statusTimer == 0) statusMsg[0] = '\0';
            }

            draw_menu(themes, themeCount, cursor, scroll, statusMsg);
        }
    }

    free(themes);
    shutdown_graphics();
    curl_global_cleanup();
    socketExit();
    romfsExit();
    return 0;
}
