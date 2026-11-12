/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The game's data (OBB) on Windows, the Mac's PVZ2ObbDownload in Win32:
 *
 * A slim copy carries no OBB (1.3 GB of the game's assets), so the first start
 * downloads it from the link the Mac build uses (pvz2-obb-win.h, from
 * data/game-1.4.2.json), checks its size and sha256, and only then starts the
 * game. The file goes where the game looks first,
 * <data>/sdcard/Android/obb/com.ea.game.pvz2_rfl/<name>, through a .part file
 * and an atomic move. A stopped download keeps its .part and the next try asks
 * for the rest with a Range request. A small window shows the progress, a
 * Cancel button, and a Try Again after a failure.
 *
 * The checks and the paths are the Mac client's: same name, size, sha256,
 * same folder in the data directory, same 256 MB of headroom, same messages.
 */
#include "pvz2-profile.h"
#include <windows.h>
#include <commctrl.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "husk-obb-win.h"
#include "pvz2-obb-win.h"
#include "pvz2-windows-darkmode.h"
#include "pvz2-windows-identity.h"

void tl_log_line(const char *fmt, ...);
void tl_win_mkdirs(const char *path);

#define OBB_CLASS L"" PVZ2_PROFILE_WIN_STEM L"Obb"
#define ID_RETRY 1
#define ID_CANCEL 2
#define TIMER_PROGRESS 1

typedef struct {
    char data[MAX_PATH];
    char exe[MAX_PATH];
    char url[1024];
    char dir[MAX_PATH + 64];
    char final[MAX_PATH + 96];
    char part[MAX_PATH + 100];
    HWND win, title, detail, bar, retry, cancel;
    HANDLE thread;
    atomic_int state;       /* 0 running, 1 done, 2 failed, 3 cancelled */
    atomic_int cancel_flag;
    atomic_llong written;
    long long have;         /* bytes at the start of this attempt */
    char why[220];
    int result;
} obb_state;

static obb_state D;

static long long file_size(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!path || !GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return -1;
    return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

static int enough_space(void)
{
    tl_win_mkdirs(D.dir);
    ULARGE_INTEGER freeb;
    long long need = PVZ2_OBB_SIZE + (256LL << 20);
    if (!GetDiskFreeSpaceExA(D.dir, &freeb, NULL, NULL)) return 1;
    if ((long long)freeb.QuadPart >= need) return 1;
    /* An OBB of another version is on its way out anyway: it goes first when
     * both do not fit. */
    if (file_size(D.final) > 0) {
        DeleteFileA(D.final);
        tl_log_line("windows: the old OBB was removed to make room for the new one");
        if (!GetDiskFreeSpaceExA(D.dir, &freeb, NULL, NULL)) return 1;
        if ((long long)freeb.QuadPart >= need) return 1;
    }
    snprintf(D.why, sizeof(D.why),
             "The game's data needs %.1f GB free on this PC (%.1f GB is free now). Free some space, then try again.",
             need / 1e9, (double)freeb.QuadPart / 1e9);
    tl_log_line("windows: not enough space for the OBB: %lld bytes free, %lld needed",
                (long long)freeb.QuadPart, need);
    return 0;
}

static int sha256_file(const char *path, char out[65], long long *bytes)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR obj = NULL;
    unsigned char *buf = NULL;
    int ok = 0;
    out[0] = 0;
    *bytes = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) return 0;
    DWORD obj_len = 0, got = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_len, sizeof(obj_len), &got, 0) < 0) goto done;
    obj = malloc(obj_len);
    if (!obj || BCryptCreateHash(alg, &hash, obj, obj_len, NULL, 0, 0) < 0) goto done;
    buf = malloc(8u << 20);
    FILE *f = buf ? fopen(path, "rb") : NULL;
    if (!f) goto done;
    size_t n;
    while ((n = fread(buf, 1, 8u << 20, f)) > 0) {
        if (BCryptHashData(hash, buf, (ULONG)n, 0) < 0) { fclose(f); goto done; }
        *bytes += (long long)n;
    }
    fclose(f);
    unsigned char md[32];
    if (BCryptFinishHash(hash, md, sizeof(md), 0) < 0) goto done;
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", md[i]);
    ok = 1;
done:
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    free(buf);
    free(obj);
    return ok;
}

