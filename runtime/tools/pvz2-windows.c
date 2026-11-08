/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PvZ2: Reflourished on Windows: the host.
 *
 * A Win32 window that ANGLE draws into (on Direct3D), the mouse as a finger,
 * the keyboard's shortcuts, and the game's start-up: the same driver as the Mac
 * and iPad hosts (husk-tl-pvz2.c), with the game's libraries loaded at run time.
 *
 * Where things are (each overridable for tests):
 *   the APK          Game/pvz2.apk next to the program        (TL_PVZ2_APK)
 *   the CDN archive  Game/1.4.2.main.tar next to the program  (TL_PVZ2_MAIN_TAR)
 *   ANGLE            libEGL.dll and libGLESv2.dll next to it  (TL_ANGLE_EGL, TL_ANGLE_GLES)
 *   the data folder  %LOCALAPPDATA%/PvZ2 Reflourished          (TL_DATA), with pvz2.log
 *   the saves        %APPDATA%/PvZ2 Reflourished (its own sdcard layout; TL_DATA)
 *   the settings     settings.ini beside the saves
 *
 * A file named "portable" next to the program puts all three folders in "data"
 * beside it. TL_DATA overrides all three, so a scripted test touches nothing
 * outside its own folder.
 */
#include <windows.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <ctype.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdatomic.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>

#include "husk-tl-audio.h"
#include "husk-tl-cdn.h"
#include "husk-tl-cursor-grid.h"
#include "husk-tl-jni.h"
#include "husk-tl-ld.h"
#include "husk-tl-pvz2.h"
#include "husk-tl-xmem.h"
#include "pvz2-cursor-images.h"
#include "pvz2-keys.h"
#include "pvz2-windows-settings.h"
#include "../win/pvz2-obb-win.h"
#if defined(__x86_64__)
#include "husk-tl-x64.h"
#endif

void tl_log_line(const char *fmt, ...);
void tl_win_log_open(const char *path);
void tl_win_mkdirs(const char *path);
void tl_win_install_crash_handler(void);
void tl_win_set_saves_dir(const char *dir);
/* runtime/win/husk-obb-win.c: the game's OBB before the first start. */
int tl_win_obb_ensure(const char *data_dir, const char *exe_dir);

/* The size the game draws at. It lays out for 1748x804 and picks its HD art from 804 up. The window gets as
 * much of that as the screen has room for, never less than the minimum; the game's surface is the window's
 * real client size, so drawing and touch are 1:1. */
#define DESIGN_W 1748
#define DESIGN_H 804
#define MIN_CLIENT_W 800
#define MIN_CLIENT_H 450
#define WINDOW_STYLE (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN)
#define ID_SETTINGS 0x1000

static int g_sw = DESIGN_W, g_sh = DESIGN_H;
static HWND g_hwnd;
/* The picture: the game is a child window, the largest rectangle of the game's
 * shape that fits, centred. See "the picture" below. */
static HWND g_view;
static int g_adapt;
static int g_show_keys;
static int g_show_cursor_plant = 1;
static int g_cross_brightness = 45;
static void badges_reposition(void);
static void pointer_apply_setting(void);
static void pointer_reposition(void);
static void pointer_hide(void);
static void pointer_tick(void);
static void make_pointer_window(HINSTANCE inst);
static void badges_apply_setting(void);
static int g_layout_w = DESIGN_W, g_layout_h = DESIGN_H;
static bool g_down;
static void view_layout(void);
static void startup_layout_from_window(void);
static char g_exe_dir[MAX_PATH], g_data[MAX_PATH], g_saves[MAX_PATH], g_apk[MAX_PATH], g_egl[MAX_PATH], g_gles[MAX_PATH];
static char g_settings_path[MAX_PATH];
static pvz2_settings g_settings;
static tl_ga_config g_cfg;
static atomic_int g_ready;
static atomic_int g_driver_done, g_driver_ok;

/* The window's own state, for the full screen switch and the remembered place. */
static int g_fullscreen;
static DWORD g_windowed_style;
static WINDOWPLACEMENT g_windowed_placement;
static int g_start_fullscreen;

static HWND g_progress;
static HFONT g_progress_font;

static void keyboard_shown(int show) { tl_log_line("windows: the game %s the keyboard", show ? "shows" : "hides"); }

static void slashes(char *p) { for (; *p; p++) if (*p == '\\') *p = '/'; }
static const char *env_or(const char *name, const char *fallback) { const char *v = getenv(name); return v && *v ? v : fallback; }
static void join(char *out, size_t n, const char *a, const char *b) { snprintf(out, n, "%s/%s", a, b); }

/* ------------------------------------------------------------- the game run */

static DWORD WINAPI launch_game(void *arg)
{
    (void)arg;
    if (!getenv("TL_AUDIO") || atoi(getenv("TL_AUDIO"))) tl_audio_install();
    bool ok = tl_pvz2_start(&g_cfg) && tl_pvz2_run();
    atomic_store(&g_ready, ok);
    atomic_store(&g_driver_ok, ok);
    atomic_store(&g_driver_done, 1);
    tl_log_line("windows: driver startup %s", ok ? "complete" : "FAILED");
    if (!ok) MessageBoxA(g_hwnd, "Reflourished could not start. See pvz2.log in the data folder for details.", "PvZ2 Reflourished", MB_ICONERROR);
    return 0;
}

/* -------------------------------------------------------------------- keys */

static bool key_matches(const char *setting, const char *name)
{
    return setting && *setting && _stricmp(setting, name) == 0;
}

/* A shortcut is the game's own feature, not a made-up tap: the seed packet
 * press, the shovel and the rest are the driver's requests, as on the Mac. */
static bool shortcut(WPARAM vk)
{
    char name[PVZ2_KEY_NAME_MAX];
    if (!pvz2_settings_key_name(vk, name, sizeof(name))) return false;
    for (int i = 0; i < PVZ2_SLOT_COUNT; i++) {
        if (key_matches(g_settings.slot[i], name)) {
            tl_log_line("windows: shortcut \"%s\": seed packet %d", name, i + 1);
            tl_pvz2_request_seed_pick(i);
            return true;
        }
    }
    if (key_matches(g_settings.shovel, name)) {
        tl_log_line("windows: shortcut \"%s\": the game's own shovel", name);
        tl_pvz2_request_native_shovel();
        return true;
    }
    if (key_matches(g_settings.fast_forward, name)) {
        tl_log_line("windows: shortcut \"%s\": the game's own fast-forward", name);
        tl_pvz2_request_fast_forward();
        return true;
    }
    if (key_matches(g_settings.plant_food, name)) {
        tl_log_line("windows: shortcut \"%s\": the game's own plant food", name);
        tl_pvz2_request_plant_food();
        return true;
    }
    if (key_matches(g_settings.collect_sun, name)) {
        tl_log_line("windows: shortcut \"%s\": collects the suns and coins", name);
        tl_pvz2_request_collect_sun();
        return true;
    }
    for (int i = 0; i < PVZ2_POWERUP_COUNT; i++) {
        if (key_matches(g_settings.powerup[i], name)) {
            tl_log_line("windows: shortcut \"%s\": power-up %d", name, i + 1);
            tl_pvz2_request_powerup(i);
            return true;
        }
    }
    return false;
}

/* Keys while the game's text field has the keyboard: typed text, Backspace and Return, as
 * Android's input method sends them (the same numbers the Mac host sends). */
static bool key_down(WPARAM vk, LPARAM lp)
{
    if (!atomic_load(&g_ready)) return false;
    bool repeat = (lp & (1 << 30)) != 0;
    switch (vk) {
    case VK_ESCAPE: tl_pvz2_request_escape(); return true;                 /* Android's Back */
    case VK_RETURN: tl_pvz2_key(66, 1); tl_pvz2_key(66, 0); return true;    /* the IME's done */
    case VK_BACK: if (tl_pvz2_keyboard_showing()) { tl_pvz2_text_action(3, ""); return true; } return false;
    default: break;
    }
    if (repeat || tl_pvz2_keyboard_showing()) return false;
    /* The shortcuts are off while a modifier is held, so Ctrl+C is a copy and
     * not "power-up 3". Alt+Enter is the full screen switch, below. */
    if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0) return false;
    return shortcut(vk);
}

static void typed(WPARAM ch)
{
    static wchar_t high;
    if (!atomic_load(&g_ready) || ch < 0x20 || ch == 0x7f) return;
    wchar_t w[3] = { 0 };
    if (ch >= 0xD800 && ch < 0xDC00) { high = (wchar_t)ch; return; }
    if (ch >= 0xDC00 && ch < 0xE000 && high) { w[0] = high; w[1] = (wchar_t)ch; high = 0; } else w[0] = (wchar_t)ch;
    char u[16] = "";
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), NULL, NULL);
    if (tl_pvz2_keyboard_showing()) tl_pvz2_text_action(0, u);
}

/* ------------------------------------------------------ the window's place */

static void save_geometry(void)
{
    if (!g_hwnd || !g_settings.keep_window) return;
    WINDOWPLACEMENT wp = { .length = sizeof(wp) };
    if (g_fullscreen) wp = g_windowed_placement;
    else if (!GetWindowPlacement(g_hwnd, &wp)) return;
    RECT r = wp.rcNormalPosition;
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%ld", (long)r.left);   WritePrivateProfileStringA("window", "x", tmp, g_settings_path);
    snprintf(tmp, sizeof(tmp), "%ld", (long)r.top);    WritePrivateProfileStringA("window", "y", tmp, g_settings_path);
    snprintf(tmp, sizeof(tmp), "%ld", (long)(r.right - r.left)); WritePrivateProfileStringA("window", "width", tmp, g_settings_path);
    snprintf(tmp, sizeof(tmp), "%ld", (long)(r.bottom - r.top)); WritePrivateProfileStringA("window", "height", tmp, g_settings_path);
    WritePrivateProfileStringA("window", "maximized", wp.showCmd == SW_SHOWMAXIMIZED ? "1" : "0", g_settings_path);
    WritePrivateProfileStringA(NULL, NULL, NULL, g_settings_path);
}

