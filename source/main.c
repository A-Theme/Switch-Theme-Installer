// A-Theme Installer — a minimal Switch homebrew (.nro) app that fetches a
// list of Tinfoil themes from the A-Theme GitHub repo and installs the
// selected one directly onto the SD card, ready for Tinfoil to pick up.
//
// This does NOT reproduce the visual theme *editor* — that's a browser/
// desktop tool by design (mouse, keyboard, color pickers). This app's job
// is just: browse -> download -> install, entirely on-console.
//
// Controls: D-Pad / Left Stick = move cursor, A = install selected theme,
//           + = exit
//
// Build requirements: devkitA64, libnx, and the switch-curl / switch-mbedtls
// / switch-zlib portlibs. See BUILD.md in this folder for full setup.

#include <switch.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdbool.h>

// ---- Configuration -------------------------------------------------------

// Where the manifest and theme folders live. Change this if you fork the
// repo or want to point the installer at a different theme collection.
#define BASE_URL     "https://raw.githubusercontent.com/A-Theme/Tinfoil-Themes/main/"
#define MANIFEST_URL BASE_URL "themes.txt"

// Where installed themes get written on the SD card (Tinfoil's own layout).
#define THEMES_ROOT "sdmc:/switch/tinfoil/themes/"

#define MAX_THEMES   256
#define PAGE_SIZE    14
#define NAME_LEN     96
#define FOLDER_LEN   96
#define ASSETS_LEN   256

typedef struct {
    char name[NAME_LEN];      // display name shown in the menu
    char folder[FOLDER_LEN];  // folder name in the repo AND on the SD card
    char assets[ASSETS_LEN];  // optional comma-separated extra files
                               // (e.g. "logo.png,shop-theme.mp3") beyond
                               // theme.json, only needed if the theme's
                               // own theme.json references local sdmc:
                               // paths instead of remote https:// URLs.
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

// Fetches `url` into `out` (caller must free(out->data) on success).
// Returns true on HTTP 200, false on any network/parse/HTTP error.
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    // Bundled CA cert store (see romfs/cacert.pem + BUILD.md) so HTTPS to
    // GitHub is actually verified rather than blindly trusted.
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

// ---- Filesystem helpers ----------------------------------------------------

// Recursively creates every directory component of `path`. Safe to call on
// paths that already exist (mkdir failures on existing dirs are ignored).
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

// ---- Manifest parsing -------------------------------------------------------

// themes.txt format — one theme per line, pipe-separated:
//   Display Name|folder_name|optional,extra,assets.ext
// Blank lines and lines starting with # are ignored.
static void parse_manifest(char *text, Theme *themes, int *count) {
    *count = 0;
    char *saveptr1 = NULL;
    char *line = strtok_r(text, "\n", &saveptr1);

    while (line && *count < MAX_THEMES) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';

        if (line[0] != '\0' && line[0] != '#') {
            char *saveptr2 = NULL;
            char *name   = strtok_r(line, "|", &saveptr2);
            char *folder = strtok_r(NULL, "|", &saveptr2);
            char *assets = strtok_r(NULL, "|", &saveptr2);

            if (name && folder) {
                Theme *t = &themes[*count];
                strncpy(t->name, name, NAME_LEN - 1);   t->name[NAME_LEN - 1] = '\0';
                strncpy(t->folder, folder, FOLDER_LEN - 1); t->folder[FOLDER_LEN - 1] = '\0';
                if (assets) { strncpy(t->assets, assets, ASSETS_LEN - 1); t->assets[ASSETS_LEN - 1] = '\0'; }
                else t->assets[0] = '\0';
                (*count)++;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr1);
    }
}

// ---- Install logic -----------------------------------------------------------

static bool install_theme(const Theme *t) {
    char dest_dir[300];
    snprintf(dest_dir, sizeof(dest_dir), THEMES_ROOT "%s", t->folder);
    mkpath(dest_dir);

    // theme.json itself
    char json_url[400];
    snprintf(json_url, sizeof(json_url), BASE_URL "%s/theme.json", t->folder);

    MemBuf jsonbuf;
    if (!http_get(json_url, &jsonbuf)) return false;

    char dest_file[400];
    snprintf(dest_file, sizeof(dest_file), "%s/theme.json", dest_dir);
    bool ok = write_file(dest_file, jsonbuf.data, jsonbuf.size);
    free(jsonbuf.data);
    if (!ok) return false;

    // optional extra assets (logo/audio files theme.json references locally)
    if (t->assets[0] != '\0') {
        char assets_copy[ASSETS_LEN];
        strncpy(assets_copy, t->assets, ASSETS_LEN - 1);
        assets_copy[ASSETS_LEN - 1] = '\0';

        char *saveptr = NULL;
        char *asset = strtok_r(assets_copy, ",", &saveptr);
        while (asset) {
            while (*asset == ' ') asset++; // trim leading space

            char asset_url[500];
            snprintf(asset_url, sizeof(asset_url), BASE_URL "%s/%s", t->folder, asset);

            MemBuf assetbuf;
            if (http_get(asset_url, &assetbuf)) {
                char asset_dest[450];
                snprintf(asset_dest, sizeof(asset_dest), "%s/%s", dest_dir, asset);
                write_file(asset_dest, assetbuf.data, assetbuf.size);
                free(assetbuf.data);
            }
            // A failed asset download doesn't fail the whole install — the
            // theme.json is already in place, which is the part that matters.

            asset = strtok_r(NULL, ",", &saveptr);
        }
    }

    return true;
}

// ---- UI -----------------------------------------------------------------------

static void render_menu(Theme *themes, int count, int cursor, int scroll, const char *status) {
    printf("\x1b[1;1H\x1b[2J"); // clear screen
    printf("\x1b[36;1mA-Theme Installer\x1b[0m — Tinfoil theme downloader\n");
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
    printf("A = Install selected theme    + = Exit\n");
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

    Theme themes[MAX_THEMES];
    int themeCount = 0;

    MemBuf manifest;
    bool gotManifest = http_get(MANIFEST_URL, &manifest);
    if (gotManifest) {
        parse_manifest(manifest.data, themes, &themeCount);
        free(manifest.data);
    }

    if (!gotManifest || themeCount == 0) {
        printf("\nCould not load the theme list.\n"
               "Check that your Switch is connected to Wi-Fi, then\n"
               "press + to exit and try again.\n");
        consoleUpdate(NULL);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);
        }
    } else {
        int cursor = 0, scroll = 0, statusTimer = 0;
        char statusMsg[160] = "";
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

            if (kDown & HidNpadButton_A) {
                snprintf(statusMsg, sizeof(statusMsg), "Installing \"%s\"...", themes[cursor].name);
                render_menu(themes, themeCount, cursor, scroll, statusMsg);
                consoleUpdate(NULL);

                bool ok = install_theme(&themes[cursor]);
                if (ok) {
                    snprintf(statusMsg, sizeof(statusMsg),
                        "Installed \"%s\" — pick it in Tinfoil's theme settings.", themes[cursor].name);
                } else {
                    snprintf(statusMsg, sizeof(statusMsg),
                        "Failed to install \"%s\". Check your connection and try again.", themes[cursor].name);
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

    curl_global_cleanup();
    socketExit();
    romfsExit();
    consoleExit(NULL);
    return 0;
}
