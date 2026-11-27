/* MindMaze - built-in updater, the portable part (CHANGE 2026-11-20; rules: REA/release-rules.md "Updater").
 *
 * The desktop builds ask GitHub for the newest release (releases/latest of MM_REPO_URL), compare its tag with
 * MM_VERSION and, after the player agrees, download their versioned platform archive, check it against the
 * release's "<file>.sha256", put it in place and start again.
 * Downloading and replacing files is the platform's job (win32_update.c, sdl_update.c); the Android app does the
 * same in shell.js + UpdaterPlugin.java. Nothing here touches the network or the disk. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define UPD_MAX_ASSETS 24

typedef struct {
    char tag[64];                    /* "v1.0.3" */
    char page[512];                  /* the release's web page */
    int nassets;
    struct { char name[128]; char url[512]; } asset[UPD_MAX_ASSETS];
} UpdRelease;

/* the API address of the newest release; setting "update_url" overrides it (tests). 0 = no real repository
 * (a local build with the OWNER placeholder): no update check */
int upd_api_url(const char *setting, char *out, size_t n);
/* GitHub's release JSON -> r. 1 = ok */
int upd_parse(const char *json, UpdRelease *r);
/* 1 if tag ("v1.2.3" or "1.2.3") is a newer version than current ("1.2.2") */
int upd_newer(const char *tag, const char *current);
const char *upd_asset(const UpdRelease *r, const char *name);   /* its download URL, NULL if not in the release */
/* data matches the checksum file ("<64 hex>  <name>"; the first 64 hex digits count). 1 = ok */
int upd_sha_ok(const void *data, size_t n, const char *sumfile, size_t sumlen);

/* archives: fn gets every entry (path inside the archive, '/' separated; type 'f' file, 'd' folder, 'l' symlink with
 * the target as data; mode = unix permissions, 0 if unknown). fn returns 0 to stop. 1 = all entries read */
typedef int (*UpdEntry)(void *ud, const char *path, char type, unsigned mode, const uint8_t *data, size_t n);
int upd_untar_gz(const uint8_t *gz, size_t n, UpdEntry fn, void *ud);
int upd_unzip(const uint8_t *zip, size_t n, UpdEntry fn, void *ud);