/* A remembered place is used only when enough of it is on a monitor now: an
 * unplugged display must not leave the window where nobody can reach it. */
struct monitor_check { const RECT *want; int found; };
static BOOL CALLBACK monitor_check_proc(HMONITOR mon, HDC dc, LPRECT r, LPARAM lp)
{
    (void)dc; (void)r;
    struct monitor_check *c = (struct monitor_check *)lp;
    MONITORINFO mi = { .cbSize = sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    RECT x;
    if (!IntersectRect(&x, c->want, &mi.rcWork)) return TRUE;
    if (x.right - x.left >= 80 && x.bottom - x.top >= 40) { c->found = 1; return FALSE; }
    return TRUE;
}

static int rect_on_a_monitor(const RECT *r)
{
    struct monitor_check c = { .want = r, .found = 0 };
    EnumDisplayMonitors(NULL, NULL, monitor_check_proc, (LPARAM)&c);
    return c.found;
}

static void default_client(int *w, int *h, int *x, int *y)
{
    *w = DESIGN_W; *h = DESIGN_H;
    RECT r = { 0, 0, *w, *h };
    AdjustWindowRectExForDpi(&r, WINDOW_STYLE, FALSE, 0, GetDpiForSystem());
    int frame_w = (int)(r.right - r.left) - *w, frame_h = (int)(r.bottom - r.top) - *h;
    RECT area;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0)) {
        area.left = area.top = 0;
        area.right = GetSystemMetrics(SM_CXSCREEN);
        area.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int room_w = (int)(area.right - area.left) - frame_w;
    int room_h = (int)(area.bottom - area.top) - frame_h;
    if (*w > room_w) *w = room_w;
    if (*h > room_h) *h = room_h;
    if (*w < MIN_CLIENT_W) *w = room_w < MIN_CLIENT_W ? room_w : MIN_CLIENT_W;
    if (*h < MIN_CLIENT_H) *h = room_h < MIN_CLIENT_H ? room_h : MIN_CLIENT_H;
    if (*w < 100) *w = 100;
    if (*h < 100) *h = 100;
    *x = area.left + ((int)(area.right - area.left) - (*w + frame_w)) / 2;
    *y = area.top + ((int)(area.bottom - area.top) - (*h + frame_h)) / 2;
    if (*x < area.left) *x = area.left;
    if (*y < area.top) *y = area.top;
}

/* The place the window opens in: the remembered one when it is still reachable,
 * otherwise a centred window at the design size. */
static void initial_placement(RECT *out, int *maximized)
{
    *maximized = 0;
    if (g_settings.keep_window) {
        int x = (int)GetPrivateProfileIntA("window", "x", -100000, g_settings_path);
        int y = (int)GetPrivateProfileIntA("window", "y", -100000, g_settings_path);
        int w = (int)GetPrivateProfileIntA("window", "width", 0, g_settings_path);
        int h = (int)GetPrivateProfileIntA("window", "height", 0, g_settings_path);
        *maximized = (int)GetPrivateProfileIntA("window", "maximized", 0, g_settings_path);
        RECT r = { x, y, x + w, y + h };
        if (w >= 200 && h >= 150 && rect_on_a_monitor(&r)) {
            *out = r;
            tl_log_line("windows: the window opens where it was left: %dx%d at %d,%d%s", w, h, x, y,
                        *maximized ? ", maximized" : "");
            return;
        }
        if (w >= 200 && h >= 150)
            tl_log_line("windows: the remembered place %dx%d at %d,%d is off every monitor; centring the window", w, h, x, y);
    }
    int w, h, x, y;
    default_client(&w, &h, &x, &y);
    RECT r = { 0, 0, w, h };
    AdjustWindowRectExForDpi(&r, WINDOW_STYLE, FALSE, 0, GetDpiForSystem());
    out->left = x; out->top = y;
    out->right = x + (r.right - r.left);
    out->bottom = y + (r.bottom - r.top);
}

/* The monitor the window is on, for the full screen switch. */
static HMONITOR monitor_for(const RECT *r)
{
    HMONITOR mon = MonitorFromRect(r, MONITOR_DEFAULTTONEAREST);
    if (!mon) mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    return mon;
}

static void toggle_fullscreen(int on)
{
    if (!g_hwnd || on == g_fullscreen) return;
    if (on) {
        GetWindowPlacement(g_hwnd, &g_windowed_placement);
        g_windowed_style = (DWORD)GetWindowLongPtrW(g_hwnd, GWL_STYLE);
        RECT wr;
        GetWindowRect(g_hwnd, &wr);
        HMONITOR mon = monitor_for(&wr);
        MONITORINFO mi = { .cbSize = sizeof(mi) };
        if (!GetMonitorInfoW(mon, &mi)) return;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        g_fullscreen = 1;
        tl_log_line("windows: full screen on the monitor %d,%d %dx%d", mi.rcMonitor.left, mi.rcMonitor.top,
                    mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
    } else {
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, g_windowed_style);
        SetWindowPlacement(g_hwnd, &g_windowed_placement);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        g_fullscreen = 0;
        tl_log_line("windows: back to the window's own size and place");
    }
    save_geometry();
}

/* ------------------------------------------------------------- the windows */

static void settings_applied(void *user, const pvz2_settings *s)
{
    (void)user;
    g_settings = *s;
    g_adapt = s->adapt_shape;
    g_show_keys = s->show_level_keys;
    g_show_cursor_plant = s->show_cursor_plant;
    g_cross_brightness = s->cursor_cross_brightness;
    view_layout();
    badges_apply_setting();
    pointer_apply_setting();
    save_geometry();
    toggle_fullscreen(g_settings.start_fullscreen ? 1 : 0);
    tl_log_line("windows: the settings are applied without a restart (%s shape at the next start)",
                g_adapt ? "the window's, between 4:3 and 21:9" : "the game's own");
}

static void open_settings(void)
{
    pvz2_settings_host host = {
        .apply = settings_applied,
        .settings_path = g_settings_path,
        .saves_dir = g_saves,
        .data_dir = g_data,
        .user = NULL
    };
    pvz2_settings_open(g_hwnd, &g_settings, &host);
}

/* The shape the game starts in. Fixed: the game's own 1748x804. Adapt: the
 * window's shape, kept between 4:3 and 2.4:1 (the shapes the game fills). The
 * guest lays a screen out once, when it opens, so this is chosen at the start;
 * another window shape afterwards gets bars, exactly as the Mac's Adapt mode
 * does (it offers a restart for the new shape; the settings say so too). */
static void startup_layout_from_window(void)
{
    g_layout_w = DESIGN_W;
    g_layout_h = DESIGN_H;
    if (!g_adapt || !g_hwnd) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int cw = (int)(rc.right - rc.left), ch = (int)(rc.bottom - rc.top);
    if (cw <= 0 || ch <= 0) return;
    double a = (double)cw / (double)ch;
    if (a < 4.0 / 3.0) a = 4.0 / 3.0;
    if (a > 2.4) a = 2.4;
    g_layout_w = cw;
    g_layout_h = (int)((double)cw / a + 0.5);
    tl_log_line("windows: the game starts in the window's shape (%.3f:1, kept between 4:3 and 2.4:1)", a);
}

/* ---------------------------------------------------------------- the picture
 *
 * The window is the frame; the game is a child window inside it, the largest
 * rectangle of the game's shape (1748x804) that fits, centred, with black bars
 * around it. ANGLE draws into that child, so the guest's surface is exactly
 * the picture, and the bars are the parent's black background, never stale
 * game pixels. A tap in the bars is the parent's, and does nothing.
 */
static void view_layout(void)
{
    if (!g_hwnd || !g_view) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;
    double aspect = (double)g_layout_w / (double)g_layout_h;
    int vw = cw, vh = (int)((double)vw / aspect + 0.5);
    if (vh > ch) { vh = ch; vw = (int)((double)vh * aspect + 0.5); }
    if (vw < 1) vw = 1;
    if (vh < 1) vh = 1;
    int x = (cw - vw) / 2, y = (ch - vh) / 2;
    SetWindowPos(g_view, NULL, x, y, vw, vh, SWP_NOZORDER | SWP_NOACTIVATE);
    badges_reposition();
    pointer_reposition();
    if (vw != g_sw || vh != g_sh) {
        g_sw = vw; g_sh = vh;
        tl_log_line("windows: window %dx%d client, the game draws at %dx%d (%d,%d), bars %d left %d right %d top %d bottom",
                    cw, ch, vw, vh, x, y, x, cw - vw - x, y, ch - vh - y);
        tl_pvz2_set_surface_size(vw, vh);
    }
}

/* ------------------------------------------------------------- key badges
 *
 * The Mac's look, with no plaque: the key in yellow with a thick dark
 * outline straight over the game, so the game shows between and around the
 * letters. The bundled level-ui font (italic) draws the letters and digits,
 * as on the Mac and iPad. That font has no Tab or arrow symbols, and the
 * system symbol fonts are too thin to survive the Mac's outline, so the host
 * draws those five as filled paths shaped like the Mac's Lucida Grande Bold
 * fallback: short fat arrows and an arrow into a bar.
 *
 * The driver publishes where the controls are and whether the keys work
 * (tl_pvz2_badges); the host supplies the labels from its own key settings.
 * GDI+ draws the glyph path into the layered window's premultiplied DIB,
 * because GDI text cannot write an alpha channel: the pixels off the letters
 * stay fully transparent, and the antialiased edges keep their coverage.
 */
#define BADGE_COUNT (TL_PVZ2_BADGE_SLOTS + 1 + 1 + 1 + TL_PVZ2_BADGE_POWERUPS)
#define BADGE_TIMER 103