static int check_and_place(char *why, size_t whyn)
{
    long long have = file_size(D.part);
    char hex[65] = "";
    long long hashed = 0;
    if (have != PVZ2_OBB_SIZE || !sha256_file(D.part, hex, &hashed) ||
        _stricmp(hex, PVZ2_OBB_SHA256) != 0) {
        tl_log_line("windows: the downloaded OBB is wrong (%lld bytes, sha256 %s); it is deleted",
                    have, hex[0] ? hex : "?");
        DeleteFileA(D.part);
        snprintf(why, whyn, "The download was damaged.");
        return 0;
    }
    DeleteFileA(D.final);
    if (!MoveFileExA(D.part, D.final, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        tl_log_line("windows: the OBB could not be put in place (%lu)", GetLastError());
        snprintf(why, whyn, "The game's data could not be saved.");
        return 0;
    }
    tl_log_line("windows: the OBB is downloaded and checked (%lld bytes)", have);
    return 1;
}

/* The download itself, on its own thread: WinHTTP with the Mac's rules (the
 * build's URL, resume with Range, 15 s without data is a failure), writing
 * the .part file as it goes. Returns 1 complete, 2 cancelled, 0 failed. */
static int obb_http(char *why, size_t whyn)
{
    HINTERNET ses = NULL, con = NULL, req = NULL;
    FILE *f = NULL;
    int result = 0;
    DWORD got = 0;
    long long have = file_size(D.part);
    if (have < 0) have = 0;
    if (have > PVZ2_OBB_SIZE) {
        DeleteFileA(D.part);
        have = 0;
    }
    wchar_t wurl[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, D.url, -1, wurl, 2048)) {
        snprintf(why, whyn, "The download link is not valid.");
        return 0;
    }
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        snprintf(why, whyn, "The download link is not valid.");
        return 0;
    }
    wchar_t host[256], path[2048];
    if (uc.dwHostNameLength >= 256) { snprintf(why, whyn, "The download link is not valid."); return 0; }
    wcsncpy(host, uc.lpszHostName, uc.dwHostNameLength);
    host[uc.dwHostNameLength] = 0;
    int pn = _snwprintf(path, 2048, L"%.*s%.*s", (int)uc.dwUrlPathLength, uc.lpszUrlPath,
                        (int)uc.dwExtraInfoLength, uc.lpszExtraInfo ? uc.lpszExtraInfo : L"");
    if (pn <= 0 || pn >= 2048) { snprintf(why, whyn, "The download link is not valid."); return 0; }

    ses = WinHttpOpen(L"" PVZ2_PROFILE_APP_NAME L"/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { snprintf(why, whyn, "WinHTTP could not start (%lu).", GetLastError()); goto done; }
    WinHttpSetTimeouts(ses, 30000, 30000, 30000, 15000);
    con = WinHttpConnect(ses, host, uc.nPort ? uc.nPort : 443, 0);
    if (!con) { snprintf(why, whyn, "The connection failed (%lu).", GetLastError()); goto done; }
    req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { snprintf(why, whyn, "The request failed (%lu).", GetLastError()); goto done; }
    if (have > 0) {
        wchar_t range[80];
        swprintf(range, 80, L"Range: bytes=%lld-\r\n", have);
        WinHttpAddRequestHeaders(req, range, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
        tl_log_line("windows: the OBB download resumes");
    }
    tl_log_line("windows: the OBB is downloaded from %s", D.url);
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0)) {
        snprintf(why, whyn, "The request failed (%lu).", GetLastError());
        tl_log_line("windows: the OBB download stopped: WinHTTP error %lu", GetLastError());
        goto done;
    }
    if (!WinHttpReceiveResponse(req, NULL)) {
        snprintf(why, whyn, "The server did not answer (%lu).", GetLastError());
        tl_log_line("windows: the OBB download stopped: WinHTTP error %lu", GetLastError());
        goto done;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (have > 0 && status == 206) {
        /* append at the end of the .part */
    } else if (status >= 200 && status < 300) {
        have = 0;                       /* the whole file: start again */
    } else if (status == 416 && file_size(D.part) == PVZ2_OBB_SIZE) {
        result = 1;                     /* the .part is already complete */
        goto done;
    } else {
        tl_log_line("windows: the OBB download failed (HTTP %lu)", status);
        snprintf(why, whyn, "The server answered %lu.", status);
        goto done;
    }
    f = fopen(D.part, have > 0 ? "r+b" : "wb");
    if (!f) {
        snprintf(why, whyn, "The download could not be saved.");
        goto done;
    }
    if (have > 0) _fseeki64(f, have, SEEK_SET);
    atomic_store(&D.written, have);
    {
        unsigned char *buf = malloc(1 << 20);
        if (!buf) { snprintf(why, whyn, "The download could not be saved."); goto done; }
        for (;;) {
            if (atomic_load(&D.cancel_flag)) { result = 2; break; }
            DWORD n = 0;
            if (!WinHttpReadData(req, buf, 1 << 20, &n)) {
                tl_log_line("windows: the OBB download stopped: WinHTTP error %lu", GetLastError());
                snprintf(why, whyn, "The connection stopped (%lu).", GetLastError());
                break;
            }
            if (!n) { result = 1; break; }
            if (fwrite(buf, 1, n, f) != n) {
                snprintf(why, whyn, "The download could not be saved (is the disk full?).");
                break;
            }
            have += n;
            atomic_store(&D.written, have);
        }
        free(buf);
    }
    if (result == 1 && have != PVZ2_OBB_SIZE) {
        tl_log_line("windows: the OBB download stopped early (%lld of %lld bytes)", have, PVZ2_OBB_SIZE);
        snprintf(why, whyn, "The download stopped early.");
        result = 0;
    }
done:
    if (f) fclose(f);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return result;
}

