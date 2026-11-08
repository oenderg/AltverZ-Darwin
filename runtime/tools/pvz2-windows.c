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
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "husk-tl-audio.h"
#include "husk-tl-cdn.h"
#include "husk-tl-jni.h"
#include "husk-tl-ld.h"
#include "husk-tl-pvz2.h"
#include "husk-tl-xmem.h"
#include "pvz2-keys.h"
#include "pvz2-windows-settings.h"
#if defined(__x86_64__)
#include "husk-tl-x64.h"
#endif

void tl_log_line(const char *fmt, ...);
void tl_win_log_open(const char *path);
void tl_win_mkdirs(const char *path);
void tl_win_install_crash_handler(void);
void tl_win_set_saves_dir(const char *dir);

/* The size the game draws at. It lays out for 1748x804 and picks its HD art from 804 up. The window gets as
 * much of that as the screen has room for, never less than the minimum; the game's surface is the window's
 * real client size, so drawing and touch are 1:1. */
#define DESIGN_W 1748
#define DESIGN_H 804
#define MIN_CLIENT_W 800
#define MIN_CLIENT_H 450
#define WINDOW_STYLE (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME)
#define ID_SETTINGS 0x1000

static int g_sw = DESIGN_W, g_sh = DESIGN_H;
static HWND g_hwnd;
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
    save_geometry();
    toggle_fullscreen(g_settings.start_fullscreen ? 1 : 0);
    tl_log_line("windows: the settings are applied without a restart");
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

/* ---------------------------------------------------------------- the mouse */

static bool g_down;
static void touch(int phase, LPARAM lp)
{
    if (!atomic_load(&g_ready)) return;
    RECT rc; GetClientRect(g_hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;
    float x = (float)(short)LOWORD(lp) * (float)g_sw / (float)cw;
    float y = (float)(short)HIWORD(lp) * (float)g_sh / (float)ch;
    if (phase == 0) { tl_pvz2_keyboard_dismissed(); g_down = true; }
    else if (phase == 3) g_down = false;
    tl_pvz2_touch(phase, 1, x, y);
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
        if (h == g_hwnd && wp != SIZE_MINIMIZED) {
            int cw = LOWORD(lp), ch = HIWORD(lp);
            if (cw > 0 && ch > 0 && (cw != g_sw || ch != g_sh)) {
                g_sw = cw; g_sh = ch;
                tl_log_line("windows: the window is now %dx%d, the game draws at %dx%d", cw, ch, cw, ch);
                /* Adapt: the guest is told the new size on the GL thread, as Android tells it on a
                 * surface change, and lays its UI out again. Before the driver starts this only stores
                 * the size; the driver is started with the same one. */
                tl_pvz2_set_surface_size(cw, ch);
            }
        }
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
    case WM_LBUTTONDOWN: SetCapture(h); touch(0, lp); return 0;
    case WM_MOUSEMOVE: if (g_down) touch(1, lp); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); touch(3, lp); return 0;
    case WM_ERASEBKGND: return 1;
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
    wc.lpszClassName = L"PvZ2Reflourished";
    RegisterClassExW(&wc);

    int maximized = 0;
    RECT wr;
    initial_placement(&wr, &maximized);
    g_windowed_placement = (WINDOWPLACEMENT){ .length = sizeof(WINDOWPLACEMENT) };
    g_windowed_placement.showCmd = maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    g_windowed_placement.rcNormalPosition = wr;
    g_windowed_style = WINDOW_STYLE;

    if (g_start_fullscreen) {
        HMONITOR mon = monitor_for(&wr);
        MONITORINFO mi = { .cbSize = sizeof(mi) };
        if (GetMonitorInfoW(mon, &mi)) {
            g_fullscreen = 1;
            return CreateWindowExW(0, wc.lpszClassName, L"PvZ2 Reflourished", WS_POPUP | WS_VISIBLE,
                                   mi.rcMonitor.left, mi.rcMonitor.top,
                                   mi.rcMonitor.right - mi.rcMonitor.left,
                                   mi.rcMonitor.bottom - mi.rcMonitor.top,
                                   NULL, NULL, inst, NULL);
        }
    }
    g_fullscreen = 0;
    return CreateWindowExW(0, wc.lpszClassName, L"PvZ2 Reflourished", WINDOW_STYLE,
                           wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
                           NULL, NULL, inst, NULL);
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

    pvz2_settings_load(g_settings_path, &g_settings);
    g_start_fullscreen = g_settings.start_fullscreen;

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
    /* The window is created at its final size; ShowWindow only shows it, so the
     * game never sees a different size while it starts. */
    ShowWindow(g_hwnd, g_fullscreen ? SW_SHOW : (g_windowed_placement.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : show));
    UpdateWindow(g_hwnd);
    RECT rc; GetClientRect(g_hwnd, &rc);
    g_sw = (int)(rc.right - rc.left);
    g_sh = (int)(rc.bottom - rc.top);
    tl_log_line("windows: window %dx%d client, the game draws at %dx%d", g_sw, g_sh, g_sw, g_sh);

    /* The window menu: Settings..., then a separator, as the player's mock-up
     * had it. Windows' own items (Move, Size, Close) follow it. */
    HMENU sys = GetSystemMenu(g_hwnd, FALSE);
    if (sys) {
        InsertMenuW(sys, 0, MF_BYPOSITION | MF_STRING, ID_SETTINGS, L"Settings...\tCtrl+,");
        InsertMenuW(sys, 1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    }

    g_cfg = (tl_ga_config){ .apk_path = g_apk, .data_dir = g_data, .package_name = "com.ea.game.pvz2_rfl",
                            .width = g_sw, .height = g_sh, .metal_layer = g_hwnd, .angle_egl = g_egl,
                            .angle_gles = g_gles, .frame_dir = NULL, .frame_every = 0 };
    const char *frames = getenv("TL_PVZ2_FRAMES");
    if (frames && *frames) { tl_win_mkdirs(frames); g_cfg.frame_dir = frames; g_cfg.frame_every = getenv("TL_PVZ2_FRAME_EVERY") ? atoi(getenv("TL_PVZ2_FRAME_EVERY")) : 60; }

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