static HWND g_badge_win;
static int g_badge_visible;
static unsigned int g_badge_generation;
static int g_badge_src_w, g_badge_src_h;
static ULONG_PTR g_gdiplus_token;
static int g_gdiplus_started;
static GpFontCollection *g_badge_fonts;
static GpFontFamily *g_badge_family;
static int g_badge_level_ui;
static HDC g_badge_dc;
static HBITMAP g_badge_dib;
static HGDIOBJ g_badge_old;
static void *g_badge_bits;
static int g_badge_w, g_badge_h;

/* The Mac's colours and stroke widths: the yellow is (1.0, 0.94, 0.18), the
 * outline (0.08, 0.06, 0.02). CoreText's stroke width is a percentage of the
 * font size and its pen is centred on the glyph, so the yellow pen is 0.03 em
 * and the dark one 0.18 em; the dark layer sits em/32 right and down. */
#define BADGE_YELLOW 0xFFFFF02E
#define BADGE_DARK   0xFF140F05

static const int *badge_rect(int index, const tl_pvz2_badge_snapshot *s)
{
    if (index < TL_PVZ2_BADGE_SLOTS) return index < s->slot_count ? s->packet[index] : NULL;
    index -= TL_PVZ2_BADGE_SLOTS;
    if (index == 0) return s->shovel;
    if (index == 1) return s->fast_forward;
    if (index == 2) return s->plant_food;
    index -= 3;
    return index >= 0 && index < s->powerup_count ? s->powerup[index] : NULL;
}

static int badge_symbol_label(const wchar_t *s)
{
    return s[0] == 0x21E5 || (s[0] >= 0x2190 && s[0] <= 0x2193);
}

/* The key the settings give this control; empty is None, and no badge. The
 * four symbols are badge_key_text's own mapping on the Mac. */
static const wchar_t *badge_label(int index, const tl_pvz2_badge_snapshot *s, wchar_t *buf, int n)
{
    const char *key = NULL;
    if (index < TL_PVZ2_BADGE_SLOTS) {
        if (index >= s->slot_count) return NULL;
        key = g_settings.slot[index];
    } else if (index == TL_PVZ2_BADGE_SLOTS) {
        key = g_settings.shovel;
    } else if (index == TL_PVZ2_BADGE_SLOTS + 1) {
        key = g_settings.fast_forward;
    } else if (index == TL_PVZ2_BADGE_SLOTS + 2) {
        key = g_settings.plant_food;
    } else {
        int p = index - TL_PVZ2_BADGE_SLOTS - 3;
        if (p < 0 || p >= PVZ2_POWERUP_COUNT || p >= s->powerup_count) return NULL;
        key = g_settings.powerup[p];
    }
    if (!key || !*key) return NULL;
    static const struct { const char *name; wchar_t sym; } symbols[] = {
        { "Tab", 0x21E5 }, { "Left", 0x2190 }, { "Right", 0x2192 },
        { "Up", 0x2191 }, { "Down", 0x2193 }
    };
    for (int i = 0; i < (int)(sizeof(symbols) / sizeof(symbols[0])); i++) {
        if (!_stricmp(key, symbols[i].name)) {
            if (n < 2) return NULL;
            buf[0] = symbols[i].sym;
            buf[1] = 0;
            return buf;
        }
    }
    if (MultiByteToWideChar(CP_UTF8, 0, key, -1, buf, n) <= 0) return NULL;
    return buf;
}

/* The bundled level UI font through GDI+'s private collection, the same file
 * the Mac and iPad register with CoreText; Segoe UI when the file is missing.
 * The symbols do not use it: level-ui.ttf's cmap has no U+21E5 or
 * U+2190..U+2193, so badge_symbol_path draws those five. */
static void badge_font_init(void)
{
    GdiplusStartupInput in = { 1, NULL, FALSE, FALSE };
    if (GdiplusStartup(&g_gdiplus_token, &in, NULL) != Ok) {
        tl_log_line("windows: GDI+ could not start; the key labels are not drawn");
        return;
    }
    g_gdiplus_started = 1;
    char path[MAX_PATH + 32];
    join(path, sizeof(path), g_exe_dir, "level-ui.ttf");
    wchar_t wpath[MAX_PATH + 32];
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES &&
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH + 32) > 0 &&
        GdipNewPrivateFontCollection(&g_badge_fonts) == Ok &&
        GdipPrivateAddFontFile(g_badge_fonts, wpath) == Ok) {
        int n = 0;
        GdipGetFontCollectionFamilyCount(g_badge_fonts, &n);
        if (n > 0) {
            GpFontFamily *list[8];
            int got = 0;
            GdipGetFontCollectionFamilyList(g_badge_fonts, 8, list, &got);
            if (got > 0) { g_badge_family = list[0]; g_badge_level_ui = 1; }
        }
    }
    if (!g_badge_family) GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &g_badge_family);
    tl_log_line("windows: the key labels use %s; Tab and the arrows are host paths",
                g_badge_level_ui ? "level-ui.ttf (fbUsv8C5eI Black)" : "Segoe UI");
}

/* The Mac's Lucida Grande Bold fallback has no Windows counterpart that stays
 * bold inside the Mac's 0.18 em outline, so the five symbols are paths. The
 * measurements are em fractions from the Mac's symbol screenshot: the arrow
 * is 0.70 em long and 0.54 em tall, the head 0.30 em long and the full height,
 * and the shaft 0.22 em thick (a little thicker than Lucida's, so the yellow
 * still reads inside the outline). Tab is the same arrow, shortened, pointing
 * into a 0.16 by 0.50 em bar. The 0.18 shear matches the Mac's italic slant.
 * The path is normalized to (0,0), and the ink's size comes back in ink. */
static GpPath *badge_symbol_path(wchar_t sym, float em, RectF *ink)
{
    float H = 0.54f * em, T = 0.22f * em, HL = 0.30f * em, L = 0.70f * em;
    if (sym == 0x21E5) { L = 0.60f * em; HL = 0.32f * em; } /* Tab's arrow, then the bar */
    float cy = H / 2.0f, half = T / 2.0f;
    float u[7] = { 0.0f, L - HL, L - HL, L, L - HL, L - HL, 0.0f };
    float v[7] = { cy - half, cy - half, 0.0f, cy, H, cy + half, cy + half };
    GpPointF p[7];
    for (int i = 0; i < 7; i++) {
        switch (sym) {
        case 0x2190: p[i].X = L - u[i]; p[i].Y = v[i]; break;         /* Left */
        case 0x2192: case 0x21E5: p[i].X = u[i]; p[i].Y = v[i]; break; /* Right, Tab */
        case 0x2191: p[i].X = v[i]; p[i].Y = L - u[i]; break;         /* Up */
        default:     p[i].X = v[i]; p[i].Y = u[i]; break;              /* Down */
        }
    }
    GpPath *path = NULL;
    if (GdipCreatePath(FillModeWinding, &path) != Ok) return NULL;
    if (GdipAddPathPolygon(path, p, 7) != Ok) {
        GdipDeletePath(path);
        return NULL;
    }
    if (sym == 0x21E5) {
        /* The tip overlaps the bar so the fill joins them and only the outer
         * silhouette gets the dark outline, as in the Mac's reference. */
        if (GdipAddPathRectangle(path, L - 0.02f * em, (H - 0.50f * em) / 2.0f,
                                 0.16f * em, 0.50f * em) != Ok) {
            GdipDeletePath(path);
            return NULL;
        }
    }
    GdipGetPathWorldBounds(path, ink, NULL, NULL);
    float mid = ink->Y + ink->Height / 2.0f;
    GpMatrix *shear = NULL;
    if (GdipCreateMatrix2(1.0f, 0.0f, -0.18f, 1.0f, 0.18f * mid, 0.0f, &shear) == Ok) {
        GdipTransformPath(path, shear);
        GdipDeleteMatrix(shear);
    }
    GdipGetPathWorldBounds(path, ink, NULL, NULL);
    GpMatrix *move = NULL;
    if (GdipCreateMatrix2(1.0f, 0.0f, 0.0f, 1.0f, -ink->X, -ink->Y, &move) == Ok) {
        GdipTransformPath(path, move);
        GdipDeleteMatrix(move);
    }
    return path;
}

/* The glyphs as one path, normalized to (0,0); the ink's size comes back in
 * ink. GDI+ measures in pixels, so the path is at the size it is drawn. */
static GpPath *badge_path(const wchar_t *label, int em, RectF *ink)
{
    if (badge_symbol_label(label)) return badge_symbol_path(label[0], (float)em, ink);
    if (!g_badge_family) return NULL;
    GpPath *path = NULL;
    if (GdipCreatePath(FillModeWinding, &path) != Ok) return NULL;
    RectF box = { 0.0f, 0.0f, 4000.0f, 1000.0f };
    GpStringFormat *fmt = NULL;
    GdipCreateStringFormat(0, LANG_NEUTRAL, &fmt);
    if (fmt) {
        GdipSetStringFormatAlign(fmt, StringAlignmentNear);
        GdipSetStringFormatLineAlign(fmt, StringAlignmentNear);
    }
    GpStatus st = GdipAddPathString(path, label, -1, g_badge_family, FontStyleItalic, (REAL)em, &box, fmt);
    if (fmt) GdipDeleteStringFormat(fmt);
    if (st != Ok) {
        GdipDeletePath(path);
        return NULL;
    }
    GdipGetPathWorldBounds(path, ink, NULL, NULL);
    GpMatrix *m = NULL;
    if (GdipCreateMatrix2(1.0f, 0.0f, 0.0f, 1.0f, -ink->X, -ink->Y, &m) == Ok) {
        GdipTransformPath(path, m);
        GdipDeleteMatrix(m);
    }
    return path;
}

static void badges_hide(void)
{
    if (g_badge_win && g_badge_visible) {
        ShowWindow(g_badge_win, SW_HIDE);
        g_badge_visible = 0;
    }
}

