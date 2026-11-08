/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PvZ2: Reflourished on Windows: the settings, and the window that edits them.
 *
 * The settings are the shortcuts (the same ones the Mac host has, named the
 * same way) and two window choices. They live in an INI file in the settings
 * folder, written with the Windows profile functions: a missing or broken file
 * means the defaults, never a crash.
 */
#ifndef PVZ2_WINDOWS_SETTINGS_H
#define PVZ2_WINDOWS_SETTINGS_H

#include <windows.h>
#include "pvz2-keys.h"

#define PVZ2_KEY_NAME_MAX 16

typedef struct {
    /* The eight seed packets, then the shovel, as pvz2-keys.h names them. */
    char slot[PVZ2_SLOT_COUNT][PVZ2_KEY_NAME_MAX];
    char shovel[PVZ2_KEY_NAME_MAX];
    char fast_forward[PVZ2_KEY_NAME_MAX];
    char plant_food[PVZ2_KEY_NAME_MAX];
    char collect_sun[PVZ2_KEY_NAME_MAX];
    char powerup[PVZ2_POWERUP_COUNT][PVZ2_KEY_NAME_MAX];
    int start_fullscreen;
    int keep_window;
} pvz2_settings;

typedef struct {
    /* Called when the player presses Apply or OK: the live settings must take
     * the values now. The window has already written the file. */
    void (*apply)(void *user, const pvz2_settings *settings);
    const char *settings_path;   /* the INI file */
    const char *saves_dir;       /* shown, and opened by "Open saves folder" */
    const char *data_dir;        /* shown, and opened by "Open game data folder" */
    void *user;
} pvz2_settings_host;

void pvz2_settings_defaults(pvz2_settings *s);
void pvz2_settings_load(const char *path, pvz2_settings *s);
int pvz2_settings_save(const char *path, const pvz2_settings *s);
void pvz2_settings_open(HWND owner, pvz2_settings *live, const pvz2_settings_host *host);
int pvz2_settings_is_open(void);
HWND pvz2_settings_window(void);

/* A key's name as the file stores it ("1", "Q", "Space", "Tab", "F1", "Up"),
 * or 0 for a key that cannot be a shortcut (Escape, Return, the modifiers). */
int pvz2_settings_key_name(WPARAM vk, char *out, size_t n);

/* Which shortcut a name means, for the message about a key already in use:
 * "seed packet 3", "the shovel", ...; NULL when no shortcut has it. */
const char *pvz2_settings_key_owner(const pvz2_settings *s, const char *name);

#endif /* PVZ2_WINDOWS_SETTINGS_H */
