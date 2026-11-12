/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Windows start-up test: load the game's four libraries from the APK, run their
 * constructors and JNI_OnLoad, in the order the driver uses. No window, no GL.
 * Usage: rfl-starttest.exe <apk> [data dir]
 */
#include "pvz2-profile.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "husk-tl-ld.h"
#include "husk-tl-xmem.h"
#include "husk-tl-jni.h"

void tl_log_line(const char *fmt, ...);
void tl_set_data_dir(const char *dir);
void tl_win_install_crash_handler(void);

static tl_lib *load(const char *name)
{
    DWORD t0 = GetTickCount();
    tl_lib *L = tl_ld_load(name);
    tl_log_line("start: load %s -> %s (%lu ms)", name, L ? "ok" : "FAILED", GetTickCount() - t0);
    return L;
}

static int onload(tl_lib *L)
{
    typedef int (*fn_t)(void *, void *);
    fn_t fn = (fn_t)tl_ld_sym(L, "JNI_OnLoad");
    if (!fn) return -1;
    int v = fn(tl_jni_vm(), NULL);
    tl_log_line("start: %s JNI_OnLoad -> %#x", tl_ld_lib_name(L), v);
    return v;
}

/* The fault guard the driver puts around calls into the game: a null read on this thread must come back
 * through the handler and siglongjmp, twice in a row (the handler resets itself after each fault). */
static sigjmp_buf g_jmp;
static volatile int g_sig;
static void on_fault(int sig, siginfo_t *info, void *ctx) { (void)info; (void)ctx; g_sig = sig; siglongjmp(g_jmp, 1); }
static int guard_check(void)
{
    int caught = 0;
    for (int round = 0; round < 2; round++) {
        struct sigaction sa, old;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
        sigaction(SIGSEGV, &sa, &old);
        g_sig = 0;
        if (sigsetjmp(g_jmp, 1) == 0) {
            volatile int *p = (volatile int *)(uintptr_t)(0x58 + round);
            (void)*p;
        } else caught++;
        sigaction(SIGSEGV, &old, NULL);
    }
    return caught;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--guard")) {
        tl_win_install_crash_handler();
        int n = guard_check();
        printf("guard: %d of 2 faults caught\n", n);
        return n == 2 ? 0 : 1;
    }
    if (argc < 2) { fprintf(stderr, "usage: %s <apk> [data dir]\n", argv[0]); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    tl_win_install_crash_handler();
    tl_set_data_dir(argc > 2 ? argv[2] : "C:/rfl/data");
    char err[200] = "";
    if (!tl_xmem_open((size_t)1536 << 20, err, sizeof(err))) { tl_log_line("start: no executable memory: %s", err); return 1; }
    if (!tl_ld_add_apk(argv[1])) { tl_log_line("start: cannot open the APK %s", argv[1]); return 1; }
    tl_jni_init();
    tl_lib *cxx = load("libc++_shared.so"), *nimble = load("libNimble.so"), *game = load("libPVZ2.so");
    if (!cxx || !nimble || !game) return 1;
    tl_log_line("start: %zu unresolved imports after loading", tl_ld_unresolved_count());
    if (!tl_ld_init(cxx)) { tl_log_line("start: libc++ constructors failed"); return 1; }
    tl_log_line("start: libc++_shared constructors ran");
    if (!tl_ld_init(nimble)) { tl_log_line("start: libNimble constructors failed"); return 1; }
    tl_log_line("start: libNimble constructors ran");
    if (!tl_ld_init(game)) { tl_log_line("start: libPVZ2 constructors failed"); return 1; }
    tl_log_line("start: libPVZ2 constructors ran");
    onload(nimble);
    if (onload(game) < 0) tl_log_line("start: libPVZ2 has no JNI_OnLoad");
    tl_lib *rfl = load(PVZ2_PROFILE_MOD_LIBRARY);
    if (rfl && tl_ld_init(rfl)) tl_log_line("start: " PVZ2_PROFILE_MOD_LIBRARY_BASE " constructors ran");
    /* the mod wrote its hooks into the game's code: make them visible to instruction fetch */
    tl_xmem_flush(tl_xmem_base(), tl_xmem_used());
    tl_log_line("start: done, %zu unresolved imports", tl_ld_unresolved_count());
    return 0;
}
