/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PvZ2: Reflourished on Windows: automatic updates (WinSparkle), installed build only.
 *
 * The feed is the Windows appcast (docs/windows-shipping.md), one item per
 * architecture, each pointing at the same setup program. Every update is
 * signed with the project's EdDSA key; the public half is compiled in here the
 * same way Sparkle's is compiled into the Mac app. A test build passes another
 * feed and key (SPARKLE_FEED_URL, SPARKLE_PUBLIC_KEY) and never uses the real
 * key.
 *
 * WinSparkle keeps its own state (last check, skipped version) in
 * HKCU\Software\oenderg\PvZ2 Reflourished\WinSparkle; the installer's
 * uninstaller removes that key.
 */
#include "pvz2-profile.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pvz2-windows-updates.h"

#ifndef PVZ2_UPDATES
#define PVZ2_UPDATES 0
#endif
#ifndef PVZ2_APP_VERSION
#define PVZ2_APP_VERSION "0"
#endif
#ifndef PVZ2_UPDATE_FEED_URL
#define PVZ2_UPDATE_FEED_URL ""
#endif
#ifndef PVZ2_UPDATE_PUBLIC_KEY
#define PVZ2_UPDATE_PUBLIC_KEY ""
#endif

void tl_log_line(const char *fmt, ...);

#define RELEASES_PAGE "https://github.com/" PVZ2_PROFILE_REPO "/releases/latest"

/* Both builds answer this the same way: a manual check ends up here. */
static void open_releases_page(HWND owner)
{
    const char *mode = getenv("TL_PVZ2_OPEN_URL");
    if (mode && !strcmp(mode, "log")) {
        tl_log_line("windows: open URL %s (test: not opened)", RELEASES_PAGE);
        return;
    }
    tl_log_line("windows: open URL %s", RELEASES_PAGE);
    ShellExecuteA(owner, "open", RELEASES_PAGE, NULL, NULL, SW_SHOWNORMAL);
}

const char *pvz2_updates_page(void) { return RELEASES_PAGE; }

/* ------------------------------------------------------------------ the updater */

#if PVZ2_UPDATES

#include "winsparkle.h"
#include "pvz2-windows-settings.h"

static HWND g_game_window;
static int g_configured;
static int g_automatic;

static void __cdecl on_shutdown_request(void)
{
    /* WinSparkle has launched the installer already and calls this from its own
     * thread; the game saves only on its own thread, so wake that one and wait
     * here until the process is gone. */
    tl_log_line("windows: the updater asks the game to close for the installer");
    if (!g_game_window || !PostMessageW(g_game_window, PVZ2_WM_UPDATE_QUIT, 0, 0)) {
        tl_log_line("windows: no window to close the game with; exiting");
        ExitProcess(0);
    }
    for (;;) Sleep(1000);
}

static void check_done(void)
{
    HWND settings = pvz2_settings_window();
    if (settings) PostMessageW(settings, PVZ2_WM_UPDATE_CHECK_DONE, 0, 0);
}

static void __cdecl on_update_found(void)
{
    tl_log_line("windows: the updater found a new version");
    check_done();
}

static void __cdecl on_no_update(void)
{
    tl_log_line("windows: the updater found no new version");
    check_done();
}

static void __cdecl on_update_cancelled(void)
{
    tl_log_line("windows: the update was cancelled");
    check_done();
}

static void __cdecl on_update_error(void)
{
    tl_log_line("windows: the updater reported an error");
    check_done();
}

/* 1.2.3 is build 10203 (major*10000 + minor*100 + patch), as on the Mac; the
 * build number is what the appcast's sparkle:version is compared with. */
static void app_build(const char *version, wchar_t *out, size_t n)
{
    int a = 0, b = 0, c = 0;
    sscanf(version, "%d.%d.%d", &a, &b, &c);
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (c < 0) c = 0;
    swprintf(out, n, L"%d", a * 10000 + b * 100 + c);
}

void pvz2_updates_init(HWND game_window, int automatic)
{
    g_game_window = game_window;
    g_automatic = automatic ? 1 : 0;
    if (!*PVZ2_UPDATE_FEED_URL || !*PVZ2_UPDATE_PUBLIC_KEY) {
        tl_log_line("windows: this build has no update feed or key; the updater is off");
        return;
    }
    wchar_t version[64], build[32];
    MultiByteToWideChar(CP_UTF8, 0, PVZ2_APP_VERSION, -1, version, 64);
    app_build(PVZ2_APP_VERSION, build, 32);
    win_sparkle_set_app_details(L"oenderg", L"" PVZ2_PROFILE_APP_NAME, version);
    win_sparkle_set_app_build_version(build);
    win_sparkle_set_appcast_url(PVZ2_UPDATE_FEED_URL);
    win_sparkle_set_eddsa_public_key(PVZ2_UPDATE_PUBLIC_KEY);
    /* One check a day, as the Mac's Sparkle does, and the Settings checkbox
     * decides whether it happens at all. */
    win_sparkle_set_update_check_interval(24 * 60 * 60);
    win_sparkle_set_automatic_check_for_updates(g_automatic);
    win_sparkle_set_shutdown_request_callback(on_shutdown_request);
    win_sparkle_set_did_find_update_callback(on_update_found);
    win_sparkle_set_did_not_find_update_callback(on_no_update);
    win_sparkle_set_update_cancelled_callback(on_update_cancelled);
    win_sparkle_set_error_callback(on_update_error);
    win_sparkle_init();
    g_configured = 1;
    tl_log_line("windows: updates from %s, version %s (build %ls), automatic %s, last check %lld",
                PVZ2_UPDATE_FEED_URL, PVZ2_APP_VERSION, build,
                g_automatic ? "on" : "off", (long long)win_sparkle_get_last_check_time());
}

int pvz2_updates_built_in(void) { return 1; }
int pvz2_updates_configured(void) { return g_configured; }

void pvz2_updates_check(HWND owner)
{
    (void)owner;
    if (!g_configured) {
        open_releases_page(g_game_window);
        return;
    }
    tl_log_line("windows: checking for updates now");
    win_sparkle_check_update_with_ui();
}

long long pvz2_updates_last_check(void)
{
    time_t t = g_configured ? win_sparkle_get_last_check_time() : 0;
    return t > 0 ? (long long)t : 0;
}

void pvz2_updates_set_automatic(int on)
{
    g_automatic = on ? 1 : 0;
    if (g_configured) {
        win_sparkle_set_automatic_check_for_updates(g_automatic);
        tl_log_line("windows: automatic update checks are %s", g_automatic ? "on" : "off");
    }
}

void pvz2_updates_shutdown(void)
{
    if (g_configured) win_sparkle_cleanup();
}

#else /* the portable build: no WinSparkle at all */

int pvz2_updates_built_in(void) { return 0; }
int pvz2_updates_configured(void) { return 0; }

void pvz2_updates_init(HWND game_window, int automatic)
{
    (void)game_window;
    (void)automatic;
    tl_log_line("windows: the portable build has no updater; its menu item opens the releases page");
}

void pvz2_updates_check(HWND owner) { open_releases_page(owner); }

long long pvz2_updates_last_check(void) { return 0; }
void pvz2_updates_set_automatic(int on) { (void)on; }
void pvz2_updates_shutdown(void) { }

#endif
