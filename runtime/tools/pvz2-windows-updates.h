/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PvZ2: Reflourished on Windows: automatic updates, in the installed build only.
 *
 * The installer's program links WinSparkle and carries its feed and the EdDSA
 * public key (runtime/win/build-win.sh passes PVZ2_UPDATE_FEED_URL and
 * PVZ2_UPDATE_PUBLIC_KEY). The portable build compiles this file with
 * PVZ2_UPDATES=0: it holds no updater, its Settings has no Updates section, and
 * "Check for Updates..." opens the releases page in the browser instead.
 */
#ifndef PVZ2_WINDOWS_UPDATES_H
#define PVZ2_WINDOWS_UPDATES_H

#include <windows.h>

/* The game's own thread closes the game for the installer: the updater calls
 * back from its own thread, and the save must happen on the game's. */
#define PVZ2_WM_UPDATE_QUIT (WM_APP + 17)
/* A check finished: the settings window refreshes the last-check time. */
#define PVZ2_WM_UPDATE_CHECK_DONE (WM_APP + 18)

/* 1 when this program was linked with WinSparkle (the installer's build). */
int pvz2_updates_built_in(void);
/* 1 when it has a feed and a public key, so there is an updater to use. */
int pvz2_updates_configured(void);
/* Sets WinSparkle up and starts its daily checks; call once, after the
 * settings are read, before the game starts. */
void pvz2_updates_init(HWND game_window, int automatic);
/* "Check for Updates..." and "Check Now": WinSparkle's window, or the
 * releases page when there is no updater. */
void pvz2_updates_check(HWND owner);
/* The last successful feed check, in Unix seconds; 0 when never. */
long long pvz2_updates_last_check(void);
/* The Settings checkbox and Apply change WinSparkle's schedule too. */
void pvz2_updates_set_automatic(int on);
/* Where the portable build's menu item goes. */
const char *pvz2_updates_page(void);
/* Stops WinSparkle's thread; the process is about to exit. */
void pvz2_updates_shutdown(void);

#endif /* PVZ2_WINDOWS_UPDATES_H */
