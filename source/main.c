// A-Theme Installer — a Switch homebrew (.nro) app that reads A-Theme's own
// themes.json manifest and installs the selected theme's zip archive
// directly onto the SD card, ready for Tinfoil to pick up. Also supports
// previewing a theme's background image/logo before committing to it.
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
// switch-zlib / switch-zziplib / switch-sdl2 / switch-sdl2_image.
// See BUILD.md in this folder.
//
// --- On the bundled/pulled-in dependencies, and what's actually verified ---
// include/jsmn.h   — real, unmodified JSON tokenizer (MIT, zserge/jsmn).
//                     ALL of this file's JSON-parsing logic (manifest
//                     parsing, and the nested background.image/logo field
//                     lookup used for previews) was compiled and run for
//                     real on a regular PC against actual sample content
//                     from the A-Theme project before being written here.
//                     See json_object_get_string() / parse_manifest().
// switch-zziplib   — existing devkitPro package, reads the downloaded
//                     .zip theme archives. Switch-specific; could not be
//                     compile-tested in the environment this was written
//                     in.
// switch-sdl2,
// switch-sdl2_image — existing devkitPro packages, used ONLY for the
//                     preview screen (decoding + displaying the theme's
//                     background image/logo). This is the single riskiest
//                     part of this whole app: it means briefly tearing
//                     down libnx's text console and standing up SDL2's
//                     video system in its place, then tearing THAT down
//                     and reinitializing the console again afterward.
//                     That specific console<->SDL2 handoff is genuinely
//                     unverified — see BUILD.md's troubleshooting section,
//                     which flags this as the most likely spot to need
//                     real debugging on real hardware.

#include <switch.h>
#include <curl/curl.h>
#include <zzip/zzip.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> // strcasecmp
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h> // rmdir()
#include <stdbool.h>

#define JSMN_STATIC
#include "jsmn.h"

// ---- Configuration -------------------------------------------------------

// The A-Theme project's own existing manifest — same file already used
// elsewhere in the project. Nothing new needs to be created or maintained
// for this app to work; it reads what's already there.
#define MANIFEST_URL "https://raw.githubusercontent.com/A-Theme/Tinfoil-Themes/main/themes.json"

// Where installed themes get written on the SD card (Tinfoil's own layout).
#define THEMES_ROOT "sdmc:/switch/tinfoil/themes/"

// Scratch space for this app's own temp files (the downloaded zip, briefly,
// before it's extracted and deleted).
#define APP_DATA_DIR "sdmc:/switch/a-theme-installer/"
#define TEMP_ZIP_PATH APP_DATA_DIR "_download.zip"

#define MAX_THEMES   400
#define PAGE_SIZE    14
#define NAME_LEN     110
#define URL_LEN      220
#define FOLDER_LEN   96
#define MAX_TOKENS   (MAX_THEMES * 2 + 16) // themes array entries + a little headroom

typedef struct {
    char name[NAME_LEN];      // display name shown in the menu
    char folder[FOLDER_LEN];  // destination folder name on the SD card
    char url[URL_LEN];        // full https URL to the theme's .zip
} Theme;

// Growable buffer used for all curl downloads.
typedef struct {
    char *data;
    size_t size;
} MemBuf;

// ---- Networking helpers ---------------------------------------------------

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemBuf *mem = (MemBuf *)userp;
    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0; // out of memory — abort this transfer
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L); // theme zips can be a few MB
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

static bool write_file(const char *path, const char *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    return written == size;
}

// Reads an entire file into a malloc'd buffer. Caller frees. Returns false
// (and doesn't touch *out_data) if the file can't be opened.
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

// Recursively deletes a folder and everything in it. Used to undo an
// install if the user rejects a theme after previewing it. Our own
// extracted theme folders are always flat (no subfolders), but this
// handles nested content gracefully too, just in case.
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

// ---- JSON helpers (shared by manifest parsing and theme.json field lookup) --
// This logic was compiled standalone and run against real manifest AND real
// theme.json content before being written here — see the file header.