static DWORD WINAPI obb_worker(void *unused)
{
    (void)unused;
    char why[220] = "";
    long long have = file_size(D.part);
    int result;
    atomic_store(&D.written, have > 0 ? have : 0);
    if (have == PVZ2_OBB_SIZE) result = 1;
    else result = obb_http(why, sizeof(why));
    if (result == 2) { atomic_store(&D.state, 3); return 0; }
    if (!result) {
        snprintf(D.why, sizeof(D.why), "%s", why[0] ? why : "The download stopped.");
        tl_log_line("windows: the OBB download stopped: %s", D.why);
        atomic_store(&D.state, 2);
        return 0;
    }
    if (!check_and_place(why, sizeof(why))) {
        snprintf(D.why, sizeof(D.why), "%s", why);
        atomic_store(&D.state, 2);
        return 0;
    }
    atomic_store(&D.state, 1);
    return 0;
}

static void start_worker(void)
{
    if (D.thread) {
        WaitForSingleObject(D.thread, 10000);
        CloseHandle(D.thread);
        D.thread = NULL;
    }
    atomic_store(&D.cancel_flag, 0);
    atomic_store(&D.state, 0);
    atomic_store(&D.written, 0);
    D.why[0] = 0;
    D.thread = CreateThread(NULL, 8 << 20, obb_worker, NULL, 0, NULL);
}

static void show_failure(void)
{
    SetWindowTextW(D.title, L"The download stopped");
    wchar_t msg[512];
    _snwprintf(msg, 512, L"%hs Check the connection, then try again.", D.why);
    SetWindowTextW(D.detail, msg);
    ShowWindow(D.retry, SW_SHOW);
    ShowWindow(D.cancel, SW_SHOW);
}