static void badges_reposition(void)
{
    if (!g_badge_win || !g_view || !g_badge_visible) return;
    RECT vr;
    if (!GetWindowRect(g_view, &vr)) return;
    SetWindowPos(g_badge_win, NULL, vr.left, vr.top, vr.right - vr.left, vr.bottom - vr.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

/* One redraw: the snapshot's controls, each with its key in the Mac's style:
 * the key's box is 0.46 of the control's height, 30% wider and 1.3 times as
 * tall as the glyphs, at the control's top right (bottom for the shovel and
 * plant food, left for plant food), with 8% of the control's height at the
 * right edge. The dark shadow is drawn first, shifted em/32 right and down,
 * then the yellow edge and fill; nothing else is painted, so the game shows
 * between and around the letters. */
static void badges_draw(const tl_pvz2_badge_snapshot *s)
{
    if (!g_gdiplus_started) return;
    int w = s->surface_w, h = s->surface_h;
    if (!g_badge_dc) g_badge_dc = CreateCompatibleDC(NULL);
    if (!g_badge_dc) return;
    if (!g_badge_dib || w != g_badge_w || h != g_badge_h) {
        if (g_badge_dib) {
            SelectObject(g_badge_dc, g_badge_old);
            DeleteObject(g_badge_dib);
            g_badge_dib = NULL;
        }
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        g_badge_dib = CreateDIBSection(g_badge_dc, &bi, DIB_RGB_COLORS, &g_badge_bits, NULL, 0);
        if (!g_badge_dib) return;
        g_badge_old = SelectObject(g_badge_dc, g_badge_dib);
        g_badge_w = w;
        g_badge_h = h;
    }
    GpBitmap *bmp = NULL;
    if (GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppPARGB, (BYTE *)g_badge_bits, &bmp) != Ok)
        return;
    GpGraphics *g = NULL;
    if (GdipGetImageGraphicsContext((GpImage *)bmp, &g) != Ok) {
        GdipDisposeImage((GpImage *)bmp);
        return;
    }
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipGraphicsClear(g, 0x00000000);
    for (int i = 0; i < BADGE_COUNT; i++) {
        const int *r = badge_rect(i, s);
        if (!r || r[2] < 16 || r[3] < 16) continue;
        wchar_t text[32];
        const wchar_t *label = badge_label(i, s, text, 32);
        if (!label) continue;
        /* CoreText's 0.46 em is the Mac's layer size; its rendered digit is
         * about 13% larger than GDI+'s path at that factor, and 0.52 matches
         * the reference crop's digit against the packet. */
        int em = (int)(r[3] * 0.52);
        if (em < 10) em = 10;
        if (em > 160) em = 160;
        RectF ink = { 0, 0, 0, 0 };
        GpPath *path = badge_path(label, em, &ink);
        if (!path) continue;
        float box_w = ink.Width + em * 0.3f;
        float box_h = em * 1.3f;
        float x = (float)(r[0] + r[2]) - box_w - r[3] * 0.08f;
        float y = (float)r[1];
        if (i == TL_PVZ2_BADGE_SLOTS || i == TL_PVZ2_BADGE_SLOTS + 2) y += r[3] - box_h;
        if (i == TL_PVZ2_BADGE_SLOTS + 2) x = (float)r[0];
        if (x + box_w > w) x = (float)w - box_w;
        if (y + box_h > h) y = (float)h - box_h;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x + box_w > w || y + box_h > h) {
            GdipDeletePath(path);
            continue;
        }
        float dx = x + (box_w - ink.Width) / 2.0f;
        /* CoreText centers the font's line box, not the ink; the level-ui
         * digits sit about 0.08 em above the ink box's centre. */
        float dy = y + (box_h - ink.Height) / 2.0f - em * 0.08f;
        GpMatrix *m = NULL;
        if (GdipCreateMatrix2(1.0f, 0.0f, 0.0f, 1.0f, dx, dy, &m) == Ok) {
            GdipTransformPath(path, m);
            GdipDeleteMatrix(m);
        }
        /* The Mac's back text layer is offset by 1/32 em; CoreText's dark
         * stroke is a 0.18 em pen centred on the glyph, so the shadow is
         * thin at the top left and about twice as thick at the bottom right. */
        GpPath *shadow = NULL;
        if (GdipClonePath(path, &shadow) == Ok) {
            GpMatrix *shift = NULL;
            if (GdipCreateMatrix2(1.0f, 0.0f, 0.0f, 1.0f, em / 32.0f, em / 32.0f,
                                  &shift) == Ok) {
                GdipTransformPath(shadow, shift);
                GdipDeleteMatrix(shift);
            }
            GpPen *dark = NULL;
            if (GdipCreatePen1(BADGE_DARK, em * 0.18f, UnitPixel, &dark) == Ok) {
                GdipSetPenLineJoin(dark, LineJoinRound);
                GdipSetPenStartCap(dark, LineCapRound);
                GdipSetPenEndCap(dark, LineCapRound);
                GdipDrawPath(g, dark, shadow);
                GdipDeletePen(dark);
            }
            GdipDeletePath(shadow);
        }
        GpPen *edge = NULL;
        if (GdipCreatePen1(BADGE_YELLOW, em * 0.03f, UnitPixel, &edge) == Ok) {
            GdipSetPenLineJoin(edge, LineJoinRound);
            GdipSetPenStartCap(edge, LineCapRound);
            GdipSetPenEndCap(edge, LineCapRound);
            GdipDrawPath(g, edge, path);
            GdipDeletePen(edge);
        }
        GpSolidFill *fill = NULL;
        if (GdipCreateSolidFill(BADGE_YELLOW, &fill) == Ok) {
            GdipFillPath(g, fill, path);
            GdipDeleteBrush((GpBrush *)fill);
        }
        GdipDeletePath(path);
    }
    GdipDeleteGraphics(g);
    GdipDisposeImage((GpImage *)bmp);
    RECT vr;
    if (!GetWindowRect(g_view, &vr)) return;
    POINT dst = { vr.left, vr.top };
    SIZE size = { w, h };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    if (!UpdateLayeredWindow(g_badge_win, NULL, &dst, &size, g_badge_dc, &src, 0, &bf, ULW_ALPHA)) return;
    if (!g_badge_visible) {
        ShowWindow(g_badge_win, SW_SHOWNOACTIVATE);
        g_badge_visible = 1;
    }
}

static void badges_tick(void)
{
    if (!g_show_keys || !atomic_load(&g_ready)) {
        badges_hide();
        return;
    }
    tl_pvz2_badge_snapshot s;
    tl_pvz2_badges(&s);
    if (s.generation == g_badge_generation && s.surface_w == g_badge_src_w && s.surface_h == g_badge_src_h)
        return;
    g_badge_generation = s.generation;
    g_badge_src_w = s.surface_w;
    g_badge_src_h = s.surface_h;
    if (!s.live || s.surface_w != g_sw || s.surface_h != g_sh || s.surface_w <= 0 || s.surface_h <= 0) {
        badges_hide();
        return;
    }
    badges_draw(&s);
}

static void badges_apply_setting(void)
{
    tl_pvz2_set_badges_enabled(g_show_keys);
    g_badge_generation = 0;
    if (g_show_keys) SetTimer(g_hwnd, BADGE_TIMER, 40, NULL);
    else {
        KillTimer(g_hwnd, BADGE_TIMER);
        badges_hide();
    }
}

static LRESULT CALLBACK badge_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(h, msg, wp, lp);
}

/* The overlay is a click-through popup owned by the game's window: it stays
 * above the picture, takes no focus and no mouse, and is hidden when the
 * window is minimized or the board is not live. */
static void make_badge_window(HINSTANCE inst)
{
    static int registered;
    if (!registered) {
        WNDCLASSEXW bc = { sizeof(bc) };
        bc.lpfnWndProc = badge_proc;
        bc.hInstance = inst;
        bc.lpszClassName = L"PvZ2ReflourishedBadges";
        RegisterClassExW(&bc);
        registered = 1;
    }
    g_badge_win = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                  L"PvZ2ReflourishedBadges", L"", WS_POPUP,
                                  0, 0, 1, 1, g_hwnd, NULL, inst, NULL);
}

/* ---------------------------------------------------------- pointer preview
 *
 * The selected plant follows the pointer, and two white bands mark the lawn
 * row and column it would go to. The runtime publishes the selection and the
 * grid; this host owns the pixels, as the Mac's CALayer does.
 *
 * The overlay is a second click-through layered window, below the key badge
 * window (the labels stay readable over the plant). It is drawn only when the
 * pointer, the snapshot, the image or the tile changes; while the pointer is
 * still and the game is idle it does no work.
 *
 * The packet art comes from the player's OBB through the plain C loader. It is
 * loaded once on a worker thread; a missing or bad OBB logs one line and turns
 * the preview off.
 */
#define CURSOR_TIMER 104
#define WM_APP_POINTER_ART (WM_APP + 1)

#define POINTER_ART_NONE 0
#define POINTER_ART_LOADING 1
#define POINTER_ART_READY 2
#define POINTER_ART_FAILED 3

typedef struct {
    int ok;
    pvz2_cursor_images images;
    char why[256];
    double ms;
} pointer_art_result;

typedef struct {
    char path[MAX_PATH + 96];
} pointer_art_job;

typedef struct {
    int dx, dy, dw, dh;
    int band_ok;
    int band[8];              /* left, right, top, bottom, x0, x1, y0, y1 */
    int brightness;
    const pvz2_cursor_image *image;
} pointer_draw_state;

static HWND g_pointer_win;
static int g_pointer_visible;
static int g_pointer_tracking;
static int g_pointer_x, g_pointer_y, g_pointer_inside;
static HDC g_pointer_dc;
static HBITMAP g_pointer_dib;
static HGDIOBJ g_pointer_old;
static void *g_pointer_bits;
static int g_pointer_w, g_pointer_h;

static pvz2_cursor_images g_pointer_art;
static int g_pointer_art_state;
static unsigned g_pointer_art_serial;
static unsigned g_pointer_generation, g_pointer_art_seen;

