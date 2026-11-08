/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PvZ2: Reflourished on Windows: the settings window.
 *
 * A plain Win32 window with common controls v6, the system's own message font
 * at the window's DPI, and two pages: General and Keyboard. The look follows
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
#include <wchar.h>

#include "pvz2-windows-settings.h"

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
    s->keep_window = 1;
    s->adapt_shape = 0;
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
    s->keep_window = ini_int(path, "window", "keepSizeAndPlace", d.keep_window);
    s->adapt_shape = ini_int(path, "window", "adaptShape", d.adapt_shape);
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
    WritePrivateProfileStringA("window", "keepSizeAndPlace", s->keep_window ? "1" : "0", path);
    WritePrivateProfileStringA("window", "adaptShape", s->adapt_shape ? "1" : "0", path);
    WritePrivateProfileStringA(NULL, NULL, NULL, path);          /* flush */
    return 1;
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
#define ID_PAGE_KEYBOARD 111

#define ID_START_FULL    200
#define ID_KEEP_WINDOW   201
#define ID_OPEN_SAVES    202
#define ID_OPEN_DATA     203
#define ID_ADAPT_SHAPE   204

#define ID_RESTORE       300
#define ID_APPLY         301

/* The key fields, in the order the pages lay them out. */
#define ID_KEY_BASE 1000
enum {
    FIELD_SLOT0 = 0, FIELD_SLOT7 = 7,
    FIELD_SHOVEL, FIELD_PLANT_FOOD, FIELD_FAST_FORWARD, FIELD_COLLECT_SUN,
    FIELD_POWERUP1, FIELD_POWERUP2, FIELD_POWERUP3, FIELD_COUNT
};
static const int g_field_ids[FIELD_COUNT] = {
    ID_KEY_BASE + FIELD_SLOT0, ID_KEY_BASE + FIELD_SLOT0 + 1, ID_KEY_BASE + FIELD_SLOT0 + 2,
    ID_KEY_BASE + FIELD_SLOT0 + 3, ID_KEY_BASE + FIELD_SLOT0 + 4, ID_KEY_BASE + FIELD_SLOT0 + 5,
    ID_KEY_BASE + FIELD_SLOT0 + 6, ID_KEY_BASE + FIELD_SLOT0 + 7,
    ID_KEY_BASE + FIELD_SHOVEL, ID_KEY_BASE + FIELD_PLANT_FOOD, ID_KEY_BASE + FIELD_FAST_FORWARD,
    ID_KEY_BASE + FIELD_COLLECT_SUN, ID_KEY_BASE + FIELD_POWERUP1, ID_KEY_BASE + FIELD_POWERUP2,
    ID_KEY_BASE + FIELD_POWERUP3
};

static HWND g_self, g_tabs, g_page[2];
static HFONT g_font;
static HBRUSH g_page_bg, g_dialog_bg;
static int g_tab;
static int g_capture = -1;
static pvz2_settings g_pending;
static pvz2_settings *g_live;
static pvz2_settings_host g_host;

static int S(int v) { return MulDiv(v, (int)GetDpiForWindow(g_self), 96); }

static HFONT make_font(int dpi)
{
    NONCLIENTMETRICSW ncm = { .cbSize = sizeof(ncm) };
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, (UINT)dpi))
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