static LRESULT CALLBACK obb_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp != TIMER_PROGRESS) break;
        {
            int st = atomic_load(&D.state);
            if (st == 0) {
                long long w = atomic_load(&D.written);
                wchar_t buf[80];
                swprintf(buf, 80, L"%lld of %lld MB", w / 1000000, PVZ2_OBB_SIZE / 1000000);
                SetWindowTextW(D.detail, buf);
                SendMessageW(D.bar, PBM_SETPOS,
                             (WPARAM)(PVZ2_OBB_SIZE > 0 ? (w * 1000) / PVZ2_OBB_SIZE : 0), 0);
            } else if (st == 1) {
                KillTimer(h, TIMER_PROGRESS);
                D.result = 1;
                DestroyWindow(h);
            } else if (st == 2) {
                KillTimer(h, TIMER_PROGRESS);
                show_failure();
            } else if (st == 3) {
                KillTimer(h, TIMER_PROGRESS);
                D.result = 0;
                DestroyWindow(h);
            }
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_RETRY) {
            ShowWindow(D.retry, SW_HIDE);
            SetWindowTextW(D.title, L"Downloading the game's data");
            SetWindowTextW(D.detail, L"Starting...");
            SendMessageW(D.bar, PBM_SETPOS, 0, 0);
            if (!enough_space()) { show_failure(); return 0; }
            start_worker();
            SetTimer(h, TIMER_PROGRESS, 50, NULL);
            return 0;
        }
        if (LOWORD(wp) == ID_CANCEL) {
            atomic_store(&D.cancel_flag, 1);
            if (atomic_load(&D.state) != 0) { D.result = 0; DestroyWindow(h); }
            return 0;
        }
        break;
    case WM_CLOSE:
        atomic_store(&D.cancel_flag, 1);
        if (atomic_load(&D.state) != 0) { D.result = 0; DestroyWindow(h); }
        return 0;
    case WM_SETTINGCHANGE:
        if (tl_win_darkmode_system_changed(lp)) tl_win_darkmode_redraw(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static int scaled(int v)
{
    return MulDiv(v, (int)GetDpiForSystem(), 96);
}

static void build_window(void)
{
    HINSTANCE inst = GetModuleHandleW(NULL);
    static int registered;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = obb_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = OBB_CLASS;
        RegisterClassExW(&wc);
        registered = 1;
    }
    RECT r = { 0, 0, scaled(460), scaled(210) };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0, GetDpiForSystem());
    int w = r.right - r.left, h = r.bottom - r.top;
    RECT area;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0)) { area.right = GetSystemMetrics(SM_CXSCREEN); area.bottom = GetSystemMetrics(SM_CYSCREEN); }
    int x = area.left + ((area.right - area.left) - w) / 2;
    int y = area.top + ((area.bottom - area.top) - h) / 2;
    D.win = CreateWindowExW(0, OBB_CLASS, L"" PVZ2_PROFILE_APP_NAME, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                            x, y, w, h, NULL, NULL, inst, NULL);
    if (!D.win) return;
    pvz2_window_app_id(D.win);
    HFONT font = CreateFontW(-MulDiv(9, (int)GetDpiForSystem(), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    HFONT bold = CreateFontW(-MulDiv(14, (int)GetDpiForSystem(), 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    D.title = CreateWindowExW(0, L"STATIC", L"Downloading the game's data", WS_CHILD | WS_VISIBLE | SS_LEFT,
                              scaled(20), scaled(16), scaled(420), scaled(24), D.win, NULL, inst, NULL);
    SendMessageW(D.title, WM_SETFONT, (WPARAM)bold, TRUE);
    D.bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE,
                            scaled(20), scaled(50), scaled(420), scaled(22), D.win, NULL, inst, NULL);
    SendMessageW(D.bar, PBM_SETRANGE32, 0, 1000);
    D.detail = CreateWindowExW(0, L"STATIC", L"The first start downloads the game's data (1.3 GB) once.",
                               WS_CHILD | WS_VISIBLE | SS_LEFT,
                               scaled(20), scaled(82), scaled(420), scaled(44), D.win, NULL, inst, NULL);
    SendMessageW(D.detail, WM_SETFONT, (WPARAM)font, TRUE);
    D.retry = CreateWindowExW(0, L"BUTTON", L"Try Again", WS_CHILD | BS_PUSHBUTTON,
                              scaled(266), scaled(158), scaled(90), scaled(28), D.win, (HMENU)ID_RETRY, inst, NULL);
    SendMessageW(D.retry, WM_SETFONT, (WPARAM)font, TRUE);
    D.cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               scaled(366), scaled(158), scaled(74), scaled(28), D.win, (HMENU)ID_CANCEL, inst, NULL);
    SendMessageW(D.cancel, WM_SETFONT, (WPARAM)font, TRUE);
    tl_win_darkmode_window(D.win);
    ShowWindow(D.win, SW_SHOW);
    SetForegroundWindow(D.win);
}

int tl_win_obb_ensure(const char *data_dir, const char *exe_dir)
{
    memset(&D, 0, sizeof(D));
    snprintf(D.data, sizeof(D.data), "%s", data_dir);
    snprintf(D.exe, sizeof(D.exe), "%s", exe_dir);
    const char *urls = getenv("TL_PVZ2_OBB_URLS");
    if (urls && *urls) snprintf(D.url, sizeof(D.url), "%.*s", (int)strcspn(urls, " "), urls);
    else snprintf(D.url, sizeof(D.url), "%s", PVZ2_OBB_URL);
    snprintf(D.dir, sizeof(D.dir), "%s/sdcard/Android/obb/" PVZ2_PROFILE_PKG, data_dir);
    snprintf(D.final, sizeof(D.final), "%s/%s", D.dir, PVZ2_OBB_NAME);
    snprintf(D.part, sizeof(D.part), "%s.part", D.final);

    long long have = file_size(D.final);
    if (have == PVZ2_OBB_SIZE) {
        tl_log_line("windows: the game's data (OBB) is here (%lld bytes)", have);
        return 1;
    }
    if (have > 0)
        tl_log_line("windows: the OBB in the data folder is %lld bytes and this app wants %lld: it is downloaded again",
                    have, PVZ2_OBB_SIZE);
    tl_log_line("windows: the game's data (OBB) is not here yet: it is downloaded first");

    build_window();
    if (!D.win) {
        tl_log_line("windows: the OBB window could not be made (%lu)", GetLastError());
        return 0;
    }
    if (enough_space()) {
        start_worker();
        SetTimer(D.win, TIMER_PROGRESS, 50, NULL);
    } else {
        show_failure();
    }
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(D.win, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (D.thread) {
        atomic_store(&D.cancel_flag, 1);
        WaitForSingleObject(D.thread, 10000);
        CloseHandle(D.thread);
        D.thread = NULL;
    }
    return D.result;
}