static GpBitmap *g_plant_bitmap;
static unsigned char *g_plant_pixels;
static const pvz2_cursor_image *g_plant_image;
static char g_plant_key[64];

static int g_pointer_band_valid, g_pointer_band_shown;
static int g_pointer_band[8];
static int g_pointer_band_col = -1, g_pointer_band_row = -1;
static unsigned g_pointer_band_generation;
static int g_pointer_wait_col = -1, g_pointer_wait_row = -1;
static unsigned g_pointer_wait_generation;
static ULONGLONG g_pointer_wait_since;
static pointer_draw_state g_pointer_draw;

static LRESULT CALLBACK pointer_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(h, msg, wp, lp);
}

static void pointer_below_badges(void)
{
    if (g_pointer_win && g_badge_win)
        SetWindowPos(g_pointer_win, g_badge_win, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void pointer_reposition(void)
{
    if (!g_pointer_win || !g_view) return;
    RECT vr;
    if (!GetWindowRect(g_view, &vr)) return;
    SetWindowPos(g_pointer_win, NULL, vr.left, vr.top, vr.right - vr.left, vr.bottom - vr.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_pointer_w != g_sw || g_pointer_h != g_sh)
        memset(&g_pointer_draw, 0, sizeof g_pointer_draw);
}

static void pointer_hide(void)
{
    if (g_pointer_win && g_pointer_visible) {
        ShowWindow(g_pointer_win, SW_HIDE);
        g_pointer_visible = 0;
    }
    memset(&g_pointer_draw, 0, sizeof g_pointer_draw);
    g_pointer_band_valid = 0;
    g_pointer_band_shown = 0;
    g_pointer_wait_col = g_pointer_wait_row = -1;
    tl_pvz2_cursor_target(0, 0, 0);
}

static void make_pointer_window(HINSTANCE inst)
{
    static int registered;
    if (!registered) {
        WNDCLASSEXW pc = { sizeof(pc) };
        pc.lpfnWndProc = pointer_proc;
        pc.hInstance = inst;
        pc.lpszClassName = L"PvZ2ReflourishedPointer";
        RegisterClassExW(&pc);
        registered = 1;
    }
    g_pointer_win = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                                    WS_EX_NOACTIVATE, L"PvZ2ReflourishedPointer", L"", WS_POPUP,
                                    0, 0, 1, 1, g_hwnd, NULL, inst, NULL);
    if (!g_pointer_win)
        tl_log_line("windows: the pointer window could not be made (%lu)", GetLastError());
}

static int pointer_ensure_dib(int w, int h)
{
    if (!g_pointer_dc) g_pointer_dc = CreateCompatibleDC(NULL);
    if (!g_pointer_dc) return 0;
    if (g_pointer_dib && w == g_pointer_w && h == g_pointer_h) return 1;
    if (g_pointer_dib) {
        SelectObject(g_pointer_dc, g_pointer_old);
        DeleteObject(g_pointer_dib);
        g_pointer_dib = NULL;
    }
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_pointer_dib = CreateDIBSection(g_pointer_dc, &bi, DIB_RGB_COLORS, &g_pointer_bits, NULL, 0);
    if (!g_pointer_dib) return 0;
    g_pointer_old = SelectObject(g_pointer_dc, g_pointer_dib);
    g_pointer_w = w;
    g_pointer_h = h;
    memset(&g_pointer_draw, 0, sizeof g_pointer_draw);
    return 1;
}

static void pointer_clear_bitmap(void)
{
    if (g_plant_bitmap) {
        GdipDisposeImage((GpImage *)g_plant_bitmap);
        g_plant_bitmap = NULL;
    }
    free(g_plant_pixels);
    g_plant_pixels = NULL;
    g_plant_image = NULL;
    g_plant_key[0] = 0;
}

/* GDI+ holds the pixels in BGRA order and uses premultiplied alpha. The loader
 * gives premultiplied RGBA; the fixed 0.6 preview opacity is folded into the
 * cached bitmap here, so the 60 Hz draw is a plain image blit. */
static int pointer_set_bitmap(const pvz2_cursor_image *image, const char *key)
{
    if (image == g_plant_image && g_plant_bitmap && !strcmp(key, g_plant_key)) return 1;
    pointer_clear_bitmap();
    if (!image || image->width <= 0 || image->height <= 0) return 0;
    size_t n = (size_t)image->width * (size_t)image->height;
    g_plant_pixels = malloc(n * 4);
    if (!g_plant_pixels) return 0;
    const unsigned char *src = image->rgba;
    unsigned char *dst = g_plant_pixels;
    for (size_t i = 0; i < n; i++, src += 4, dst += 4) {
        dst[0] = (unsigned char)((src[2] * 153u + 127u) / 255u);
        dst[1] = (unsigned char)((src[1] * 153u + 127u) / 255u);
        dst[2] = (unsigned char)((src[0] * 153u + 127u) / 255u);
        dst[3] = (unsigned char)((src[3] * 153u + 127u) / 255u);
    }
    if (GdipCreateBitmapFromScan0(image->width, image->height, image->width * 4,
                                  PixelFormat32bppPARGB, (BYTE *)g_plant_pixels,
                                  &g_plant_bitmap) != Ok) {
        pointer_clear_bitmap();
        return 0;
    }
    g_plant_image = image;
    snprintf(g_plant_key, sizeof g_plant_key, "%s", key);
    return 1;
}

static const pvz2_cursor_image *pointer_find(const char *key)
{
    if (g_pointer_art_state != POINTER_ART_READY) return NULL;
    for (int i = 0; i < g_pointer_art.count; i++)
        if (!strcmp(g_pointer_art.items[i].id, key)) return &g_pointer_art.items[i];
    return NULL;
}

static int pointer_key_for(const tl_pvz2_cursor_snapshot *s, char *key, size_t n)
{
    if (s->kind == TL_PVZ2_CURSOR_PLANT) {
        char up[32];
        int i = 0;
        for (; s->plant[i] && i < (int)sizeof(up) - 1; i++)
            up[i] = (char)toupper((unsigned char)s->plant[i]);
        up[i] = 0;
        snprintf(key, n, "IMAGE_UI_PACKETS_%s", up);
        return 1;
    }
    if (s->kind == TL_PVZ2_CURSOR_SHOVEL) {
        snprintf(key, n, "IMAGE_UI_HUD_INGAME_SHOVEL_ICON");
        return 1;
    }
    if (s->kind == TL_PVZ2_CURSOR_PLANT_FOOD) {
        snprintf(key, n, "IMAGE_UI_HUD_INGAME_PLANTFOOD_BUTTON");
        return 1;
    }
    return 0;
}

static void pointer_update_image(const tl_pvz2_cursor_snapshot *s)
{
    char key[64];
    if (!pointer_key_for(s, key, sizeof key)) {
        pointer_clear_bitmap();
        return;
    }
    pointer_set_bitmap(pointer_find(key), key);
    if (getenv("TL_PVZ2_CURSOR_STATS"))
        tl_log_line("windows: pointer preview: kind %d plant %s", s->kind,
                    s->plant[0] ? s->plant : "none");
}

static void pointer_art_done(pointer_art_result *r)
{
    pointer_clear_bitmap();
    if (g_pointer_art_state == POINTER_ART_READY) pvz2_cursor_images_free(&g_pointer_art);
    if (r->ok) {
        g_pointer_art = r->images;
        g_pointer_art_state = POINTER_ART_READY;
        g_pointer_art_serial++;
        size_t bytes = 0;
        for (int i = 0; i < r->images.count; i++)
            bytes += (size_t)r->images.items[i].width * r->images.items[i].height * 4;
        tl_log_line("windows: pointer art: %d images loaded in %.1f ms (%zu bytes)",
                    r->images.count, r->ms, bytes);
    } else {
        g_pointer_art_state = POINTER_ART_FAILED;
        if (g_hwnd) KillTimer(g_hwnd, CURSOR_TIMER);
        pointer_hide();
        tl_log_line("windows: pointer art: %s", r->why[0] ? r->why : "could not read the OBB");
    }
    g_pointer_generation = 0;
    free(r);
}

static DWORD WINAPI pointer_art_worker(void *argument)
{
    pointer_art_job *job = argument;
    pointer_art_result *r = calloc(1, sizeof *r);
    if (!r) {
        free(job);
        return 0;
    }
    ULONGLONG started = GetTickCount64();
    r->ok = pvz2_cursor_images_load(job->path, &r->images, r->why, sizeof r->why);
    r->ms = (double)(GetTickCount64() - started);
    free(job);
    if (!PostMessageA(g_hwnd, WM_APP_POINTER_ART, 0, (LPARAM)r)) {
        pvz2_cursor_images_free(&r->images);
        free(r);
    }
    return 0;
}

static void pointer_load_art_once(void)
{
    if (g_pointer_art_state != POINTER_ART_NONE) return;
    g_pointer_art_state = POINTER_ART_LOADING;
    char dir[MAX_PATH + 64], path[MAX_PATH * 2];
    join(dir, sizeof dir, g_data, "sdcard/Android/obb/com.ea.game.pvz2_rfl");
    join(path, sizeof path, dir, PVZ2_OBB_NAME);
    WIN32_FILE_ATTRIBUTE_DATA attrs;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attrs)) {
        g_pointer_art_state = POINTER_ART_FAILED;
        tl_log_line("windows: pointer art: the OBB is missing (%s)", path);
        return;
    }
    pointer_art_job *job = malloc(sizeof *job);
    if (!job) {
        g_pointer_art_state = POINTER_ART_FAILED;
        tl_log_line("windows: pointer art: out of memory");
        return;
    }
    snprintf(job->path, sizeof job->path, "%s", path);
    HANDLE thread = CreateThread(NULL, 0, pointer_art_worker, job, 0, NULL);
    if (!thread) {
        free(job);
        g_pointer_art_state = POINTER_ART_FAILED;
        tl_log_line("windows: pointer art: the loader thread could not start");
        return;
    }
    CloseHandle(thread);
}