static HWND add(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style,
                int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static HWND add_page(HWND parent, int x, int y, int w, int h, int id)
{
    HWND p = CreateWindowExW(WS_EX_CONTROLPARENT, L"PvZ2SettingsPage", L"", WS_CHILD,
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
        L"Shovel", L"Plant food", L"Fast-forward", L"Collect suns",
        L"Power-up 1", L"Power-up 2", L"Power-up 3"
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
    add(page, L"STATIC", field_label(field), 0, x, y + 3, label_w, 20, 0);
    HWND e = add(page, L"EDIT", L"", WS_BORDER | ES_CENTER | ES_READONLY,
                 x + label_w + 8, y, key_w, 24, g_field_ids[field]);
    SetWindowSubclass(e, keyfield_proc, (UINT_PTR)g_field_ids[field], 0);
    field_refresh(e, field);
}

static void build_keyboard_page(HWND p)
{
    add(p, L"BUTTON", L"Seed packets", BS_GROUPBOX, 10, 8, 500, 142, 0);
    for (int i = 0; i < 4; i++) {
        key_row(p, FIELD_SLOT0 + i, 22, 32 + i * 26, 96, 56);
        key_row(p, FIELD_SLOT0 + 4 + i, 272, 32 + i * 26, 96, 56);
    }
    add(p, L"BUTTON", L"Controls", BS_GROUPBOX, 10, 156, 500, 144, 0);
    key_row(p, FIELD_SHOVEL, 22, 180, 96, 56);
    key_row(p, FIELD_PLANT_FOOD, 22, 208, 96, 56);
    key_row(p, FIELD_FAST_FORWARD, 22, 236, 96, 56);
    key_row(p, FIELD_COLLECT_SUN, 22, 264, 96, 56);
    key_row(p, FIELD_POWERUP1, 272, 180, 96, 56);
    key_row(p, FIELD_POWERUP2, 272, 208, 96, 56);
    key_row(p, FIELD_POWERUP3, 272, 236, 96, 56);
    add(p, L"BUTTON", L"Restore defaults", BS_PUSHBUTTON, 390, 306, 120, 26, ID_RESTORE);
}

static void build_general_page(HWND p)
{
    add(p, L"BUTTON", L"Window", BS_GROUPBOX, 10, 8, 500, 140, 0);
    HWND full = add(p, L"BUTTON", L"Start in full screen", BS_AUTOCHECKBOX, 24, 30, 300, 22, ID_START_FULL);
    SendMessageW(full, BM_SETCHECK, g_pending.start_fullscreen ? BST_CHECKED : BST_UNCHECKED, 0);
    HWND keep = add(p, L"BUTTON", L"Keep the window's size and place", BS_AUTOCHECKBOX, 24, 58, 320, 22, ID_KEEP_WINDOW);
    SendMessageW(keep, BM_SETCHECK, g_pending.keep_window ? BST_CHECKED : BST_UNCHECKED, 0);
    HWND adapt = add(p, L"BUTTON", L"Let the game adapt to the window's shape (4:3 to 21:9)",
                     BS_AUTOCHECKBOX, 24, 86, 460, 22, ID_ADAPT_SHAPE);
    SendMessageW(adapt, BM_SETCHECK, g_pending.adapt_shape ? BST_CHECKED : BST_UNCHECKED, 0);
    /* The guest lays a screen out once, when it opens (the Mac host says the
     * same), so the choice is for the next start, not a live resize. */
    add(p, L"STATIC", L"A new shape takes effect when the game next starts. Until then the", 0, 38, 112, 460, 16, 0);
    add(p, L"STATIC", L"picture keeps the shape it started in, with bars.", 0, 38, 128, 460, 16, 0);

    add(p, L"BUTTON", L"Saves and game data", BS_GROUPBOX, 10, 156, 500, 178, 0);
    add(p, L"STATIC", L"Saves and settings:", 0, 24, 178, 200, 20, 0);
    add(p, L"STATIC", L"", SS_PATHELLIPSIS, 24, 196, 470, 18, 0);
    add(p, L"BUTTON", L"Open saves folder", BS_PUSHBUTTON, 24, 220, 130, 26, ID_OPEN_SAVES);
    add(p, L"STATIC", L"Game data (the OBB and the translation cache):", 0, 24, 254, 300, 20, 0);
    add(p, L"STATIC", L"", SS_PATHELLIPSIS, 24, 272, 470, 18, 0);
    add(p, L"BUTTON", L"Open game data folder", BS_PUSHBUTTON, 24, 296, 140, 26, ID_OPEN_DATA);
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
    for (int j = 0; j < 2; j++) ShowWindow(g_page[j], j == i ? SW_SHOW : SW_HIDE);
}

static void layout(HWND h)
{
    RECT rc;
    GetClientRect(h, &rc);
    int foot = S(40);
    if (g_tabs) {
        SetWindowPos(g_tabs, NULL, S(10), S(10), rc.right - S(20), rc.bottom - S(60),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RECT pr = { 0, 0, rc.right - S(20), rc.bottom - S(60) };
        TabCtrl_AdjustRect(g_tabs, FALSE, &pr);
        for (int i = 0; i < 2; i++)
            if (g_page[i])
                SetWindowPos(g_page[i], NULL, S(10) + pr.left, S(10) + pr.top,
                             pr.right - pr.left, pr.bottom - pr.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
    }
    int bw = S(80), bh = S(26), gap = S(8);
    int x = rc.right - S(10) - bw;
    int y = rc.bottom - S(8) - bh;
    HWND b;
    if ((b = GetDlgItem(h, ID_APPLY)) != NULL) SetWindowPos(b, NULL, x, y, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    x -= bw + gap;
    if ((b = GetDlgItem(h, IDCANCEL)) != NULL) SetWindowPos(b, NULL, x, y, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    x -= bw + gap;
    if ((b = GetDlgItem(h, IDOK)) != NULL) SetWindowPos(b, NULL, x, y, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    (void)foot;
}

static void build(HWND h)
{
    g_dialog_bg = GetSysColorBrush(COLOR_BTNFACE);
    g_page_bg = GetSysColorBrush(COLOR_WINDOW);
    g_font = make_font((int)GetDpiForWindow(h));
    for (int i = 0; i < 2; i++) if (g_page[i]) DestroyWindow(g_page[i]);
    if (g_tabs) DestroyWindow(g_tabs);
    for (int id = IDOK; id <= ID_APPLY; id++) { HWND old = GetDlgItem(h, id); if (old) DestroyWindow(old); }
    g_tabs = NULL;
    g_page[0] = g_page[1] = NULL;

    RECT rc;
    GetClientRect(h, &rc);
    g_tabs = add(h, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS, 10, 10, 520, 400, ID_TABS);
    const wchar_t *names[2] = { L"General", L"Keyboard" };
    for (int i = 0; i < 2; i++) {
        TCITEMW t = { .mask = TCIF_TEXT, .pszText = (wchar_t *)names[i] };
        SendMessageW(g_tabs, TCM_INSERTITEMW, i, (LPARAM)&t);
    }
    RECT pr = { S(10), S(10), rc.right - S(10), rc.bottom - S(50) };
    TabCtrl_AdjustRect(g_tabs, FALSE, &pr);
    g_page[0] = add_page(h, 10, 10, 520, 400, ID_PAGE_GENERAL);
    g_page[1] = add_page(h, 10, 10, 520, 400, ID_PAGE_KEYBOARD);
    build_general_page(g_page[0]);
    build_keyboard_page(g_page[1]);
    fill_paths(g_page[0]);

    add(h, L"BUTTON", L"OK", BS_DEFPUSHBUTTON, 0, 0, 80, 26, IDOK);
    add(h, L"BUTTON", L"Cancel", BS_PUSHBUTTON, 0, 0, 80, 26, IDCANCEL);
    add(h, L"BUTTON", L"Apply", BS_PUSHBUTTON, 0, 0, 80, 26, ID_APPLY);
    SendMessageW(g_tabs, TCM_SETCURSEL, g_tab, 0);
    show_page(g_tab);
    layout(h);
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
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: apply(); DestroyWindow(h); return 0;
        case IDCANCEL: DestroyWindow(h); return 0;
        case ID_APPLY: apply(); return 0;
        case ID_START_FULL:
            g_pending.start_fullscreen = (int)SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return 0;
        case ID_KEEP_WINDOW:
            g_pending.keep_window = (int)SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return 0;
        case ID_ADAPT_SHAPE:
            g_pending.adapt_shape = (int)SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return 0;
        case ID_OPEN_SAVES: ShellExecuteA(h, "open", g_host.saves_dir, NULL, NULL, SW_SHOWNORMAL); return 0;
        case ID_OPEN_DATA: ShellExecuteA(h, "open", g_host.data_dir, NULL, NULL, SW_SHOWNORMAL); return 0;
        case ID_RESTORE: {
            pvz2_settings d;
            pvz2_settings_defaults(&d);
            g_pending = d;
            for (int i = 0; i < FIELD_COUNT; i++) {
                HWND f = GetDlgItem(g_page[1], g_field_ids[i]);
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
        g_page[0] = g_page[1] = NULL;
        if (g_font) { DeleteObject(g_font); g_font = NULL; }
        return 0;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK page_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
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
    g_tab = 0;
    g_capture = -1;

    int dpi = (int)GetDpiForWindow(owner);
    if (!dpi) dpi = (int)GetDpiForSystem();
    RECT r = { 0, 0, MulDiv(540, dpi, 96), MulDiv(470, dpi, 96) };
    AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU, FALSE, 0, (UINT)dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    RECT o = { 0, 0, 0, 0 };
    GetWindowRect(owner ? owner : GetDesktopWindow(), &o);
    HMONITOR mon = MonitorFromWindow(owner ? owner : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { .cbSize = sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int x = o.left + ((o.right - o.left) - w) / 2;
    int y = o.top + ((o.bottom - o.top) - h) / 2;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;

    HWND win = CreateWindowExW(0, L"PvZ2ReflourishedSettings", L"PvZ2 Reflourished Settings",
                               WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h,
                               owner, NULL, GetModuleHandleW(NULL), NULL);
    if (!win) return;
    ShowWindow(win, SW_SHOW);
    SetForegroundWindow(win);
}

int pvz2_settings_is_open(void) { return g_self && IsWindow(g_self); }
HWND pvz2_settings_window(void) { return g_self && IsWindow(g_self) ? g_self : NULL; }
