/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PvZ2: Reflourished on Windows: the settings window.
 *
 * A plain Win32 window with common controls v6, the system's own message font
 * at the window's DPI, and four pages: General, Gameplay, Keyboard and About. The look follows
 * the mock-up the player approved; the controls are laid out from the DPI, and
 * the page windows answer WM_CTLCOLORSTATIC themselves so their labels do not
 * sit on grey boxes.
 *
 * The shortcuts are the Mac host's, named the same way in the file: "1", "Q",
 * "Space", "Tab", "F1", "Up", or an empty string for "None".
 */
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include "pvz2-windows-settings.h"
#include "pvz2-windows-darkmode.h"
#include "pvz2-windows-identity.h"
#include "pvz2-windows-updates.h"

#pragma comment(lib, "comctl32.lib")

/* ------------------------------------------------------------- the values */

void pvz2_settings_defaults(pvz2_settings *s)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < PVZ2_SLOT_COUNT; i++)
        snprintf(s->slot[i], sizeof(s->slot[i]), "%s", pvz2_default_slot_keys[i]);
    snprintf(s->shovel, sizeof(s->shovel), "%s", PVZ2_DEFAULT_SHOVEL_KEY);
    snprintf(s->fast_forward, sizeof(s->fast_forward), "%s", PVZ2_DEFAULT_FAST_FORWARD_KEY);
    snprintf(s->plant_food, sizeof(s->plant_food), "%s", PVZ2_DEFAULT_PLANT_FOOD_KEY);
    snprintf(s->collect_sun, sizeof(s->collect_sun), "Space");
    for (int i = 0; i < PVZ2_POWERUP_COUNT; i++)
        snprintf(s->powerup[i], sizeof(s->powerup[i]), "%s", pvz2_default_powerup_keys[i]);
    s->start_fullscreen = 0;
    s->confirm_quit = 1;
    s->adapt_shape = 0;
    snprintf(s->fixed_aspect, sizeof(s->fixed_aspect), "display");
    s->show_level_keys = 1;
    s->show_cursor_plant = 1;
    s->cursor_cross_brightness = 45;
    s->automatic_updates = 1;
}

/* A value that is missing from the file is the default; a value that is there
 * but empty is "None", which is a setting of its own. */
static void ini_string(const char *path, const char *section, const char *key,
                       const char *fallback, char *out, size_t n)
{
    char buf[128];
    GetPrivateProfileStringA(section, key, "\x01", buf, sizeof(buf), path);
    if (!strcmp(buf, "\x01")) snprintf(out, n, "%s", fallback);
    else snprintf(out, n, "%s", buf);
}

static int ini_int(const char *path, const char *section, const char *key, int fallback)
{
    return (int)GetPrivateProfileIntA(section, key, fallback, path);
}

void pvz2_settings_load(const char *path, pvz2_settings *s)
{
    pvz2_settings d;
    pvz2_settings_defaults(&d);
    pvz2_settings_defaults(s);
    for (int i = 0; i < PVZ2_SLOT_COUNT; i++) {
        char key[24];
        snprintf(key, sizeof(key), "seed%d", i + 1);
        ini_string(path, "keys", key, d.slot[i], s->slot[i], sizeof(s->slot[i]));
    }
    ini_string(path, "keys", "shovel", d.shovel, s->shovel, sizeof(s->shovel));
    ini_string(path, "keys", "fastForward", d.fast_forward, s->fast_forward, sizeof(s->fast_forward));
    ini_string(path, "keys", "plantFood", d.plant_food, s->plant_food, sizeof(s->plant_food));
    ini_string(path, "keys", "collectSun", d.collect_sun, s->collect_sun, sizeof(s->collect_sun));
    for (int i = 0; i < PVZ2_POWERUP_COUNT; i++) {
        char key[24];
        snprintf(key, sizeof(key), "powerup%d", i + 1);
        ini_string(path, "keys", key, d.powerup[i], s->powerup[i], sizeof(s->powerup[i]));
    }
    s->start_fullscreen = ini_int(path, "window", "startFullScreen", d.start_fullscreen);
    s->confirm_quit = ini_int(path, "window", "confirmQuit", d.confirm_quit) != 0;
    char mode[16];
    ini_string(path, "window", "resizeMode", "", mode, sizeof(mode));
    s->adapt_shape = *mode ? !_stricmp(mode, "Adapt") :
        ini_int(path, "window", "adaptShape", ini_int(path, "window", "adapt", 0)) != 0;
    ini_string(path, "window", "fixedAspect", d.fixed_aspect, s->fixed_aspect, sizeof(s->fixed_aspect));
    const char *valid[] = { "display", "game", "16:9", "16:10", "3:2", "4:3", "21:9" };
    int known = 0;
    for (int i = 0; i < 7; i++) if (!strcmp(valid[i], s->fixed_aspect)) known = 1;
    if (!known) snprintf(s->fixed_aspect, sizeof(s->fixed_aspect), "display");
    s->show_level_keys = ini_int(path, "keys", "showLevelKeys", d.show_level_keys);
    s->automatic_updates = ini_int(path, "updates", "automaticChecks", d.automatic_updates) != 0;
    s->show_cursor_plant = ini_int(path, "pointer", "showCursorPlant", d.show_cursor_plant) != 0;
    s->cursor_cross_brightness = ini_int(path, "pointer", "crossBrightness", d.cursor_cross_brightness);
    if (s->cursor_cross_brightness < 0) s->cursor_cross_brightness = 0;
    if (s->cursor_cross_brightness > 100) s->cursor_cross_brightness = 100;
}

int pvz2_settings_save(const char *path, const pvz2_settings *s)
{
    WritePrivateProfileStringA("keys", "seed1", NULL, path);   /* make sure the file exists */
    for (int i = 0; i < PVZ2_SLOT_COUNT; i++) {
        char key[24];
        snprintf(key, sizeof(key), "seed%d", i + 1);
        WritePrivateProfileStringA("keys", key, s->slot[i], path);
    }
    WritePrivateProfileStringA("keys", "shovel", s->shovel, path);
    WritePrivateProfileStringA("keys", "fastForward", s->fast_forward, path);
    WritePrivateProfileStringA("keys", "plantFood", s->plant_food, path);
    WritePrivateProfileStringA("keys", "collectSun", s->collect_sun, path);
    for (int i = 0; i < PVZ2_POWERUP_COUNT; i++) {
        char key[24];
        snprintf(key, sizeof(key), "powerup%d", i + 1);
        WritePrivateProfileStringA("keys", key, s->powerup[i], path);
    }
    WritePrivateProfileStringA("window", "startFullScreen", s->start_fullscreen ? "1" : "0", path);
    WritePrivateProfileStringA("window", "confirmQuit", s->confirm_quit ? "1" : "0", path);
    WritePrivateProfileStringA("window", "resizeMode", s->adapt_shape ? "Adapt" : "Fixed", path);
    WritePrivateProfileStringA("window", "fixedAspect", s->fixed_aspect, path);
    WritePrivateProfileStringA("window", "adaptShape", s->adapt_shape ? "1" : "0", path);
    WritePrivateProfileStringA("keys", "showLevelKeys", s->show_level_keys ? "1" : "0", path);
    WritePrivateProfileStringA("updates", "automaticChecks", s->automatic_updates ? "1" : "0", path);
    WritePrivateProfileStringA("pointer", "showCursorPlant", s->show_cursor_plant ? "1" : "0", path);
    {
        char pct[16];
        snprintf(pct, sizeof(pct), "%d", s->cursor_cross_brightness);
        WritePrivateProfileStringA("pointer", "crossBrightness", pct, path);
    }
    WritePrivateProfileStringA(NULL, NULL, NULL, path);          /* flush */
    return 1;
}

