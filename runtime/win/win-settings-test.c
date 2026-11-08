/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise profile migration without opening a window or touching player data. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "pvz2-windows-settings.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failed++; } } while (0)

/* The settings window's updates section logs through the game's logger; this test never
 * opens the window, so the logger is a quiet stand-in. */
void tl_log_line(const char *fmt, ...) { (void)fmt; }

int main(void)
{
    char temp[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(sizeof(temp), temp) || !GetTempFileNameA(temp, "rfl", 0, path)) return 2;
    int failed = 0;
    pvz2_settings s;
    pvz2_settings_load(path, &s);
    CHECK(!s.adapt_shape && !strcmp(s.fixed_aspect, "display"));
    CHECK(s.confirm_quit);
    CHECK(s.show_level_keys && s.show_cursor_plant && s.cursor_cross_brightness == 45);
    CHECK(s.automatic_updates);
    WritePrivateProfileStringA("window", "adaptShape", "1", path);
    WritePrivateProfileStringA("window", "keepSizeAndPlace", "0", path);
    WritePrivateProfileStringA("keys", "seed1", "J", path);
    WritePrivateProfileStringA("keys", "shovel", "", path);
    WritePrivateProfileStringA("keys", "collectSun", "F9", path);
    WritePrivateProfileStringA("keys", "showLevelKeys", "0", path);
    WritePrivateProfileStringA("pointer", "showCursorPlant", "0", path);
    WritePrivateProfileStringA("pointer", "crossBrightness", "71", path);
    WritePrivateProfileStringA("updates", "automaticChecks", "0", path);
    pvz2_settings_load(path, &s);
    CHECK(s.adapt_shape && !strcmp(s.slot[0], "J") && !*s.shovel);
    CHECK(!strcmp(s.collect_sun, "F9") && !s.show_level_keys && !s.show_cursor_plant && s.cursor_cross_brightness == 71);
    CHECK(!s.automatic_updates);
    s.automatic_updates = 1;
    s.adapt_shape = 0;
    s.confirm_quit = 0;
    strcpy(s.fixed_aspect, "4:3");
    pvz2_settings_save(path, &s);
    pvz2_settings_load(path, &s);
    CHECK(!s.adapt_shape && !strcmp(s.fixed_aspect, "4:3"));
    CHECK(!s.confirm_quit);
    CHECK(!strcmp(s.slot[0], "J") && !*s.shovel && s.cursor_cross_brightness == 71);
    CHECK(pvz2_settings_aspect(&s, NULL) == 4.0 / 3.0);
    WritePrivateProfileStringA("window", "resizeMode", NULL, path);
    WritePrivateProfileStringA("window", "adaptShape", NULL, path);
    WritePrivateProfileStringA("window", "adapt", "1", path);
    WritePrivateProfileStringA("window", "fixedAspect", "broken", path);
    WritePrivateProfileStringA("pointer", "crossBrightness", "999", path);
    pvz2_settings_load(path, &s);
    CHECK(s.adapt_shape && !strcmp(s.fixed_aspect, "display") && s.cursor_cross_brightness == 100);
    DeleteFileA(path);
    printf("settings migration: %s (%d failures)\n", failed ? "FAIL" : "PASS", failed);
    return failed ? 1 : 0;
}