static int json_streq(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
        strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return 1;
    }
    return 0;
}

// Returns the token index immediately after the full subtree rooted at
// tokens[idx] — needed to correctly skip over nested objects/arrays while
// walking a flat jsmn token stream.
static int skip_token_subtree(jsmntok_t *tokens, int idx) {
    jsmntok_t *t = &tokens[idx];
    int end = idx + 1;
    if (t->type == JSMN_OBJECT) {
        for (int n = 0; n < t->size; n++) {
            end = skip_token_subtree(tokens, end); // key
            end = skip_token_subtree(tokens, end); // value
        }
    } else if (t->type == JSMN_ARRAY) {
        for (int n = 0; n < t->size; n++) {
            end = skip_token_subtree(tokens, end);
        }
    }
    return end;
}

// Finds the token index of the value for `key` within the object at
// tokens[obj_idx] (immediate children only, not nested deeper).
static int json_object_find_child_index(const char *json, jsmntok_t *tokens, int obj_idx, const char *key) {
    jsmntok_t *obj = &tokens[obj_idx];
    if (obj->type != JSMN_OBJECT) return -1;
    int i = obj_idx + 1;
    for (int n = 0; n < obj->size; n++) {
        int key_idx = i;
        int val_idx = key_idx + 1; // JSON object keys are always plain strings
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

// Turns a theme's zip URL into a readable display name and a filesystem-safe
// folder name. E.g. ".../Non_Aramaki_Themes/hbg_crysis.zip?" becomes display
// name "[Community] hbg crysis" and folder "hbg_crysis".
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
        if (nlen == 0 || entry_name[nlen - 1] == '/') continue; // directory entry

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
            // A-Theme's zips package the config file as "settings.json" (real
            // theme files from this project have always used that name), but
            // Tinfoil itself reads "theme.json" from the SD card. Normalize
            // whichever one we find to the name Tinfoil actually expects —
            // everything else keeps its original filename.
            bool is_config = (strcasecmp(base, "theme.json") == 0 ||
                               strcasecmp(base, "settings.json") == 0);
            const char *dest_name = is_config ? "theme.json" : base;

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
        snprintf(status_detail, status_cap, "Installed, but no theme.json/settings.json found inside the zip");
        return false;
    }

    return true;
}

// ---- Preview image lookup ----------------------------------------------------

// After a theme is installed, figures out what image to preview: prefers
// background.image, falls back to logo. If that reference is a local
// filename (already sitting in the theme's folder from extraction), reads
// it straight off the SD card. If it's a remote http(s) URL, downloads it.
// Returns the raw, still-encoded (PNG/JPG) image bytes on success.
static bool find_preview_image_bytes(const char *dest_dir, char **out_data, size_t *out_size) {
    char theme_json_path[350];
    snprintf(theme_json_path, sizeof(theme_json_path), "%s/theme.json", dest_dir);

    char *json = NULL;
    size_t json_len = 0;
    if (!read_file(theme_json_path, &json, &json_len)) return false;

    jsmntok_t tokens[512];
    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, json, json_len, tokens, 512);
    if (r < 1 || tokens[0].type != JSMN_OBJECT) { free(json); return false; }

    char image_ref[URL_LEN] = "";
    bool found = false;

    int bg_idx = json_object_find_child_index(json, tokens, 0, "background");
    if (bg_idx >= 0 && tokens[bg_idx].type == JSMN_OBJECT) {
        found = json_object_get_string(json, tokens, bg_idx, "image", image_ref, sizeof(image_ref));
    }
    if (!found) {
        found = json_object_get_string(json, tokens, 0, "logo", image_ref, sizeof(image_ref));
    }
    free(json);
    if (!found || image_ref[0] == '\0') return false;

    if (strncmp(image_ref, "http://", 7) == 0 || strncmp(image_ref, "https://", 8) == 0) {
        MemBuf buf;
        if (!http_get(image_ref, &buf)) return false;
        *out_data = buf.data;
        *out_size = buf.size;
        return true;
    }

    // Local reference — could be a bare filename or a full sdmc: path from
    // the original theme.json; either way, what matters is the basename,
    // since that's what extract_zip() flattened everything to.
    const char *slash = strrchr(image_ref, '/');
    const char *basename = slash ? slash + 1 : image_ref;
    char local_path[500];
    snprintf(local_path, sizeof(local_path), "%s/%s", dest_dir, basename);

    char *data = NULL;
    size_t size = 0;
    if (!read_file(local_path, &data, &size)) return false;
    *out_data = data;
    *out_size = size;
    return true;
}

