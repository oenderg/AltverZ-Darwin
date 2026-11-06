/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PvZ2: Reflourished on Windows: the host.
 *
 * A Win32 window that ANGLE draws into (on Direct3D), the mouse as a finger, the
 * keyboard's shortcuts later, and the game's start-up: the same driver as the Mac
 * and iPad hosts (husk-tl-pvz2.c), with the game's libraries loaded at run time.
 *
 * Where things are (each overridable for tests):
 *   the APK          Game/pvz2.apk next to the program        (TL_PVZ2_APK)
 *   the CDN archive  Game/1.4.2.main.tar next to the program  (TL_PVZ2_MAIN_TAR)
 *   ANGLE            libEGL.dll and libGLESv2.dll next to it  (TL_ANGLE_EGL, TL_ANGLE_GLES)
 *   the data folder  %LOCALAPPDATA%/PvZ2 Reflourished          (TL_DATA), with pvz2.log in it
 */
#include <windows.h>
#include <shlobj.h>
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

void tl_log_line(const char *fmt, ...);
void tl_win_log_open(const char *path);
void tl_win_mkdirs(const char *path);
void tl_win_install_crash_handler(void);

/* The size the game draws at. It lays out for 1748x804 and picks its HD art from 804 up. */
static int g_sw = 1748, g_sh = 804;
static HWND g_hwnd;
static char g_exe_dir[MAX_PATH], g_data[MAX_PATH], g_apk[MAX_PATH], g_egl[MAX_PATH], g_gles[MAX_PATH];
static tl_ga_config g_cfg;
static atomic_int g_ready;

static void slashes(char *p) { for (; *p; p++) if (*p == '\\') *p = '/'; }
static const char *env_or(const char *name, const char *fallback) { const char *v = getenv(name); return v && *v ? v : fallback; }

/* ------------------------------------------------------------- the game run */

static DWORD WINAPI launch_game(void *arg)
{
    (void)arg;
    if (!getenv("TL_AUDIO") || atoi(getenv("TL_AUDIO"))) tl_audio_install();
    bool ok = tl_pvz2_start(&g_cfg) && tl_pvz2_run();
    atomic_store(&g_ready, ok);
    tl_log_line("windows: driver startup %s", ok ? "complete" : "FAILED");
    if (!ok) MessageBoxA(g_hwnd, "Reflourished could not start. See pvz2.log in the data folder for details.", "PvZ2 Reflourished", MB_ICONERROR);
    return 0;
}

/* ---------------------------------------------------------------- the window */

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
    case WM_LBUTTONDOWN: SetCapture(h); touch(0, lp); return 0;
    case WM_MOUSEMOVE: if (g_down) touch(1, lp); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); touch(3, lp); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE:
        /* Android's way out: the game hears it is going to the background, and saves. */
        tl_log_line("windows: the window is closing; the game goes to the background first");
        if (atomic_load(&g_ready)) { tl_pvz2_lifecycle_background(); Sleep(1500); }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(h, msg, wp, lp);
    }
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
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, g_sw, g_sh };
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, GetDpiForSystem());
    return CreateWindowExW(0, wc.lpszClassName, L"PvZ2 Reflourished", style, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
}

/* ------------------------------------------------------------------- start */

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    tl_win_install_crash_handler();

    GetModuleFileNameA(NULL, g_exe_dir, sizeof(g_exe_dir));
    slashes(g_exe_dir);
    char *s = strrchr(g_exe_dir, '/'); if (s) *s = 0;

    char local[MAX_PATH] = ".";
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local))) slashes(local);
    char def[MAX_PATH];
    snprintf(def, sizeof(def), "%s/PvZ2 Reflourished", local);
    snprintf(g_data, sizeof(g_data), "%s", env_or("TL_DATA", def));
    slashes(g_data);
    tl_win_mkdirs(g_data);
    char logp[MAX_PATH + 16]; snprintf(logp, sizeof(logp), "%s/pvz2.log", g_data);
    tl_win_log_open(logp);

    snprintf(def, sizeof(def), "%s/Game/pvz2.apk", g_exe_dir);
    snprintf(g_apk, sizeof(g_apk), "%s", env_or("TL_PVZ2_APK", def));
    snprintf(def, sizeof(def), "%s/libEGL.dll", g_exe_dir);
    snprintf(g_egl, sizeof(g_egl), "%s", env_or("TL_ANGLE_EGL", def));
    snprintf(def, sizeof(def), "%s/libGLESv2.dll", g_exe_dir);
    snprintf(g_gles, sizeof(g_gles), "%s", env_or("TL_ANGLE_GLES", def));
    tl_log_line("windows: data %s, APK %s, ANGLE %s", g_data, g_apk, g_egl);

    /* The CDN fallback archive, staged into the data folder unless a newer download is there. */
    {
        char bundled[MAX_PATH + 32], cached[MAX_PATH + 32];
        snprintf(bundled, sizeof(bundled), "%s/Game/1.4.2.main.tar", g_exe_dir);
        snprintf(cached, sizeof(cached), "%s/No_Backup", g_data); tl_win_mkdirs(cached);
        snprintf(cached, sizeof(cached), "%s/No_Backup/1.4.2.main.tar", g_data);
        if (!getenv("TL_PVZ2_MAIN_TAR")) { tl_cdn_stage(bundled, cached); _putenv_s("TL_PVZ2_MAIN_TAR", cached); }
    }

    char err[200] = "";
    if (!tl_xmem_open((size_t)2048 << 20, err, sizeof(err))) {
        tl_log_line("windows: no executable memory: %s", err);
        MessageBoxA(NULL, err, "PvZ2 Reflourished", MB_ICONERROR);
        return 1;
    }
    tl_ld_set_verbosity(getenv("TL_VERBOSE") ? atoi(getenv("TL_VERBOSE")) : 1);
    tl_jni_set_trace(getenv("TL_JNI_TRACE") ? atoi(getenv("TL_JNI_TRACE")) : 1);

    g_hwnd = make_window(inst);
    if (!g_hwnd) { tl_log_line("windows: CreateWindow failed (%lu)", GetLastError()); return 1; }
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);
    RECT rc; GetClientRect(g_hwnd, &rc);
    tl_log_line("windows: window %dx%d, the game draws at %dx%d", (int)(rc.right - rc.left), (int)(rc.bottom - rc.top), g_sw, g_sh);

    g_cfg = (tl_ga_config){ .apk_path = g_apk, .data_dir = g_data, .package_name = "com.ea.game.pvz2_rfl",
                            .width = g_sw, .height = g_sh, .metal_layer = g_hwnd, .angle_egl = g_egl,
                            .angle_gles = g_gles, .frame_dir = NULL, .frame_every = 0 };
    const char *frames = getenv("TL_PVZ2_FRAMES");
    if (frames && *frames) { tl_win_mkdirs(frames); g_cfg.frame_dir = frames; g_cfg.frame_every = getenv("TL_PVZ2_FRAME_EVERY") ? atoi(getenv("TL_PVZ2_FRAME_EVERY")) : 60; }

    CloseHandle(CreateThread(NULL, 8 << 20, launch_game, NULL, 0, NULL));

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    tl_log_line("windows: the window is closed");
    ExitProcess(0);
}