static void pointer_apply_setting(void)
{
    if (!g_pointer_win) {
        tl_log_line("windows: pointer preview is off: no overlay window");
        return;
    }
    tl_pvz2_set_cursor_enabled(g_show_cursor_plant);
    if (!g_show_cursor_plant) {
        if (g_hwnd) KillTimer(g_hwnd, CURSOR_TIMER);
        pointer_hide();
        tl_log_line("windows: pointer preview is off");
        return;
    }
    pointer_load_art_once();
    if (g_hwnd) SetTimer(g_hwnd, CURSOR_TIMER, 16, NULL);
    tl_log_line("windows: pointer preview is on, cross brightness %d%%", g_cross_brightness);
}

/* The Mac's binary search over the surface extent: the pixel where the mapped
 * band number reaches value. Zero row offset, as the row band must be. */
static int pointer_band_axis(const tl_pvz2_cursor_snapshot *s, int axis, int value)
{
    int lo = 0, hi = axis ? s->surface_h : s->surface_w;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (tl_cursor_grid_band_axis(s, axis, mid) < value) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static void pointer_band_rect(const tl_pvz2_cursor_snapshot *s, int col, int row, int band[8])
{
    band[0] = pointer_band_axis(s, 0, 0);
    band[1] = pointer_band_axis(s, 0, s->columns);
    band[2] = pointer_band_axis(s, 1, 0);
    band[3] = pointer_band_axis(s, 1, s->rows);
    band[4] = pointer_band_axis(s, 0, col);
    band[5] = pointer_band_axis(s, 0, col + 1);
    band[6] = pointer_band_axis(s, 1, row);
    band[7] = pointer_band_axis(s, 1, row + 1);
}

static int pointer_draw(const pointer_draw_state *state, const int band[8], int band_ok)
{
    if (!pointer_ensure_dib(g_sw, g_sh)) return 0;
    GpBitmap *bitmap = NULL;
    if (GdipCreateBitmapFromScan0(g_pointer_w, g_pointer_h, g_pointer_w * 4,
                                  PixelFormat32bppPARGB, (BYTE *)g_pointer_bits, &bitmap) != Ok)
        return 0;
    GpGraphics *g = NULL;
    if (GdipGetImageGraphicsContext((GpImage *)bitmap, &g) != Ok) {
        GdipDisposeImage((GpImage *)bitmap);
        return 0;
    }
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    GdipGraphicsClear(g, 0);
    if (band_ok) {
        GpPath *path = NULL;
        if (GdipCreatePath(FillModeWinding, &path) == Ok) {
            GdipAddPathRectangle(path, (REAL)band[0], (REAL)band[6],
                                 (REAL)(band[1] - band[0]), (REAL)(band[7] - band[6]));
            GdipAddPathRectangle(path, (REAL)band[4], (REAL)band[2],
                                 (REAL)(band[5] - band[4]), (REAL)(band[3] - band[2]));
            unsigned alpha = (unsigned)((state->brightness * 255 + 50) / 100);
            if (alpha > 255) alpha = 255;
            GpSolidFill *fill = NULL;
            if (GdipCreateSolidFill((ARGB)((alpha << 24) | 0xFFFFFF), &fill) == Ok) {
                GdipFillPath(g, fill, path);
                GdipDeleteBrush((GpBrush *)fill);
            }
            GdipDeletePath(path);
        }
    }
    if (g_plant_bitmap) {
        GdipSetInterpolationMode(g, InterpolationModeHighQualityBilinear);
        GdipDrawImageRect(g, (GpImage *)g_plant_bitmap, (REAL)state->dx, (REAL)state->dy,
                          (REAL)state->dw, (REAL)state->dh);
    }
    GdipDeleteGraphics(g);
    GdipDisposeImage((GpImage *)bitmap);

    RECT vr;
    if (!GetWindowRect(g_view, &vr)) return 0;
    POINT dst = { vr.left, vr.top };
    SIZE size = { g_sw, g_sh };
    POINT src = { 0, 0 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    return UpdateLayeredWindow(g_pointer_win, NULL, &dst, &size, g_pointer_dc, &src, 0,
                               &blend, ULW_ALPHA) != 0;
}

static void pointer_tick(void)
{
    if (!g_show_cursor_plant || !g_ready || !g_gdiplus_started || !g_pointer_win) {
        pointer_hide();
        return;
    }
    tl_pvz2_cursor_snapshot s;
    tl_pvz2_cursor(&s);
    if (!s.live || s.kind == TL_PVZ2_CURSOR_NONE || s.surface_w <= 0 || s.surface_h <= 0 ||
        s.surface_w != g_sw || s.surface_h != g_sh) {
        pointer_hide();
        return;
    }

    int inside = 0, got = 0, sx = 0, sy = 0;
    POINT pt;
    if (GetCursorPos(&pt) && ScreenToClient(g_view, &pt)) {
        got = 1;
        RECT rc;
        GetClientRect(g_view, &rc);
        if (pt.x >= 0 && pt.y >= 0 && pt.x < rc.right && pt.y < rc.bottom) {
            inside = 1;
            sx = pt.x;
            sy = pt.y;
        }
    }
    if (!got && g_pointer_inside) {
        inside = 1;
        sx = g_pointer_x;
        sy = g_pointer_y;
    }
    if (!inside) {
        pointer_hide();
        return;
    }

    if (s.generation != g_pointer_generation || g_pointer_art_serial != g_pointer_art_seen) {
        g_pointer_generation = s.generation;
        g_pointer_art_seen = g_pointer_art_serial;
        pointer_update_image(&s);
    }
    if (!g_plant_bitmap || !g_plant_image) {
        pointer_hide();
        return;
    }

    int col = -1, row = -1, in_bounds = 0, cross = 0, keep = 0;
    int band[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    if (s.grid_valid) {
        col = tl_cursor_grid_axis(&s, 0, (int)(sx + 0.5f));
        row = tl_cursor_grid_axis(&s, 1, (int)(sy + 0.5f));
        in_bounds = col >= 0 && col < s.columns && row >= 0 && row < s.rows;
    }
    if (in_bounds) {
        tl_pvz2_cursor_target((int)(sx + 0.5f), (int)(sy + 0.5f),
                              s.kind == TL_PVZ2_CURSOR_PLANT);
        if (s.kind == TL_PVZ2_CURSOR_PLANT) {
            int answer = s.placement_known && s.placement_col == col && s.placement_row == row;
            if (answer) {
                g_pointer_wait_col = g_pointer_wait_row = -1;
                cross = s.placement_allowed;
                if (cross) {
                    pointer_band_rect(&s, col, row, band);
                    memcpy(g_pointer_band, band, sizeof band);
                    g_pointer_band_col = col;
                    g_pointer_band_row = row;
                    g_pointer_band_generation = s.generation;
                    g_pointer_band_valid = 1;
                }
            } else {
                if (g_pointer_wait_col != col || g_pointer_wait_row != row ||
                    g_pointer_wait_generation != s.generation) {
                    g_pointer_wait_col = col;
                    g_pointer_wait_row = row;
                    g_pointer_wait_generation = s.generation;
                    g_pointer_wait_since = GetTickCount64();
                }
                keep = g_pointer_band_shown && g_pointer_band_valid &&
                       s.placement_known && s.placement_allowed &&
                       s.placement_col == g_pointer_band_col &&
                       s.placement_row == g_pointer_band_row &&
                       g_pointer_band_generation == s.generation &&
                       GetTickCount64() - g_pointer_wait_since < 100;
                if (keep)
                    memcpy(band, g_pointer_band, sizeof band);
                else
                    g_pointer_band_valid = 0;
            }
        } else {
            cross = 1;
            pointer_band_rect(&s, col, row, band);
            memcpy(g_pointer_band, band, sizeof band);
            g_pointer_band_col = col;
            g_pointer_band_row = row;
            g_pointer_band_generation = s.generation;
            g_pointer_band_valid = 1;
        }
    } else {
        tl_pvz2_cursor_target(0, 0, 0);
        g_pointer_wait_col = g_pointer_wait_row = -1;
    }
    int band_ok = cross || keep;
    g_pointer_band_shown = band_ok;
    if (!band_ok) g_pointer_band_valid = 0;
    static int pointer_stats = -1;
    if (pointer_stats < 0) pointer_stats = getenv("TL_PVZ2_CURSOR_STATS") != NULL;
    if (pointer_stats) {
        static ULONGLONG last_log;
        ULONGLONG now = GetTickCount64();
        if (now - last_log >= 500) {
            last_log = now;
            tl_log_line("windows: pointer state: %d,%d tile %d,%d known %d allowed %d col,row %d,%d cross %d keep %d",
                        sx, sy, col, row, s.placement_known, s.placement_allowed,
                        s.placement_col, s.placement_row, cross, keep);
        }
    }

    double row_height = 0;
    if (s.grid_valid && s.app_h > 0 && s.app_scale > 0 && s.camera_scale > 0)
        row_height = 76.0 * s.app_scale * s.camera_scale * (double)g_sh / (double)s.app_h;
    double side = row_height > 0 ? row_height * 0.65 : (double)g_sh * 0.14;
    if (!(side > 0)) side = 1;
    int long_side = g_plant_image->width > g_plant_image->height
                        ? g_plant_image->width : g_plant_image->height;
    double scale = side / (double)long_side;
    int dw = (int)((double)g_plant_image->width * scale + 0.5);
    int dh = (int)((double)g_plant_image->height * scale + 0.5);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int dx = (int)(sx - dw / 2.0);
    int dy = (int)(sy - dh / 2.0);

    pointer_draw_state next;
    memset(&next, 0, sizeof next);
    next.dx = dx;
    next.dy = dy;
    next.dw = dw;
    next.dh = dh;
    next.band_ok = band_ok;
    memcpy(next.band, band, sizeof band);
    next.brightness = g_cross_brightness;
    next.image = g_plant_image;

    if (g_pointer_w != g_sw || g_pointer_h != g_sh)
        memset(&g_pointer_draw, 0, sizeof g_pointer_draw);
    if (!g_pointer_visible || memcmp(&next, &g_pointer_draw, sizeof next) != 0) {
        LARGE_INTEGER begin, end, freq;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&begin);
        int drawn = pointer_draw(&next, band, band_ok);
        QueryPerformanceCounter(&end);
        if (!drawn) {
            pointer_hide();
            return;
        }
        g_pointer_draw = next;
        if (pointer_stats) {
            static double total_ms, worst_ms;
            static unsigned updates;
            double ms = (double)(end.QuadPart - begin.QuadPart) * 1000.0 /
                        (double)(freq.QuadPart ? freq.QuadPart : 1);
            total_ms += ms;
            if (ms > worst_ms) worst_ms = ms;
            if (++updates == 300) {
                tl_log_line("windows: pointer draw: %.3f ms average, %.3f ms worst (%u updates)",
                            total_ms / updates, worst_ms, updates);
                updates = 0;
                total_ms = worst_ms = 0;
            }
        }
    }
    if (!g_pointer_visible) {
        ShowWindow(g_pointer_win, SW_SHOWNOACTIVATE);
        g_pointer_visible = 1;
        pointer_below_badges();
    }
}

static void touch(int phase, LPARAM lp)
{
    if (!atomic_load(&g_ready)) return;
    RECT rc; GetClientRect(g_view, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;
    int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
    /* A drag that leaves the picture still belongs to the finger that went
     * down in it: it stays at the picture's edge (the Mac keeps the last point
     * in the picture). */
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= cw) mx = cw - 1;
    if (my >= ch) my = ch - 1;
    float x = (float)mx * (float)g_sw / (float)cw;
    float y = (float)my * (float)g_sh / (float)ch;
    if (phase == 0) { tl_pvz2_keyboard_dismissed(); g_down = true; }
    else if (phase == 3) g_down = false;
    tl_pvz2_touch(phase, 1, x, y);
}

static LRESULT CALLBACK view_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_LBUTTONDOWN:
        SetFocus(GetParent(h));
        SetCapture(h);
        touch(0, lp);
        return 0;
    case WM_MOUSEMOVE:
        g_pointer_x = (int)(short)LOWORD(lp);
        g_pointer_y = (int)(short)HIWORD(lp);
        g_pointer_inside = 1;
        if (!g_pointer_tracking) {
            TRACKMOUSEEVENT tm = { sizeof(tm), TME_LEAVE, h, 0 };
            if (TrackMouseEvent(&tm)) g_pointer_tracking = 1;
        }
        if (g_down) touch(1, lp);
        return 0;
    case WM_MOUSELEAVE:
        g_pointer_tracking = 0;
        g_pointer_inside = 0;
        pointer_hide();
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        touch(3, lp);
        return 0;
    case WM_ERASEBKGND:
        /* ANGLE presents every frame; GDI painting here would only flash. */
        return 1;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void make_view(HINSTANCE inst)
{
    static int registered;
    if (!registered) {
        WNDCLASSEXW vc = { sizeof(vc) };
        vc.style = CS_OWNDC;
        vc.lpfnWndProc = view_proc;
        vc.hInstance = inst;
        vc.hCursor = LoadCursor(NULL, IDC_ARROW);
        vc.lpszClassName = L"PvZ2ReflourishedView";
        RegisterClassExW(&vc);
        registered = 1;
    }
    g_view = CreateWindowExW(0, L"PvZ2ReflourishedView", L"", WS_CHILD | WS_VISIBLE,
                             0, 0, 1, 1, g_hwnd, NULL, inst, NULL);
    view_layout();
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_F11) { toggle_fullscreen(!g_fullscreen); return 0; }
        if (wp == VK_OEM_COMMA && GetKeyState(VK_CONTROL) < 0) { open_settings(); return 0; }
        if (key_down(wp, lp)) return 0;
        break;
    case WM_SYSKEYDOWN:
        /* Alt+Enter: full screen, as every game has it. Alt alone opens the
         * window menu, which is where Settings... lives. */
        if (wp == VK_RETURN && (GetKeyState(VK_MENU) < 0)) { toggle_fullscreen(!g_fullscreen); return 0; }
        break;
    case WM_CHAR: typed(wp); return 0;
    case WM_SIZE:
        if (h == g_hwnd && wp == SIZE_MINIMIZED) {
            g_badge_generation = 0;
            badges_hide();
            pointer_hide();
        } else if (h == g_hwnd) {
            view_layout();
            pointer_reposition();
            InvalidateRect(h, NULL, TRUE);
        }
        return 0;
    case WM_MOVE:
        badges_reposition();
        pointer_reposition();
        return 0;
    case WM_TIMER:
        if (wp == BADGE_TIMER) {
            badges_tick();
            return 0;
        }
        if (wp == CURSOR_TIMER) {
            pointer_tick();
            return 0;
        }
        break;
    case WM_APP_POINTER_ART:
        pointer_art_done((pointer_art_result *)lp);
        return 0;
    case WM_EXITSIZEMOVE:
        if (!g_fullscreen) save_geometry();
        return 0;
    case WM_GETMINMAXINFO:
        if (!g_fullscreen) {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            RECT r = { 0, 0, MIN_CLIENT_W, MIN_CLIENT_H };
            AdjustWindowRectExForDpi(&r, WINDOW_STYLE, FALSE, 0, GetDpiForSystem());
            mm->ptMinTrackSize.x = r.right - r.left;
            mm->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == ID_SETTINGS) { open_settings(); return 0; }
        break;
    case WM_LBUTTONDOWN:
        /* A click on a bar: the window takes the focus, the game gets no
         * touch (the picture is the child window). */
        SetFocus(h);
        return 0;
    case WM_ERASEBKGND:
    case WM_PAINT: {
        /* The bars are the window's own background: black, and repainted when
         * the window changes size, never the game's stale pixels. */
        PAINTSTRUCT ps;
        HDC dc = msg == WM_PAINT ? BeginPaint(h, &ps) : (HDC)wp;
        RECT r;
        GetClientRect(h, &r);
        FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (msg == WM_PAINT) EndPaint(h, &ps);
        return 1;
    }
    case WM_CLOSE:
        /* Android's way out: the game hears it is going to the background, and saves. */
        save_geometry();
        tl_log_line("windows: the window is closing; the game goes to the background first");
        if (atomic_load(&g_ready)) { tl_pvz2_lifecycle_background(); Sleep(1500); }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND make_window(HINSTANCE inst)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"PvZ2Reflourished";
    RegisterClassExW(&wc);

    int maximized = 0;
    RECT wr;
    initial_placement(&wr, &maximized);
    g_windowed_placement = (WINDOWPLACEMENT){ .length = sizeof(WINDOWPLACEMENT) };
    g_windowed_placement.showCmd = maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    g_windowed_placement.rcNormalPosition = wr;
    g_windowed_style = WINDOW_STYLE;

    HWND parent;
    if (g_start_fullscreen) {
        HMONITOR mon = monitor_for(&wr);
        MONITORINFO mi = { .cbSize = sizeof(mi) };
        if (GetMonitorInfoW(mon, &mi)) {
            g_fullscreen = 1;
            parent = CreateWindowExW(0, wc.lpszClassName, L"PvZ2 Reflourished", WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN,
                                     mi.rcMonitor.left, mi.rcMonitor.top,
                                     mi.rcMonitor.right - mi.rcMonitor.left,
                                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                                     NULL, NULL, inst, NULL);
            if (parent) { g_hwnd = parent; startup_layout_from_window(); make_view(inst); make_badge_window(inst); }
            return parent;
        }
    }
    g_fullscreen = 0;
    parent = CreateWindowExW(0, wc.lpszClassName, L"PvZ2 Reflourished", WINDOW_STYLE,
                             wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
                             NULL, NULL, inst, NULL);
    if (parent) { g_hwnd = parent; startup_layout_from_window(); make_view(inst); make_badge_window(inst); }
    return parent;
}

/* ------------------------------------------------------- the first x64 start */

#if defined(__x86_64__)
/* The translation is cached as a module in the data folder: a .dll of about
 * 200 MB. Its absence is the one thing that means the next start translates. */
static int translation_needed(void)
{
    char pat[MAX_PATH + 32];
    join(pat, sizeof(pat), g_data, "translated/*.dll");
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return 1;
    int found = 0;
    do {
        if (fd.nFileSizeHigh > 0 || fd.nFileSizeLow > (16u << 20)) { found = 1; break; }
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    return !found;
}

static LRESULT CALLBACK progress_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp; (void)lp;
    switch (msg) {
    case WM_CREATE:
        SetTimer(h, 1, 100, NULL);
        return 0;
    case WM_TIMER:
        if (tl_pvz2_frames() > 0 || atomic_load(&g_driver_done)) {
            KillTimer(h, 1);
            DestroyWindow(h);
        }
        return 0;
    case WM_CLOSE:
        /* The translation is not something to leave half done. */
        return 0;
    case WM_DESTROY:
        if (g_progress == h) g_progress = NULL;
        if (g_progress_font) { DeleteObject(g_progress_font); g_progress_font = NULL; }
        tl_log_line("windows: the game is drawing; the preparing window is gone");
        return 0;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void show_progress_window(HINSTANCE inst)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = progress_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"PvZ2ReflourishedPrepare";
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    RECT r = { 0, 0, MulDiv(460, dpi, 96), MulDiv(120, dpi, 96) };
    AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU, FALSE, 0, dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    RECT game;
    GetWindowRect(g_hwnd, &game);
    int x = game.left + ((game.right - game.left) - w) / 2;
    int y = game.top + ((game.bottom - game.top) - h) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"PvZ2ReflourishedPrepare", L"PvZ2 Reflourished",
                                WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h, g_hwnd, NULL, inst, NULL);
    if (!hwnd) return;
    NONCLIENTMETRICSW ncm = { .cbSize = sizeof(ncm) };
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi))
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    g_progress_font = font;
    HWND text = CreateWindowExW(0, L"STATIC", L"Preparing the game for this PC. This happens once.",
                                WS_CHILD | WS_VISIBLE, MulDiv(16, dpi, 96), MulDiv(16, dpi, 96),
                                MulDiv(420, dpi, 96), MulDiv(22, dpi, 96), hwnd, NULL, inst, NULL);
    HWND bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
                               MulDiv(16, dpi, 96), MulDiv(52, dpi, 96), MulDiv(420, dpi, 96), MulDiv(20, dpi, 96),
                               hwnd, NULL, inst, NULL);
    HWND note = CreateWindowExW(0, L"STATIC", L"It can take several minutes.", WS_CHILD | WS_VISIBLE,
                                MulDiv(16, dpi, 96), MulDiv(80, dpi, 96), MulDiv(420, dpi, 96), MulDiv(20, dpi, 96),
                                hwnd, NULL, inst, NULL);
    SendMessageW(text, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(bar, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(note, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(bar, PBM_SETMARQUEE, TRUE, 30);
    g_progress = hwnd;
    tl_log_line("windows: the first start translates the game; the preparing window is up");
}
#endif

/* ------------------------------------------------------------------- start */

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    tl_win_install_crash_handler();
    INITCOMMONCONTROLSEX icc = { .dwSize = sizeof(icc), .dwICC = ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    GetModuleFileNameA(NULL, g_exe_dir, sizeof(g_exe_dir));
    slashes(g_exe_dir);
    char *s = strrchr(g_exe_dir, '/'); if (s) *s = 0;

    /* The three folders. TL_DATA wins over everything, and then the saves and
     * the settings live in it too, so a test leaves nothing behind. */
    const char *tl = getenv("TL_DATA");
    const char *mode = "normal";
    char portable[MAX_PATH + 16];
    join(portable, sizeof(portable), g_exe_dir, "portable");
    if (tl && *tl) {
        snprintf(g_data, sizeof(g_data), "%s", tl); slashes(g_data);
        snprintf(g_saves, sizeof(g_saves), "%s", g_data);
        join(g_settings_path, sizeof(g_settings_path), g_data, "settings.ini");
        mode = "TL_DATA (everything in one folder)";
    } else if (GetFileAttributesA(portable) != INVALID_FILE_ATTRIBUTES) {
        join(g_data, sizeof(g_data), g_exe_dir, "data"); slashes(g_data);
        snprintf(g_saves, sizeof(g_saves), "%s", g_data);
        join(g_settings_path, sizeof(g_settings_path), g_data, "settings.ini");
        mode = "portable (a \"portable\" file is beside the program)";
    } else {
        char roaming[MAX_PATH] = ".", local[MAX_PATH] = ".";
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, roaming))) slashes(roaming);
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local))) slashes(local);
        join(g_saves, sizeof(g_saves), roaming, "PvZ2 Reflourished");
        join(g_data, sizeof(g_data), local, "PvZ2 Reflourished");
        join(g_settings_path, sizeof(g_settings_path), g_saves, "settings.ini");
    }
    slashes(g_saves);
    slashes(g_settings_path);
    tl_win_mkdirs(g_data);
    tl_win_mkdirs(g_saves);
    {
        char logp[MAX_PATH + 16];
        join(logp, sizeof(logp), g_data, "pvz2.log");
        tl_win_log_open(logp);
    }
    tl_log_line("windows: %s mode", mode);
    tl_log_line("windows: settings file %s", g_settings_path);
    tl_log_line("windows: the game's saves %s", g_saves);
    tl_log_line("windows: the game's data %s (the OBB, the CDN archive and the x64 cache)", g_data);
    tl_win_set_saves_dir(g_saves);

    /* The game's data before the game: a slim copy carries no OBB and fetches
     * it on the first start, as the Mac and iPad do. */
    if (!tl_win_obb_ensure(g_data, g_exe_dir)) {
        tl_log_line("windows: the game is not started: its data was not downloaded");
        return 1;
    }

    pvz2_settings_load(g_settings_path, &g_settings);
    g_start_fullscreen = g_settings.start_fullscreen;
    g_adapt = g_settings.adapt_shape;
    g_show_keys = g_settings.show_level_keys;
    g_show_cursor_plant = g_settings.show_cursor_plant;
    g_cross_brightness = g_settings.cursor_cross_brightness;

    snprintf(g_apk, sizeof(g_apk), "%s", env_or("TL_PVZ2_APK", "")); 
    if (!g_apk[0]) { char p[MAX_PATH + 32]; join(p, sizeof(p), g_exe_dir, "Game/pvz2.apk"); snprintf(g_apk, sizeof(g_apk), "%s", p); }
    snprintf(g_egl, sizeof(g_egl), "%s", env_or("TL_ANGLE_EGL", ""));
    if (!g_egl[0]) { char p[MAX_PATH + 32]; join(p, sizeof(p), g_exe_dir, "libEGL.dll"); snprintf(g_egl, sizeof(g_egl), "%s", p); }
    snprintf(g_gles, sizeof(g_gles), "%s", env_or("TL_ANGLE_GLES", ""));
    if (!g_gles[0]) { char p[MAX_PATH + 32]; join(p, sizeof(p), g_exe_dir, "libGLESv2.dll"); snprintf(g_gles, sizeof(g_gles), "%s", p); }
    tl_log_line("windows: data %s, APK %s, ANGLE %s", g_data, g_apk, g_egl);

    /* The CDN fallback archive, staged into the data folder unless a newer download is there. */
    {
        char bundled[MAX_PATH + 32], cached[MAX_PATH + 32], dir[MAX_PATH + 32];
        join(bundled, sizeof(bundled), g_exe_dir, "Game/1.4.2.main.tar");
        join(dir, sizeof(dir), g_data, "No_Backup"); tl_win_mkdirs(dir);
        join(cached, sizeof(cached), g_data, "No_Backup/1.4.2.main.tar");
        if (!getenv("TL_PVZ2_MAIN_TAR")) { tl_cdn_stage(bundled, cached); _putenv_s("TL_PVZ2_MAIN_TAR", cached); }
    }

    char err[200] = "";
    if (!tl_xmem_open((size_t)2048 << 20, err, sizeof(err))) {
        tl_log_line("windows: no executable memory: %s", err);
        MessageBoxA(NULL, err, "PvZ2 Reflourished", MB_ICONERROR);
        return 1;
    }
