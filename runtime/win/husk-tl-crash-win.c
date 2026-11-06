/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A crash report for Windows: the exception, where it happened (the guest library and
 * offset, or the Windows module), the registers, and the guest's call chain as far as
 * its frame pointers lead. Installed first, so it sees a fault before anything else.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

#include "husk-tl-ld.h"

void tl_log_line(const char *fmt, ...);

static void where(uint64_t a, char *out, size_t n)
{
    const char *lib = NULL; const void *sa = NULL;
    const char *sym = tl_ld_symbol_at((const void *)(uintptr_t)a, &lib, &sa);
    tl_lib *L = tl_ld_lib_of((const void *)(uintptr_t)a);
    if (L) {
        uint64_t off = a - (uint64_t)(uintptr_t)tl_ld_lib_base(L);
        snprintf(out, n, "%s+%#llx%s%s", tl_ld_lib_name(L), (unsigned long long)off, sym ? " " : "", sym ? sym : "");
        return;
    }
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)a, &m)) {
        char name[MAX_PATH]; GetModuleFileNameA(m, name, sizeof(name));
        const char *b = strrchr(name, '\\');
        snprintf(out, n, "%s+%#llx", b ? b + 1 : name, (unsigned long long)(a - (uint64_t)(uintptr_t)m));
        return;
    }
    snprintf(out, n, "%#llx", (unsigned long long)a);
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    /* only what ends the process: debug output, C++ exceptions and the like pass */
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_IN_PAGE_ERROR && code != EXCEPTION_DATATYPE_MISALIGNMENT && code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_BREAKPOINT && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = e->ContextRecord;
    char pc[256], lr[256];
    where(c->Pc, pc, sizeof(pc)); where(c->Lr, lr, sizeof(lr));
    tl_log_line("CRASH: exception %#lx at %s (thread %lu)", code, pc, GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2)
        tl_log_line("CRASH: %s address %#llx", e->ExceptionRecord->ExceptionInformation[0] == 8 ? "executing" : e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                    (unsigned long long)e->ExceptionRecord->ExceptionInformation[1]);
    tl_log_line("CRASH: lr %s", lr);
    for (int i = 0; i < 29; i += 4)
        tl_log_line("CRASH: x%-2d %016llx %016llx %016llx %016llx", i, (unsigned long long)c->X[i], (unsigned long long)c->X[i + 1],
                    (unsigned long long)(i + 2 < 29 ? c->X[i + 2] : 0), (unsigned long long)(i + 3 < 29 ? c->X[i + 3] : 0));
    tl_log_line("CRASH: fp %016llx sp %016llx", (unsigned long long)c->Fp, (unsigned long long)c->Sp);
    uint64_t fp = c->Fp;
    for (int i = 0; i < 24 && fp && !(fp & 7); i++) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)fp, &mi, sizeof(mi)) || mi.State != MEM_COMMIT) break;
        uint64_t next = ((uint64_t *)(uintptr_t)fp)[0], ret = ((uint64_t *)(uintptr_t)fp)[1];
        char w[256]; where(ret, w, sizeof(w));
        tl_log_line("CRASH:   #%d %s", i, w);
        if (next <= fp) break;
        fp = next;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void tl_win_install_crash_handler(void) { AddVectoredExceptionHandler(1, on_exception); }