double pvz2_settings_aspect(const pvz2_settings *s, HMONITOR monitor)
{
    if (!strcmp(s->fixed_aspect, "game")) return 1748.0 / 804.0;
    int w, h;
    if (sscanf(s->fixed_aspect, "%d:%d", &w, &h) == 2 && w > 0 && h > 0)
        return (double)w / h;
    MONITORINFO mi = { .cbSize = sizeof(mi) };
    double a = 16.0 / 9.0;
    if (GetMonitorInfoW(monitor, &mi))
        a = (double)(mi.rcMonitor.right - mi.rcMonitor.left) / (mi.rcMonitor.bottom - mi.rcMonitor.top);
    /* These are the limits the game's layout supports, as on the Mac. */
    if (a < 4.0 / 3.0) a = 4.0 / 3.0;
    if (a > 2.4) a = 2.4;
    return a;
}

/* ---------------------------------------------------------------- the keys */

int pvz2_settings_key_name(WPARAM vk, char *out, size_t n)
{
    const char *named = NULL;
    switch (vk) {
    case VK_SPACE: named = "Space"; break;
    case VK_TAB: named = "Tab"; break;
    case VK_LEFT: named = "Left"; break;
    case VK_RIGHT: named = "Right"; break;
    case VK_UP: named = "Up"; break;
    case VK_DOWN: named = "Down"; break;
    case VK_F1: named = "F1"; break;
    case VK_F2: named = "F2"; break;
    case VK_F3: named = "F3"; break;
    case VK_F4: named = "F4"; break;
    case VK_F5: named = "F5"; break;
    case VK_F6: named = "F6"; break;
    case VK_F7: named = "F7"; break;
    case VK_F8: named = "F8"; break;
    case VK_F9: named = "F9"; break;
    case VK_F10: named = "F10"; break;
    case VK_F11: named = "F11"; break;
    case VK_F12: named = "F12"; break;
    default: break;
    }
    if (named) { snprintf(out, n, "%s", named); return 1; }
    /* Return and Escape belong to the game and the settings window; the
     * modifiers and the lock keys cannot be shortcuts on their own. */
    if (vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_BACK || vk == VK_DELETE ||
        vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
        vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL ||
        vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN ||
        vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL)
        return 0;
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        snprintf(out, n, "%c", (char)vk);
        return 1;
    }
    LONG ch = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR) & 0x7fff;
    if (ch < 0x21 || ch > 0x7e) return 0;
    snprintf(out, n, "%c", (char)ch);
    return 1;
}

static const char *owner_name(const pvz2_settings *s, const char *name)
{
    for (int i = 0; i < PVZ2_SLOT_COUNT; i++) {
        static char label[32];
        if (s->slot[i][0] && _stricmp(s->slot[i], name) == 0) {
            snprintf(label, sizeof(label), "seed packet %d", i + 1);
            return label;
        }
    }
    if (s->shovel[0] && _stricmp(s->shovel, name) == 0) return "the shovel";
    if (s->fast_forward[0] && _stricmp(s->fast_forward, name) == 0) return "fast-forward";
    if (s->plant_food[0] && _stricmp(s->plant_food, name) == 0) return "plant food";
    if (s->collect_sun[0] && _stricmp(s->collect_sun, name) == 0) return "collecting suns";
    for (int i = 0; i < PVZ2_POWERUP_COUNT; i++)
        if (s->powerup[i][0] && _stricmp(s->powerup[i], name) == 0) {
            static char label[32];
            snprintf(label, sizeof(label), "power-up %d", i + 1);
            return label;
        }
    return NULL;
}

const char *pvz2_settings_key_owner(const pvz2_settings *s, const char *name)
{
    if (!name || !*name) return NULL;
    return owner_name(s, name);
}

/* ------------------------------------------------------------- the window */

#define ID_TABS          100
#define ID_PAGE_GENERAL  110
#define ID_PAGE_GAMEPLAY 111
#define ID_PAGE_KEYBOARD 112
#define ID_PAGE_ABOUT    113

#define ID_START_FULL    200
#define ID_FIXED_SHAPE   201
#define ID_OPEN_SAVES    202
#define ID_OPEN_DATA     203
#define ID_ADAPT_SHAPE   204
#define ID_SHOW_KEYS     205
#define ID_SHOW_CURSOR   206
#define ID_CROSS_BRIGHTNESS 207
#define ID_CROSS_VALUE   208
#define ID_OPEN_LOG      209
#define ID_ASPECT        210
#define ID_ASPECT_LABEL  211
#define ID_MODE_FOOTER   212
#define ID_CONFIRM_QUIT  213
#define ID_WINDOW_GROUP  214
#define ID_QUIT_FOOTER   215
#define ID_AUTOMATIC_UPDATES 216
#define ID_CHECK_NOW     217
#define ID_LAST_CHECK    218

#define ID_RESTORE       300
#define ID_APPLY         301
#define ID_CHECK_TIMER   302

/* The key fields, in the order the pages lay them out. */
#define ID_KEY_BASE 1000
enum {
    FIELD_SLOT0 = 0, FIELD_SLOT7 = 7,
    FIELD_SHOVEL, FIELD_PLANT_FOOD, FIELD_FAST_FORWARD,
    FIELD_POWERUP1, FIELD_POWERUP2, FIELD_POWERUP3, FIELD_COLLECT_SUN, FIELD_COUNT
};
static const int g_field_ids[FIELD_COUNT] = {
    ID_KEY_BASE + FIELD_SLOT0, ID_KEY_BASE + FIELD_SLOT0 + 1, ID_KEY_BASE + FIELD_SLOT0 + 2,
    ID_KEY_BASE + FIELD_SLOT0 + 3, ID_KEY_BASE + FIELD_SLOT0 + 4, ID_KEY_BASE + FIELD_SLOT0 + 5,
    ID_KEY_BASE + FIELD_SLOT0 + 6, ID_KEY_BASE + FIELD_SLOT0 + 7,
    ID_KEY_BASE + FIELD_SHOVEL, ID_KEY_BASE + FIELD_PLANT_FOOD, ID_KEY_BASE + FIELD_FAST_FORWARD,
    ID_KEY_BASE + FIELD_POWERUP1, ID_KEY_BASE + FIELD_POWERUP2,
    ID_KEY_BASE + FIELD_POWERUP3, ID_KEY_BASE + FIELD_COLLECT_SUN
};

enum { GENERAL, GAMEPLAY, KEYBOARD, ABOUT, PAGE_COUNT };
static HWND g_self, g_tabs, g_page[PAGE_COUNT];
static HFONT g_font;
static HBRUSH g_page_bg, g_dialog_bg;
static int g_tab;
static int g_capture = -1;
static int g_check_ticks;
static pvz2_settings g_pending;
static pvz2_settings *g_live;
static pvz2_settings_host g_host;
static HWND g_owner;
static const char *g_aspect_tags[] = { "display", "game", "", "16:9", "16:10", "3:2", "4:3", "21:9" };

static int S(int v) { return MulDiv(v, (int)GetDpiForWindow(g_self), 96); }