#if defined(__x86_64__)
    /* x86-64: the stub page the translated code reaches the host through (husk-tl-x64.c) */
    if (!tl_x64_open(err, sizeof(err))) {
        tl_log_line("windows: %s", err);
        MessageBoxA(NULL, err, "PvZ2 Reflourished", MB_ICONERROR);
        return 1;
    }
#endif
    tl_ld_set_verbosity(getenv("TL_VERBOSE") ? atoi(getenv("TL_VERBOSE")) : 1);
    tl_jni_set_trace(getenv("TL_JNI_TRACE") ? atoi(getenv("TL_JNI_TRACE")) : 1);

    g_hwnd = make_window(inst);
    if (!g_hwnd) { tl_log_line("windows: CreateWindow failed (%lu)", GetLastError()); return 1; }
    make_pointer_window(inst);
    pointer_reposition();
    pointer_below_badges();
    /* The window is created at its final size; ShowWindow only shows it, so the
     * game never sees a different size while it starts. */
    ShowWindow(g_hwnd, g_fullscreen ? SW_SHOW : (g_windowed_placement.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : show));
    UpdateWindow(g_hwnd);
    /* The shown window can be a different size than the created one (a
     * maximized start): the shape the game starts in is chosen from it. */
    startup_layout_from_window();
    view_layout();
    RECT rc; GetClientRect(g_hwnd, &rc);
    int cw = (int)(rc.right - rc.left), ch = (int)(rc.bottom - rc.top);
    tl_log_line("windows: window %dx%d client, the game draws at %dx%d", cw, ch, g_sw, g_sh);

    /* The window menu: Settings..., then a separator, as the player's mock-up
     * had it. Windows' own items (Move, Size, Close) follow it. */
    HMENU sys = GetSystemMenu(g_hwnd, FALSE);
    if (sys) {
        InsertMenuW(sys, 0, MF_BYPOSITION | MF_STRING, ID_SETTINGS, L"Settings...\tCtrl+,");
        InsertMenuW(sys, 1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    }

    g_cfg = (tl_ga_config){ .apk_path = g_apk, .data_dir = g_data, .package_name = "com.ea.game.pvz2_rfl",
                            .width = g_sw, .height = g_sh, .metal_layer = g_view, .angle_egl = g_egl,
                            .angle_gles = g_gles, .frame_dir = NULL, .frame_every = 0 };
    const char *frames = getenv("TL_PVZ2_FRAMES");
    if (frames && *frames) { tl_win_mkdirs(frames); g_cfg.frame_dir = frames; g_cfg.frame_every = getenv("TL_PVZ2_FRAME_EVERY") ? atoi(getenv("TL_PVZ2_FRAME_EVERY")) : 60; }

    badge_font_init();
    badges_apply_setting();
    pointer_apply_setting();
    tl_pvz2_set_keyboard_hook(keyboard_shown);
    /* The shortcuts are the Mac's: the game's own card press for a seed packet
     * and its own shovel, so neither needs coordinates at any surface size. */
    tl_pvz2_set_seed_pick(1);
    tl_pvz2_set_native_shovel(2);
#if defined(__x86_64__)
    if (translation_needed()) show_progress_window(inst);
    else tl_log_line("windows: the x64 translation cache is there; no preparing window");
#endif
    CloseHandle(CreateThread(NULL, 8 << 20, launch_game, NULL, 0, NULL));

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        HWND settings = pvz2_settings_window();
        if (settings && IsDialogMessageW(settings, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    tl_log_line("windows: the window is closed");
    ExitProcess(0);
}