// ---- Preview screen (SDL2 / SDL2_image) --------------------------------------
// The riskiest part of this app — see the file header and BUILD.md.
// Tears down the libnx text console, shows the decoded image fullscreen via
// SDL2, waits for A (keep) or B (undo), then restores the console.
//
// Returns true if the user chose to KEEP the theme, false to undo it.
static bool show_preview_and_confirm(const char *image_data, size_t image_size, const char *theme_name) {
    consoleExit(NULL);

    bool keep = true; // if SDL fails to even start, default to keeping the
                       // install rather than punishing the user for a
                       // preview-only failure — the theme itself is fine.

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        consoleInit(NULL);
        printf("Preview unavailable (SDL init failed): %s\n", SDL_GetError());
        printf("The theme is still installed. Press A to continue.\n");
        consoleUpdate(NULL);
        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_A) break;
            consoleUpdate(NULL);
        }
        return true;
    }

    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_CreateWindowAndRenderer(1280, 720, 0, &window, &renderer);

    SDL_RWops *rw = SDL_RWFromConstMem(image_data, (int)image_size);
    SDL_Surface *surface = rw ? IMG_Load_RW(rw, 1) : NULL;
    SDL_Texture *texture = surface ? SDL_CreateTextureFromSurface(renderer, surface) : NULL;

    if (!texture) {
        // Couldn't decode the image — don't punish the install for it,
        // just skip straight to "keep" and let the user know via console
        // afterward.
        keep = true;
    } else {
        int iw = surface->w, ih = surface->h;
        float scale = 1280.0f / iw;
        if (ih * scale > 720.0f) scale = 720.0f / ih;
        int dw = (int)(iw * scale), dh = (int)(ih * scale);
        SDL_Rect dst = { (1280 - dw) / 2, (720 - dh) / 2, dw, dh };

        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);

        bool waiting = true;
        while (appletMainLoop() && waiting) {
            padUpdate(&pad);
            u64 kDown = padGetButtonsDown(&pad);
            if (kDown & HidNpadButton_A) { keep = true; waiting = false; }
            if (kDown & HidNpadButton_B) { keep = false; waiting = false; }

            SDL_SetRenderDrawColor(renderer, 11, 20, 32, 255); // matches the web editor's dark bg
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, NULL, &dst);
            SDL_RenderPresent(renderer);
        }
    }

    if (texture) SDL_DestroyTexture(texture);
    if (surface) SDL_FreeSurface(surface);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    IMG_Quit();
    SDL_Quit();

    consoleInit(NULL);
    return keep;
}

// ---- UI -----------------------------------------------------------------------

static void render_menu(Theme *themes, int count, int cursor, int scroll, const char *status) {
    printf("\x1b[1;1H\x1b[2J"); // clear screen
    printf("\x1b[36;1mA-Theme Installer\x1b[0m - Tinfoil theme downloader\n");
    printf("--------------------------------------------------------\n");

    if (count == 0) {
        printf("\nNo themes found in the manifest.\n");
    } else {
        int end = scroll + PAGE_SIZE;
        if (end > count) end = count;
        for (int i = scroll; i < end; i++) {
            printf("%s %s\n", (i == cursor) ? "\x1b[33;1m>" : " ", themes[i].name);
        }
        if (count > PAGE_SIZE) {
            printf("\x1b[0m\n(%d/%d)\n", cursor + 1, count);
        }
    }

    printf("\x1b[0m\n--------------------------------------------------------\n");
    if (status[0]) printf("%s\n", status);
    printf("A = Install    Y = Preview then install    + = Exit\n");
}