static HFONT make_font(int dpi)
{
    NONCLIENTMETRICSW ncm = { .cbSize = sizeof(ncm) };
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, (UINT)dpi))
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

/* How a control follows the window's width and height. Every control is made at
 * its position for the minimum size (in 96-dpi units); the page layout then moves
 * and stretches it from these rules, so the minimum looks exactly as it always
 * did and a larger window only adds room where it is useful. */
enum {
    A_RIGHT  = 1,    /* x moves with the right edge (a button at the right) */
    A_HALF   = 2,    /* x moves by half the extra width, up to 120 units (the second column) */
    W_FULL   = 4,    /* w grows with all the extra width */
    W_HALF   = 8,    /* w grows with half of it */
    A_BOTTOM = 16    /* sits at the bottom of the page when the page is taller */
};
typedef struct { HWND h; int x, y, w, ht, rule, cap; } item;
#define ITEM_MAX 64
static item g_items[PAGE_COUNT][ITEM_MAX];
static int g_item_count[PAGE_COUNT];
#define MIN_CLIENT_W 560   /* the client size the window opens at, in 96-dpi units */
#define MIN_CLIENT_H 530
#define PAGE_BASE_W  500   /* the width of a group box at that size */
/* A normal resizable frame: edges and corners, no maximize button. */
#ifndef SWP_STATECHANGED
#define SWP_STATECHANGED 0x8000   /* set when the window is shown, hidden, minimized or maximized */
#endif
#define SETTINGS_STYLE (WS_CAPTION | WS_SYSMENU | WS_THICKFRAME)

static HWND add_ruled(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style,
                      int x, int y, int w, int h, int id, int rule, int cap)
{
    if ((!wcscmp(cls, L"BUTTON") && (style & BS_TYPEMASK) != BS_GROUPBOX) ||
        !wcscmp(cls, L"EDIT") || !wcscmp(cls, L"msctls_trackbar32")) style |= WS_TABSTOP;
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    for (int i = 0; i < PAGE_COUNT; i++)
        if (g_page[i] == parent && g_item_count[i] < ITEM_MAX)
            g_items[i][g_item_count[i]++] = (item){ c, x, y, w, h, rule, cap };
    return c;
}

static HWND add(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style,
                int x, int y, int w, int h, int id)
{
    return add_ruled(parent, cls, text, style, x, y, w, h, id, 0, 0);
}

static item *item_of(int page, int id)
{
    HWND c = g_page[page] ? GetDlgItem(g_page[page], id) : NULL;
    for (int i = 0; c && i < g_item_count[page]; i++)
        if (g_items[page][i].h == c) return &g_items[page][i];
    return NULL;
}

static HWND add_page(HWND parent, int x, int y, int w, int h, int id)
{
    HWND p = CreateWindowExW(WS_EX_CONTROLPARENT, L"PvZ2SettingsPage", L"", WS_CHILD | WS_VSCROLL | WS_HSCROLL,
                             S(x), S(y), S(w), S(h), parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(NULL), NULL);
    SendMessageW(p, WM_SETFONT, (WPARAM)g_font, TRUE);
    return p;
}

static const wchar_t *field_label(int field)
{
    static const wchar_t *labels[FIELD_COUNT] = {
        L"Seed packet 1", L"Seed packet 2", L"Seed packet 3", L"Seed packet 4",
        L"Seed packet 5", L"Seed packet 6", L"Seed packet 7", L"Seed packet 8",
        L"Shovel", L"Plant food", L"Fast-forward",
        L"Power-up 1", L"Power-up 2", L"Power-up 3", L"Collect sun, coins, gems and plant food"
    };
    return labels[field];
}

static void field_get(int field, char *out, size_t n)
{
    const char *v = "";
    if (field <= FIELD_SLOT7) v = g_pending.slot[field];
    else if (field == FIELD_SHOVEL) v = g_pending.shovel;
    else if (field == FIELD_PLANT_FOOD) v = g_pending.plant_food;
    else if (field == FIELD_FAST_FORWARD) v = g_pending.fast_forward;
    else if (field == FIELD_COLLECT_SUN) v = g_pending.collect_sun;
    else if (field == FIELD_POWERUP1) v = g_pending.powerup[0];
    else if (field == FIELD_POWERUP2) v = g_pending.powerup[1];
    else if (field == FIELD_POWERUP3) v = g_pending.powerup[2];
    snprintf(out, n, "%s", v && *v ? v : "None");
}

static char *field_value(int field)
{
    if (field <= FIELD_SLOT7) return g_pending.slot[field];
    if (field == FIELD_SHOVEL) return g_pending.shovel;
    if (field == FIELD_PLANT_FOOD) return g_pending.plant_food;
    if (field == FIELD_FAST_FORWARD) return g_pending.fast_forward;
    if (field == FIELD_COLLECT_SUN) return g_pending.collect_sun;
    if (field == FIELD_POWERUP1) return g_pending.powerup[0];
    if (field == FIELD_POWERUP2) return g_pending.powerup[1];
    if (field == FIELD_POWERUP3) return g_pending.powerup[2];
    return NULL;
}

static void field_set(int field, const char *value)
{
    char *v = field_value(field);
    if (v) snprintf(v, PVZ2_KEY_NAME_MAX, "%s", value ? value : "");
}

static void field_refresh(HWND field_hwnd, int field)
{
    char text[32];
    field_get(field, text, sizeof(text));
    SetWindowTextA(field_hwnd, text);
}

/* One key field: clicking it, or tabbing into it, asks for a key; Escape puts
 * the old one back, Delete clears it, and a key another shortcut already has
 * is refused with the name of that shortcut. */
