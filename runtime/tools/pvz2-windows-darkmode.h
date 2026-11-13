/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Windows host's dark mode, through win32-darkmodelib: the plain C face of
 * pvz2-windows-darkmode.cpp.
 *
 * The library's functions are declared extern "C" inside namespace dmlib, so
 * a C file cannot name them. Everything C++ stays in the one .cpp behind
 * these prototypes, which keeps the rest of the host C and the C++ runtime in
 * a single object.
 */
#ifndef PVZ2_WINDOWS_DARKMODE_H
#define PVZ2_WINDOWS_DARKMODE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads Windows' light or dark preference once, before any window is made. */
void tl_win_darkmode_init(void);
/* Returns true only when the player explicitly chooses Quit. */
int tl_win_darkmode_confirm_quit(HWND owner);

/* 1 while the host is in dark mode, 0 in light mode (the library's classic). */
int tl_win_darkmode_active(void);

/* A top-level host window: dark title bar, its controls, and its background. */
void tl_win_darkmode_window(HWND hwnd);

/* A page window inside the Settings window: its labels and its background. */
void tl_win_darkmode_page(HWND hwnd);

/* The game window's title bar only; the game's picture is left alone. */
void tl_win_darkmode_title(HWND hwnd);

/* WM_SETTINGCHANGE's lParam. Returns 1 when the light/dark preference changed
 * and the library has re-read it; the caller then redraws its own windows. */
int tl_win_darkmode_system_changed(LPARAM lp);

/* Re-applies the title bar and child themes and redraws the window. */
void tl_win_darkmode_redraw(HWND hwnd);

#ifdef __cplusplus
}
#endif

#endif