// ---- Entry point ------------------------------------------------------------

int main(int argc, char **argv) {
    consoleInit(NULL);
    romfsInit(); // mounts the bundled cacert.pem for TLS verification

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    socketInitializeDefault();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    printf("A-Theme Installer\nFetching theme list...\n");
    consoleUpdate(NULL);

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
        printf("\nCould not load the theme list.\n");
        if (gotManifest && !parsedOk) {
            printf("Connected fine, but couldn't find a \"themes\" list in\n"
                   "the manifest -- it may have changed format.\n");
        } else if (!gotManifest && http_code != 0) {
            printf("Server responded with HTTP %ld while fetching:\n%s\n", http_code, MANIFEST_URL);
        } else {
            printf("Check that your Switch is connected to Wi-Fi.\n");
        }
        printf("\nPress + to exit.\n");
        consoleUpdate(NULL);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);
        }
    } else {
        int cursor = 0, scroll = 0, statusTimer = 0;
        char statusMsg[220] = "";
        bool running = true;

        render_menu(themes, themeCount, cursor, scroll, statusMsg);
        consoleUpdate(NULL);

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

                snprintf(statusMsg, sizeof(statusMsg), "Installing \"%s\"...", t->name);
                render_menu(themes, themeCount, cursor, scroll, statusMsg);
                consoleUpdate(NULL);

                char detail[128] = "";
                bool ok = install_theme(t, detail, sizeof(detail));

                if (!ok) {
                    snprintf(statusMsg, sizeof(statusMsg), "Failed: %s -- \"%s\"", detail, t->name);
                } else if (!wantsPreview) {
                    snprintf(statusMsg, sizeof(statusMsg),
                        "Installed \"%s\" -- pick it in Tinfoil's theme settings.", t->name);
                } else {
                    char dest_dir[300];
                    snprintf(dest_dir, sizeof(dest_dir), THEMES_ROOT "%s", t->folder);

                    char *img_data = NULL;
                    size_t img_size = 0;
                    if (find_preview_image_bytes(dest_dir, &img_data, &img_size)) {
                        bool keep = show_preview_and_confirm(img_data, img_size, t->name);
                        free(img_data);

                        if (keep) {
                            snprintf(statusMsg, sizeof(statusMsg),
                                "Installed \"%s\" -- pick it in Tinfoil's theme settings.", t->name);
                        } else {
                            remove_dir_recursive(dest_dir);
                            snprintf(statusMsg, sizeof(statusMsg), "Removed \"%s\".", t->name);
                        }
                    } else {
                        // Installed fine, just nothing to preview (no
                        // background.image or logo field, or it couldn't
                        // be fetched/decoded) — leave it installed.
                        snprintf(statusMsg, sizeof(statusMsg),
                            "Installed \"%s\" (no preview available) -- pick it in Tinfoil's theme settings.", t->name);
                    }

                    render_menu(themes, themeCount, cursor, scroll, statusMsg);
                    consoleUpdate(NULL);
                }
                statusTimer = 240; // ~4 seconds at 60fps
            }

            if (cursor < scroll) scroll = cursor;
            if (cursor >= scroll + PAGE_SIZE) scroll = cursor - PAGE_SIZE + 1;

            if (statusTimer > 0) {
                statusTimer--;
                if (statusTimer == 0) statusMsg[0] = '\0';
            }

            render_menu(themes, themeCount, cursor, scroll, statusMsg);
            consoleUpdate(NULL);
        }
    }

    free(themes);
    curl_global_cleanup();
    socketExit();
    romfsExit();
    consoleExit(NULL);
    return 0;
}
