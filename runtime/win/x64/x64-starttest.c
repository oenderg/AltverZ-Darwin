/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The game's start-up on x86-64 Windows, without a window: the four libraries loaded from the APK, translated by
 * ScarletNX's recompiler into one module (kept in a cache folder), then their constructors and JNI_OnLoad run as
 * translated code. The x86-64 counterpart of win-starttest.c.
 *
 *   rfl-x64-starttest.exe <apk> [data dir] [cache dir]
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "husk-tl-jni.h"
#include "husk-tl-ld.h"
#include "husk-tl-xmem.h"
#include "husk-tl-x64.h"

void tl_log_line(const char *fmt, ...);
void tl_set_data_dir(const char *dir);
void tl_win_install_crash_handler(void);

static tl_lib *load(const char *name)
{
    ULONGLONG t0 = GetTickCount64();
    tl_lib *L = tl_ld_load(name);
    tl_log_line("start: load %s -> %s (%llu ms)", name, L ? "ok" : "FAILED", GetTickCount64() - t0);
    return L;
}

static int onload(tl_lib *L)
{
    void *fn = tl_ld_sym(L, "JNI_OnLoad");
    if (!fn) return -1;
    int v = (int)tl_guest_call((uint64_t)(uintptr_t)fn, "ill", (uint64_t)(uintptr_t)tl_jni_vm(), 0);
    tl_log_line("start: %s JNI_OnLoad -> %#x", tl_ld_lib_name(L), v);
    return v;
}

static bool init(tl_lib *L, const char *what)
{
    ULONGLONG t0 = GetTickCount64();
    bool ok = tl_ld_init(L);
    tl_log_line("start: %s constructors %s (%llu ms)", what, ok ? "ran" : "FAILED", GetTickCount64() - t0);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <apk> [data dir] [cache dir]\n", argv[0]); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    tl_win_install_crash_handler();
    tl_set_data_dir(argc > 2 ? argv[2] : "C:/rfl/data");
    const char *cache = argc > 3 ? argv[3] : "C:/rfl/x64-cache";
    char err[300] = "";
    if (!tl_xmem_open((size_t)1536 << 20, err, sizeof(err))) { tl_log_line("start: no memory region: %s", err); return 1; }
    tl_log_line("start: region at %p", tl_xmem_base());
    if (!tl_x64_open(err, sizeof(err))) { tl_log_line("start: %s", err); return 1; }
    if (!tl_ld_add_apk(argv[1])) { tl_log_line("start: cannot open the APK %s", argv[1]); return 1; }
    tl_jni_init();
    tl_lib *cxx = load("libc++_shared.so"), *nimble = load("libNimble.so"), *game = load("libPVZ2.so"), *rfl = load("libRFLib.so");
    if (!cxx || !nimble || !game) return 1;
    tl_log_line("start: %zu unresolved imports after loading", tl_ld_unresolved_count());
    if (!tl_x64_translate(cache, err, sizeof(err))) { tl_log_line("start: translation failed: %s", err); return 1; }
    if (!init(cxx, "libc++_shared")) return 1;
    if (!init(nimble, "libNimble")) return 1;
    if (!init(game, "libPVZ2")) return 1;
    onload(nimble);
    if (onload(game) < 0) tl_log_line("start: libPVZ2 has no JNI_OnLoad");
    if (rfl) init(rfl, "libRFLib");
    tl_log_line("start: done, %zu unresolved imports", tl_ld_unresolved_count());
    return 0;
}