static LRESULT CALLBACK keyfield_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR id, DWORD_PTR ref)
{
    int field = -1;
    for (int i = 0; i < FIELD_COUNT; i++) if ((UINT_PTR)g_field_ids[i] == id) field = i;
    (void)ref;
    switch (msg) {
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
    case WM_LBUTTONDOWN:
        if (field >= 0 && g_capture < 0) {
            g_capture = field;
            SetWindowTextW(h, L"Press a key...");
            SetFocus(h);
        }
        return 0;
    case WM_CHAR: return 0;
    case WM_KEYDOWN: {
        if (field < 0 || g_capture != field) return 0;
        if (wp == VK_ESCAPE) {
            g_capture = -1;
            field_refresh(h, field);
            return 0;
        }
        if (wp == VK_DELETE || wp == VK_BACK) {
            field_set(field, "");
            g_capture = -1;
            field_refresh(h, field);
            return 0;
        }
        char name[PVZ2_KEY_NAME_MAX];
        if (!pvz2_settings_key_name(wp, name, sizeof(name))) return 0;
        /* The field being edited is not "already used": pressing the same key
         * again is how a player keeps it. */
        char *own = field_value(field);
        char old[PVZ2_KEY_NAME_MAX];
        snprintf(old, sizeof(old), "%s", own ? own : "");
        if (own) *own = 0;
        const char *owner = pvz2_settings_key_owner(&g_pending, name);
        if (own) snprintf(own, PVZ2_KEY_NAME_MAX, "%s", old);
        if (owner) {
            char text[160];
            snprintf(text, sizeof(text), "The key %s is already used for %s.\nChoose another key.", name, owner);
            MessageBoxA(g_self, text, "PvZ2 Reflourished Settings", MB_ICONINFORMATION | MB_OK);
            return 0;
        }
        field_set(field, name);
        g_capture = -1;
        field_refresh(h, field);
        return 0;
    }
    case WM_NCDESTROY: RemoveWindowSubclass(h, keyfield_proc, id); break;
    default: break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void key_row(HWND page, int field, int x, int y, int label_w, int key_w)
{
    /* The second column moves with half the extra width, and the key fields
     * widen a little with it (up to 120 units). */
    int col2 = x > 100;
    add_ruled(page, L"STATIC", field_label(field), 0, x, y + 3, label_w, 20, 0, col2 ? A_HALF : 0, 0);
    HWND e = add_ruled(page, L"EDIT", L"", WS_BORDER | ES_CENTER | ES_READONLY,
                       x + label_w + 8, y, key_w, 24, g_field_ids[field],
                       (col2 ? A_HALF : 0) | (key_w < 100 ? W_HALF : 0), 64);
    SetWindowSubclass(e, keyfield_proc, (UINT_PTR)g_field_ids[field], 0);
    field_refresh(e, field);
}

static void build_keyboard_page(HWND p)
{
    add_ruled(p, L"BUTTON", L"Seed packets", BS_GROUPBOX, 10, 8, 500, 142, 0, W_FULL, 0);
    for (int i = 0; i < 8; i++)
        key_row(p, FIELD_SLOT0 + i, i < 4 ? 22 : 272, 32 + (i % 4) * 26, 96, 56);
    add_ruled(p, L"BUTTON", L"Controls", BS_GROUPBOX, 10, 156, 500, 172, 0, W_FULL, 0);
    key_row(p, FIELD_SHOVEL, 22, 180, 96, 56);
    key_row(p, FIELD_PLANT_FOOD, 22, 208, 96, 56);
    key_row(p, FIELD_FAST_FORWARD, 22, 236, 96, 56);
    key_row(p, FIELD_POWERUP1, 272, 180, 96, 56);
    key_row(p, FIELD_POWERUP2, 272, 208, 96, 56);
    key_row(p, FIELD_POWERUP3, 272, 236, 96, 56);
    key_row(p, FIELD_COLLECT_SUN, 22, 294, 346, 100);
    add_ruled(p, L"STATIC", L"Click a key field and press the key you want: letters, digits, punctuation, Space, Tab, the arrows or F1–F12. Esc cancels, Delete clears. A key can only belong to one shortcut.",
              0, 22, 338, 476, 48, 0, W_FULL, 204);
    /* At the right, and at the foot of the page when the page is taller. */
    add_ruled(p, L"BUTTON", L"Restore defaults", BS_PUSHBUTTON, 390, 392, 120, 26, ID_RESTORE,
              A_RIGHT | A_BOTTOM, 0);
}

static void pointer_page_label(void)
{
    if (!g_page[GAMEPLAY]) return;
    HWND label = GetDlgItem(g_page[GAMEPLAY], ID_CROSS_VALUE);
    if (!label) return;
    wchar_t text[16];
    swprintf(text, sizeof(text) / sizeof(text[0]), L"%d%%", g_pending.cursor_cross_brightness);
    SetWindowTextW(label, text);
}

static void build_pointer_page(HWND p)
{
    add_ruled(p, L"BUTTON", L"Playing", BS_GROUPBOX, 10, 8, 500, 116, 0, W_FULL, 0);
    HWND keys = add(p, L"BUTTON", L"Show keys in levels", BS_AUTOCHECKBOX, 24, 30, 440, 22, ID_SHOW_KEYS);
    SendMessageW(keys, BM_SETCHECK, g_pending.show_level_keys ? BST_CHECKED : BST_UNCHECKED, 0);
    add_ruled(p, L"STATIC", L"“Show keys in levels” writes the key that works each control on its corner, in the game's own font (seed packets, shovel, fast-forward, plant food and the level's power-ups); it is on by default.",
              0, 24, 60, 460, 54, 0, W_FULL, 220);
    add_ruled(p, L"BUTTON", L"The pointer", BS_GROUPBOX, 10, 134, 500, 200, 0, W_FULL, 0);
    HWND show = add(p, L"BUTTON", L"Show the selected plant at the pointer", BS_AUTOCHECKBOX,
                    24, 158, 440, 22, ID_SHOW_CURSOR);
    SendMessageW(show, BM_SETCHECK, g_pending.show_cursor_plant ? BST_CHECKED : BST_UNCHECKED, 0);
    add(p, L"STATIC", L"Cross brightness", 0, 24, 206, 140, 20, 0);
    /* The slider takes the extra width; its percentage stays at the right. */
    HWND bar = add_ruled(p, L"msctls_trackbar32", L"", TBS_HORZ | TBS_NOTICKS,
                         180, 200, 220, 32, ID_CROSS_BRIGHTNESS, W_FULL, 0);
    SendMessageW(bar, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageW(bar, TBM_SETPOS, TRUE, g_pending.cursor_cross_brightness);
    add_ruled(p, L"STATIC", L"", 0, 410, 206, 60, 20, ID_CROSS_VALUE, A_RIGHT, 0);
    add_ruled(p, L"STATIC", L"The plant, shovel or plant food in your hand follows the pointer, as it would under your finger, and a cross marks the tile it will land on; the cross hides where a plant cannot go. “Cross brightness” sets how bright the cross is: 45% by default.",
              0, 24, 238, 460, 80, 0, W_FULL, 220);
    pointer_page_label();

}

static void mode_refresh(HWND p)
{
    CheckRadioButton(p, ID_FIXED_SHAPE, ID_ADAPT_SHAPE,
                     g_pending.adapt_shape ? ID_ADAPT_SHAPE : ID_FIXED_SHAPE);
    ShowWindow(GetDlgItem(p, ID_ASPECT), g_pending.adapt_shape ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(p, ID_ASPECT_LABEL), g_pending.adapt_shape ? SW_HIDE : SW_SHOW);
    SetWindowTextW(GetDlgItem(p, ID_MODE_FOOTER), g_pending.adapt_shape ?
        L"The window takes any shape from 4:3 to 2.4:1, and the game starts in the window's shape. The game lays a screen out once, so after you change the shape the picture keeps its old one, with bars: \u21BB restarts the game in the new shape (it saves first), \u2715 puts the window back. Experimental." :
        L"The window keeps one shape and the game fills it, at any size. A new shape takes effect when the game restarts: \u21BB in the corner does that now (it saves first), \u2715 keeps the current shape until the next start. \u201CCurrent display\u201D is the shape of the display the game is on, so full screen has no bars.");
    /* The footer's height and the rows below it follow its wrapped text, which
     * the page layout measures at the page's current width. */
    SendMessageW(p, WM_SIZE, SIZE_RESTORED, 0);
}

/* A control's width at `dw` extra units of page width (in pixels), by its rule. */
static int item_width(const item *it, int dw)
{
    int grow = it->rule & W_FULL ? dw : it->rule & W_HALF ? dw / 2 : 0;
    if (it->cap && grow > S(it->cap)) grow = S(it->cap);
    return S(it->w) + grow;
}

/* Hiding the aspect row removes its space, and neither mode leaves a
 * fixed-height plaque: the footer is measured in the message font at its real
 * width and the rows below follow it. Called by the page layout, before it
 * places the controls. */
static void general_vertical(HWND p, int dw)
{
    item *footer = item_of(GENERAL, ID_MODE_FOOTER);
    if (!footer) return;
    int w = item_width(footer, dw);
    wchar_t text[512];
    GetWindowTextW(footer->h, text, 512);
    HDC dc = GetDC(p);
    HGDIOBJ old = SelectObject(dc, g_font);
    RECT measured = { 0, 0, w, 0 };
    DrawTextW(dc, text, -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(p, dc);
    int dpi = (int)GetDpiForWindow(g_self);
    int y = g_pending.adapt_shape ? 72 : 112;
    int height = (measured.bottom * 96 + dpi - 1) / dpi;      /* back to 96-dpi units */
    int bottom = y + height + 16;
    footer->y = y;
    footer->ht = height;
    item *it;
    if ((it = item_of(GENERAL, ID_WINDOW_GROUP))) { it->y = 8; it->ht = bottom - 8; }
    if ((it = item_of(GENERAL, ID_START_FULL))) it->y = bottom + 20;
    if ((it = item_of(GENERAL, ID_CONFIRM_QUIT))) it->y = bottom + 62;
    if ((it = item_of(GENERAL, ID_QUIT_FOOTER))) it->y = bottom + 98;
}

static void build_general_page(HWND p)
{
    add_ruled(p, L"BUTTON", L"Window", BS_GROUPBOX, 10, 8, 500, 242, ID_WINDOW_GROUP, W_FULL, 0);
    add(p, L"STATIC", L"Mode", 0, 24, 36, 120, 22, 0);
    add(p, L"BUTTON", L"Fixed", BS_AUTORADIOBUTTON | WS_GROUP, 180, 32, 100, 24, ID_FIXED_SHAPE);
    add(p, L"BUTTON", L"Adapt", BS_AUTORADIOBUTTON, 292, 32, 100, 24, ID_ADAPT_SHAPE);
    add(p, L"STATIC", L"Aspect ratio", 0, 24, 78, 140, 22, ID_ASPECT_LABEL);
    /* The list grows with the window, but not past a width that reads well. */
    HWND combo = add_ruled(p, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                           180, 72, 302, 240, ID_ASPECT, W_FULL, 150);
    MONITORINFO mi = { .cbSize = sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(g_owner, MONITOR_DEFAULTTONEAREST), &mi);
    double a = (double)(mi.rcMonitor.right - mi.rcMonitor.left) / (mi.rcMonitor.bottom - mi.rcMonitor.top);
    const double ratios[] = { 16.0/9, 1.6, 1.5, 4.0/3, 21.0/9 };
    const wchar_t *ratio_names[] = { L"16:9", L"16:10", L"3:2", L"4:3", L"21:9" };
    wchar_t shape[32], display[80];
    swprintf(shape, 32, L"%.2f:1", a);
    for (int i = 0; i < 5; i++)
        if (a / ratios[i] > .985 && a / ratios[i] < 1.015) swprintf(shape, 32, L"%ls", ratio_names[i]);
    swprintf(display, 80, L"Current display (%ls)", shape);
    const wchar_t *names[] = { display, L"The game's own (2.17:1)", L"────────────────", L"16:9", L"16:10", L"3:2", L"4:3", L"21:9" };
    for (int i = 0; i < 8; i++) {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)names[i]);
        if (!strcmp(g_aspect_tags[i], g_pending.fixed_aspect)) SendMessageW(combo, CB_SETCURSEL, i, 0);
    }
    add_ruled(p, L"STATIC", L"", 0, 24, 120, 460, 112, ID_MODE_FOOTER, W_FULL, 220);
    HWND full = add(p, L"BUTTON", L"Start in full screen", BS_AUTOCHECKBOX, 24, 276, 440, 24, ID_START_FULL);
    SendMessageW(full, BM_SETCHECK, g_pending.start_fullscreen ? BST_CHECKED : BST_UNCHECKED, 0);
    HWND confirm = add(p, L"BUTTON", L"Ask before quitting", BS_AUTOCHECKBOX, 24, 318, 440, 24, ID_CONFIRM_QUIT);
    SendMessageW(confirm, BM_SETCHECK, g_pending.confirm_quit ? BST_CHECKED : BST_UNCHECKED, 0);
    add_ruled(p, L"STATIC", L"Closing the window or pressing Alt+F4 saves and quits. To pause and keep your place instead, minimize. Quitting asks first when this is on.",
              0, 24, 354, 460, 58, ID_QUIT_FOOTER, W_FULL, 220);
    mode_refresh(p);
}

static void about_refresh_check_time(HWND page)
{
    HWND label = GetDlgItem(page, ID_LAST_CHECK);
    if (!label) return;
    long long t = pvz2_updates_last_check();
    wchar_t text[96];
    if (t <= 0) {
        wcscpy(text, L"Last checked: never");
    } else {
        static const wchar_t *months[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                           L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
        time_t tt = (time_t)t;
        struct tm *tmv = localtime(&tt);
        if (!tmv || tmv->tm_mon < 0 || tmv->tm_mon > 11)
            wcscpy(text, L"Last checked: never");
        else
            swprintf(text, 96, L"Last checked: %d %ls %d at %02d:%02d",
                     tmv->tm_mday, months[tmv->tm_mon], tmv->tm_year + 1900,
                     tmv->tm_hour, tmv->tm_min);
    }
    SetWindowTextW(label, text);
}

static void build_about_page(HWND p)
{
    add(p, L"STATIC", L"PvZ2 Reflourished", 0, 24, 20, 460, 24, 0);
    add(p, L"STATIC", L"Version " L"" PVZ2_APP_VERSION, 0, 24, 50, 460, 20, 0);
    add_ruled(p, L"STATIC", L"Plants vs. Zombies 2: Reflourished, running on Windows: the game's own Android code, drawn through Direct3D 11.",
              0, 24, 80, 460, 42, 0, W_FULL, 220);
    int y = 138;
    if (pvz2_updates_configured()) {
        /* The Mac's Updates section, word for word, with its own last check
         * time; the portable build has no updater, so nothing here. */
        add_ruled(p, L"BUTTON", L"Updates", BS_GROUPBOX, 10, 132, 500, 120, 0, W_FULL, 0);
        HWND auto_check = add(p, L"BUTTON", L"Check for updates automatically", BS_AUTOCHECKBOX,
                              24, 156, 440, 22, ID_AUTOMATIC_UPDATES);
        SendMessageW(auto_check, BM_SETCHECK, g_pending.automatic_updates ? BST_CHECKED : BST_UNCHECKED, 0);
        add_ruled(p, L"STATIC", L"", 0, 24, 184, 340, 20, ID_LAST_CHECK, W_FULL, 0);
        add_ruled(p, L"BUTTON", L"Check Now", BS_PUSHBUTTON, 382, 180, 112, 26, ID_CHECK_NOW, A_RIGHT, 0);
        add_ruled(p, L"STATIC", L"Once a day the app looks for a new release on GitHub, and asks before it installs one. Your saves and settings stay as they are.",
                  0, 24, 212, 460, 34, 0, W_FULL, 220);
        y = 262;
    }
    /* The paths take the extra width (they end in an ellipsis when they must);
     * the buttons stay at the right. */
    add_ruled(p, L"BUTTON", L"Files", BS_GROUPBOX, 10, y, 500, 246, 0, W_FULL, 0);
    add(p, L"STATIC", L"Saves and settings", 0, 24, y + 26, 350, 20, 0);
    add_ruled(p, L"STATIC", L"", SS_PATHELLIPSIS, 24, y + 50, 346, 22, 0, W_FULL, 0);
    add_ruled(p, L"BUTTON", L"Open folder", BS_PUSHBUTTON, 382, y + 36, 112, 26, ID_OPEN_SAVES, A_RIGHT, 0);
    add(p, L"STATIC", L"Game data", 0, 24, y + 94, 350, 20, 0);
    add_ruled(p, L"STATIC", L"", SS_PATHELLIPSIS, 24, y + 118, 346, 22, 0, W_FULL, 0);
    add_ruled(p, L"BUTTON", L"Open folder", BS_PUSHBUTTON, 382, y + 104, 112, 26, ID_OPEN_DATA, A_RIGHT, 0);
    add(p, L"STATIC", L"Log", 0, 24, y + 162, 350, 20, 0);
    add(p, L"STATIC", L"pvz2.log, for a bug report", 0, 24, y + 186, 346, 22, 0);
    add_ruled(p, L"BUTTON", L"Open folder", BS_PUSHBUTTON, 382, y + 172, 112, 26, ID_OPEN_LOG, A_RIGHT, 0);
    about_refresh_check_time(p);
}

/* The two path labels are made here and filled by layout, which knows the
 * page's coordinates but not the folder paths. */
static void fill_paths(HWND page)
{
    HWND first = GetWindow(page, GW_CHILD);
    int seen = 0;
    for (HWND c = first; c; c = GetWindow(c, GW_HWNDNEXT)) {
        wchar_t cls[16];
        GetClassNameW(c, cls, 16);
        if (_wcsicmp(cls, L"Static") != 0) continue;
        if (GetWindowLongW(c, GWL_STYLE) & SS_PATHELLIPSIS) {
            wchar_t path[MAX_PATH * 2];
            MultiByteToWideChar(CP_UTF8, 0, seen == 0 ? g_host.saves_dir : g_host.data_dir, -1,
                                path, MAX_PATH * 2);
            SetWindowTextW(c, path);
            seen++;
        }
    }
}

static void show_page(int i)
{
    for (int j = 0; j < PAGE_COUNT; j++) ShowWindow(g_page[j], j == i ? SW_SHOW : SW_HIDE);
    if (i == ABOUT && g_page[ABOUT]) about_refresh_check_time(g_page[ABOUT]);
}

/* Place one page's controls for the page's current size. Controls sit at their
 * minimum-size positions, plus what their rule takes of the extra width; the
 * extra is whatever the page is wider than the minimum one (never negative: a
 * page that is too narrow scrolls, as it always did). */
static void page_layout(int i)
{
    HWND p = g_page[i];
    if (!p || !g_item_count[i]) return;
    RECT r;
    GetClientRect(p, &r);
    int dw = r.right - S(PAGE_BASE_W + 20);
    if (dw < 0) dw = 0;
    int sx = GetScrollPos(p, SB_HORZ), sy = GetScrollPos(p, SB_VERT);
    if (i == GENERAL) general_vertical(p, dw);
    HDWP dwp = BeginDeferWindowPos(g_item_count[i]);
    for (int k = 0; k < g_item_count[i] && dwp; k++) {
        const item *it = &g_items[i][k];
        /* The second column drifts right with the window, but only so far: past
         * that the two columns would read as unrelated. */
        int half = dw / 2 > S(120) ? S(120) : dw / 2;
        int x = S(it->x) + (it->rule & A_RIGHT ? dw : it->rule & A_HALF ? half : 0);
        int y = S(it->y), h = S(it->ht);
        if ((it->rule & A_BOTTOM) && r.bottom - S(12) - h > y) y = r.bottom - S(12) - h;
        dwp = DeferWindowPos(dwp, it->h, NULL, x - sx, y - sy, item_width(it, dw), h,
                             SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (dwp) EndDeferWindowPos(dwp);
    /* Controls that moved must repaint where they are now. */
    RedrawWindow(p, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* The smallest size the window may have: the client area it opens with, at the
 * window's DPI, plus its frame; never more than the screen it is on holds. */
static void min_window_size(HWND h, int *w, int *ht)
{
    UINT dpi = GetDpiForWindow(h);
    if (!dpi) dpi = 96;
    RECT r = { 0, 0, MulDiv(MIN_CLIENT_W, dpi, 96), MulDiv(MIN_CLIENT_H, dpi, 96) };
    AdjustWindowRectExForDpi(&r, SETTINGS_STYLE, FALSE, 0, dpi);
    *w = r.right - r.left;
    *ht = r.bottom - r.top;
    MONITORINFO mi = { .cbSize = sizeof(mi) };
    if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
        if (*w > mi.rcWork.right - mi.rcWork.left) *w = mi.rcWork.right - mi.rcWork.left;
        if (*ht > mi.rcWork.bottom - mi.rcWork.top) *ht = mi.rcWork.bottom - mi.rcWork.top;
    }
}

static void layout(HWND h)
{
    RECT rc;
    GetClientRect(h, &rc);
    if (IsIconic(h) || rc.right <= 0 || rc.bottom <= 0) return;
    /* The tab area fills the window above the buttons; the buttons keep to the
     * bottom right. */
    int bw = S(80), bh = S(26), gap = S(8);
    int x = rc.right - S(10) - bw;
    int y = rc.bottom - S(8) - bh;
    HDWP dwp = BeginDeferWindowPos(8);
    if (g_tabs && dwp) {
        dwp = DeferWindowPos(dwp, g_tabs, NULL, S(10), S(10), rc.right - S(20), rc.bottom - S(60),
                             SWP_NOZORDER | SWP_NOACTIVATE);
        RECT pr = { 0, 0, rc.right - S(20), rc.bottom - S(60) };
        TabCtrl_AdjustRect(g_tabs, FALSE, &pr);
        for (int i = 0; i < PAGE_COUNT && dwp; i++)
            if (g_page[i])
                dwp = DeferWindowPos(dwp, g_page[i], NULL, S(10) + pr.left, S(10) + pr.top,
                                     pr.right - pr.left, pr.bottom - pr.top,
                                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    static const int buttons[3] = { ID_APPLY, IDCANCEL, IDOK };
    for (int k = 0; k < 3 && dwp; k++, x -= bw + gap) {
        HWND b = GetDlgItem(h, buttons[k]);
        if (b) dwp = DeferWindowPos(dwp, b, NULL, x, y, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (dwp) EndDeferWindowPos(dwp);
    RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static void build(HWND h)
{
    g_dialog_bg = GetSysColorBrush(COLOR_BTNFACE);
    g_page_bg = GetSysColorBrush(COLOR_WINDOW);
    g_font = make_font((int)GetDpiForWindow(h));
    for (int i = 0; i < PAGE_COUNT; i++) if (g_page[i]) DestroyWindow(g_page[i]);
    if (g_tabs) DestroyWindow(g_tabs);
    for (int id = IDOK; id <= ID_APPLY; id++) { HWND old = GetDlgItem(h, id); if (old) DestroyWindow(old); }
    g_tabs = NULL;
    memset(g_page, 0, sizeof(g_page));
    memset(g_item_count, 0, sizeof(g_item_count));

    RECT rc;
    GetClientRect(h, &rc);
    g_tabs = add(h, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS, 10, 10, 520, 400, ID_TABS);
    const wchar_t *names[PAGE_COUNT] = { L"General", L"Gameplay", L"Keyboard", L"About" };
    for (int i = 0; i < PAGE_COUNT; i++) {
        TCITEMW t = { .mask = TCIF_TEXT, .pszText = (wchar_t *)names[i] };
        SendMessageW(g_tabs, TCM_INSERTITEMW, i, (LPARAM)&t);
    }
    RECT pr = { S(10), S(10), rc.right - S(10), rc.bottom - S(50) };
    TabCtrl_AdjustRect(g_tabs, FALSE, &pr);
    g_page[0] = add_page(h, 10, 10, 520, 400, ID_PAGE_GENERAL);
    g_page[GAMEPLAY] = add_page(h, 10, 10, 520, 400, ID_PAGE_GAMEPLAY);
    g_page[KEYBOARD] = add_page(h, 10, 10, 520, 400, ID_PAGE_KEYBOARD);
    g_page[ABOUT] = add_page(h, 10, 10, 520, 400, ID_PAGE_ABOUT);
    build_general_page(g_page[GENERAL]);
    build_keyboard_page(g_page[KEYBOARD]);
    build_pointer_page(g_page[GAMEPLAY]);
    build_about_page(g_page[ABOUT]);
    fill_paths(g_page[ABOUT]);
    /* The pages were sized before their children existed; let them measure
     * their scroll ranges now. */
    for (int i = 0; i < PAGE_COUNT; i++) SendMessageW(g_page[i], WM_SIZE, SIZE_RESTORED, 0);

    add(h, L"BUTTON", L"OK", BS_DEFPUSHBUTTON, 0, 0, 80, 26, IDOK);
    add(h, L"BUTTON", L"Cancel", BS_PUSHBUTTON, 0, 0, 80, 26, IDCANCEL);
    add(h, L"BUTTON", L"Apply", BS_PUSHBUTTON, 0, 0, 80, 26, ID_APPLY);
    SendMessageW(g_tabs, TCM_SETCURSEL, g_tab, 0);
    show_page(g_tab);
    layout(h);
}

/* The library themes the window and the controls; the pages need their own
 * colour subclass because WM_CTLCOLORSTATIC goes to a control's parent. */
static void darkmode_apply(HWND h)
{
    tl_win_darkmode_window(h);
    for (int i = 0; i < PAGE_COUNT; i++)
        if (g_page[i]) tl_win_darkmode_page(g_page[i]);
}

static void apply(void)
{
    if (g_live) *g_live = g_pending;
    pvz2_settings_save(g_host.settings_path, &g_pending);
    if (g_host.apply) g_host.apply(g_host.user, g_live);
}

static LRESULT CALLBACK settings_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_self = h;
        build(h);
        darkmode_apply(h);
        return 0;
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->hwndFrom == g_tabs && n->code == TCN_SELCHANGE) {
            g_tab = (int)SendMessageW(g_tabs, TCM_GETCURSEL, 0, 0);
            show_page(g_tab);
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)g_dialog_bg;
    }
    case WM_HSCROLL:
        if (g_page[GAMEPLAY] && (HWND)lp == GetDlgItem(g_page[GAMEPLAY], ID_CROSS_BRIGHTNESS)) {
            int v = (int)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            g_pending.cursor_cross_brightness = v;
            pointer_page_label();
        }
        return 0;
    case WM_TIMER:
        if (wp == ID_CHECK_TIMER) {
            about_refresh_check_time(g_page[ABOUT]);
            if (--g_check_ticks <= 0) KillTimer(h, ID_CHECK_TIMER);
            return 0;
        }
        break;
    case PVZ2_WM_UPDATE_CHECK_DONE:
        about_refresh_check_time(g_page[ABOUT]);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: apply(); DestroyWindow(h); return 0;
        case IDCANCEL: DestroyWindow(h); return 0;
        case ID_APPLY: apply(); return 0;
        case ID_START_FULL:
            g_pending.start_fullscreen = (int)SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return 0;
        case ID_CONFIRM_QUIT:
            g_pending.confirm_quit = (int)SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return 0;
        case ID_FIXED_SHAPE:
        case ID_ADAPT_SHAPE:
            g_pending.adapt_shape = LOWORD(wp) == ID_ADAPT_SHAPE;
            mode_refresh(g_page[GENERAL]);
            return 0;
        case ID_ASPECT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int i = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                if (i == 2) {
                    /* Skip the separator in either keyboard direction. */
                    i = !strcmp(g_pending.fixed_aspect, "game") ? 3 : 1;
                    SendMessageW((HWND)lp, CB_SETCURSEL, i, 0);
                }
                if (i >= 0 && i < 8 && *g_aspect_tags[i])
                    snprintf(g_pending.fixed_aspect, sizeof(g_pending.fixed_aspect), "%s", g_aspect_tags[i]);
                else for (int j = 0; j < 8; j++) if (!strcmp(g_aspect_tags[j], g_pending.fixed_aspect))
                    SendMessageW((HWND)lp, CB_SETCURSEL, j, 0);
            }
            return 0;
        case ID_SHOW_KEYS:
            g_pending.show_level_keys = (int)SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return 0;
        case ID_SHOW_CURSOR:
            g_pending.show_cursor_plant = (int)SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return 0;
        case ID_AUTOMATIC_UPDATES:
            g_pending.automatic_updates = (int)SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return 0;
        case ID_CHECK_NOW:
            /* WinSparkle's own window reports through the callbacks; the page
             * follows the last check time for a while after the click. */
            pvz2_updates_check(h);
            g_check_ticks = 30;
            SetTimer(h, ID_CHECK_TIMER, 1000, NULL);
            return 0;
        case ID_OPEN_SAVES: ShellExecuteA(h, "open", g_host.saves_dir, NULL, NULL, SW_SHOWNORMAL); return 0;
        case ID_OPEN_DATA: ShellExecuteA(h, "open", g_host.data_dir, NULL, NULL, SW_SHOWNORMAL); return 0;
        case ID_OPEN_LOG: ShellExecuteA(h, "open", g_host.data_dir, NULL, NULL, SW_SHOWNORMAL); return 0;
        case ID_RESTORE: {
            pvz2_settings d;
            pvz2_settings_defaults(&d);
            /* Restoring keys must not reset the window or gameplay choices. */
            memcpy(g_pending.slot, d.slot, sizeof(d.slot));
            memcpy(g_pending.shovel, d.shovel, sizeof(d.shovel));
            memcpy(g_pending.plant_food, d.plant_food, sizeof(d.plant_food));
            memcpy(g_pending.fast_forward, d.fast_forward, sizeof(d.fast_forward));
            memcpy(g_pending.collect_sun, d.collect_sun, sizeof(d.collect_sun));
            memcpy(g_pending.powerup, d.powerup, sizeof(d.powerup));
            for (int i = 0; i < FIELD_COUNT; i++) {
                HWND f = GetDlgItem(g_page[KEYBOARD], g_field_ids[i]);
                if (f) field_refresh(f, i);
            }
            return 0;
        }
        default: break;
        }
        return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        DeleteObject(g_font);
        g_font = NULL;
        build(h);
        darkmode_apply(h);
        return 0;
    }
    case WM_SETTINGCHANGE:
        /* Windows' light or dark mode may have changed while this window is
         * open: the library re-reads it and the window redraws in place. */
        if (tl_win_darkmode_system_changed(lp)) {
            darkmode_apply(h);
            tl_win_darkmode_redraw(h);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        /* The edges stop at the minimum... */
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        int w, ht;
        min_window_size(h, &w, &ht);
        mm->ptMinTrackSize.x = w;
        mm->ptMinTrackSize.y = ht;
        return 0;
    }
    case WM_WINDOWPOSCHANGING: {
        /* ...and so does a program that calls SetWindowPos or MoveWindow (a
         * window tool such as AltSnap): the minimum track size does not bind
         * those. When the left or top edge is the one that moved, the opposite
         * edge stays where it was. */
        WINDOWPOS *wpos = (WINDOWPOS *)lp;
        if ((wpos->flags & (SWP_NOSIZE | SWP_STATECHANGED)) || IsIconic(h)) break;
        int w, ht;
        min_window_size(h, &w, &ht);
        if (wpos->cx >= w && wpos->cy >= ht) break;
        RECT cur;
        GetWindowRect(h, &cur);
        if (wpos->cx < w) {
            if (!(wpos->flags & SWP_NOMOVE) && wpos->x != cur.left && wpos->x + wpos->cx == cur.right)
                wpos->x = cur.right - w;
            wpos->cx = w;
        }
        if (wpos->cy < ht) {
            if (!(wpos->flags & SWP_NOMOVE) && wpos->y != cur.top && wpos->y + wpos->cy == cur.bottom)
                wpos->y = cur.bottom - ht;
            wpos->cy = ht;
        }
        return 0;
    }
    case WM_SIZE:
        layout(h);
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_self = NULL;
        g_tabs = NULL;
        memset(g_page, 0, sizeof(g_page));
        if (g_font) { DeleteObject(g_font); g_font = NULL; }
        return 0;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK page_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        for (int i = 0; i < PAGE_COUNT; i++)
            if (g_page[i] == h) page_layout(i);
        for (int axis = 0; axis < 2; axis++) {
            int bar = axis ? SB_VERT : SB_HORZ;
            SCROLLINFO si = { .cbSize = sizeof(si), .fMask = SIF_POS };
            GetScrollInfo(h, bar, &si);
            int old = si.nPos;
            si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
            si.nMin = 0;
            /* The range follows the children, so the About page scrolls to
             * its Files section with and without the Updates section. */
            int extent = 0;
            for (HWND c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
                RECT r;
                if (!GetWindowRect(c, &r)) continue;
                MapWindowPoints(NULL, h, (POINT *)&r, 2);
                int end = axis ? r.bottom : r.right;
                if (end + old > extent) extent = end + old;
            }
            /* A page scrolls only when its controls do not fit; then it keeps a
             * margin after the last one. At the minimum size the Keyboard page
             * fits, and a larger window takes every scroll bar away. */
            RECT r; GetClientRect(h, &r);
            int room = axis ? r.bottom : r.right;
            si.nMax = extent - 1;
            if (extent > room) si.nMax += S(12);
            if (room && si.nMax < room - 1) si.nMax = room - 1;
            si.nPage = room;
            SetScrollInfo(h, bar, &si, TRUE);
            si.fMask = SIF_POS;
            GetScrollInfo(h, bar, &si);
            if (old != si.nPos) ScrollWindowEx(h, axis ? 0 : old - si.nPos,
                axis ? old - si.nPos : 0, NULL, NULL, NULL, NULL, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
        }
        return 0;
    case WM_MOUSEWHEEL:
        return SendMessageW(h, WM_VSCROLL, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? SB_LINEUP : SB_LINEDOWN, 0);
    case WM_VSCROLL:
    case WM_HSCROLL: {
        /* A trackbar still forwards its own scroll message to Settings. */
        if (lp) return SendMessageW(GetParent(h), msg, wp, lp);
        int bar = msg == WM_VSCROLL ? SB_VERT : SB_HORZ;
        SCROLLINFO si = { .cbSize = sizeof(si), .fMask = SIF_ALL };
        GetScrollInfo(h, bar, &si);
        int old = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP: si.nPos -= S(32); break;
        case SB_LINEDOWN: si.nPos += S(32); break;
        case SB_PAGEUP: si.nPos -= si.nPage; break;
        case SB_PAGEDOWN: si.nPos += si.nPage; break;
        case SB_THUMBTRACK: si.nPos = si.nTrackPos; break;
        default: return 0;
        }
        si.fMask = SIF_POS;
        SetScrollInfo(h, bar, &si, TRUE);
        GetScrollInfo(h, bar, &si);
        ScrollWindowEx(h, bar == SB_HORZ ? old - si.nPos : 0, bar == SB_VERT ? old - si.nPos : 0,
                       NULL, NULL, NULL, NULL, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)wp, &r, g_page_bg ? g_page_bg : GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        return (LRESULT)(g_page_bg ? g_page_bg : GetSysColorBrush(COLOR_WINDOW));
    }
    /* The page's buttons send their command to the page, not the settings
     * window: pass it on, or Restore defaults and the folder buttons are dead. */
    case WM_COMMAND:
    case WM_NOTIFY:
        return SendMessageW(GetParent(h), msg, wp, lp);
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void register_classes(void)
{
    static int done;
    if (done) return;
    WNDCLASSEXW pc = { sizeof(pc) };
    pc.lpfnWndProc = page_proc;
    pc.hInstance = GetModuleHandleW(NULL);
    pc.lpszClassName = L"PvZ2SettingsPage";
    pc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&pc);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = settings_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"PvZ2ReflourishedSettings";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);
    done = 1;
}

void pvz2_settings_open(HWND owner, pvz2_settings *live, const pvz2_settings_host *host)
{
    register_classes();
    if (g_self && IsWindow(g_self)) {
        ShowWindow(g_self, SW_RESTORE);
        SetForegroundWindow(g_self);
        return;
    }
    g_live = live;
    g_pending = *live;
    g_host = *host;
    g_owner = owner;
    g_tab = 0;
    g_capture = -1;

    int dpi = (int)GetDpiForWindow(owner);
    if (!dpi) dpi = (int)GetDpiForSystem();
    RECT r = { 0, 0, MulDiv(560, dpi, 96), MulDiv(530, dpi, 96) };
    AdjustWindowRectExForDpi(&r, SETTINGS_STYLE, FALSE, 0, (UINT)dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    RECT o = { 0, 0, 0, 0 };
    GetWindowRect(owner ? owner : GetDesktopWindow(), &o);
    HMONITOR mon = MonitorFromWindow(owner ? owner : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { .cbSize = sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    if (w > mi.rcWork.right - mi.rcWork.left) w = mi.rcWork.right - mi.rcWork.left;
    if (h > mi.rcWork.bottom - mi.rcWork.top) h = mi.rcWork.bottom - mi.rcWork.top;
    int x = o.left + ((o.right - o.left) - w) / 2;
    int y = o.top + ((o.bottom - o.top) - h) / 2;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;

    HWND win = CreateWindowExW(0, L"PvZ2ReflourishedSettings", L"PvZ2 Reflourished Settings",
                               SETTINGS_STYLE | WS_CLIPCHILDREN, x, y, w, h,
                               owner, NULL, GetModuleHandleW(NULL), NULL);
    if (!win) return;
    pvz2_window_app_id(win);
    ShowWindow(win, SW_SHOW);
    SetForegroundWindow(win);
}

int pvz2_settings_is_open(void) { return g_self && IsWindow(g_self); }
HWND pvz2_settings_window(void) { return g_self && IsWindow(g_self) ? g_self : NULL; }
