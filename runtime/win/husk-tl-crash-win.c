/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A crash report for Windows: the exception, where it happened (the guest library and
 * offset, or the Windows module), the registers, and the guest's call chain as far as
 * its frame pointers lead. Installed first, so it sees a fault before anything else.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

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

/* ------------------------------------------------- signal handlers, by thread
 *
 * A handler for SIGSEGV, SIGBUS or SIGILL counts on the thread that installed it, and only there: the
 * driver puts one up around each call into the game that may read a stale pointer, and takes it down
 * after. Such a fault on that thread resumes in the handler (on the same stack, a little below where it
 * stopped), which leaves by siglongjmp. Faults anywhere else are crashes, as before. */
static struct sigaction g_sig[32];
static DWORD g_sig_thread[32];
static SRWLOCK g_sig_lock = SRWLOCK_INIT;

int tl_win_sigaction(int s, const struct sigaction *a, struct sigaction *o)
{
    if (s <= 0 || s >= 32) return -1;
    AcquireSRWLockExclusive(&g_sig_lock);
    if (o) *o = g_sig[s];
    if (a) {
        g_sig[s] = *a;
        bool real = a->sa_handler != SIG_DFL && a->sa_handler != SIG_IGN;
        g_sig_thread[s] = real ? GetCurrentThreadId() : 0;
    }
    ReleaseSRWLockExclusive(&g_sig_lock);
    return 0;
}

static __thread siginfo_t t_info;
static void handler_returned(void)
{
    tl_log_line("CRASH: a fault handler returned instead of jumping out");
    TerminateProcess(GetCurrentProcess(), 3);
}

static bool to_handler(EXCEPTION_POINTERS *e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    int sig = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? SIGSEGV
            : code == EXCEPTION_DATATYPE_MISALIGNMENT ? SIGBUS
            : code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION ? SIGILL : 0;
    if (!sig) return false;
    struct sigaction sa;
    AcquireSRWLockExclusive(&g_sig_lock);
    sa = g_sig[sig];
    bool mine = g_sig_thread[sig] == GetCurrentThreadId() && sa.sa_handler != SIG_DFL && sa.sa_handler != SIG_IGN;
    if (mine && (sa.sa_flags & SA_RESETHAND)) { memset(&g_sig[sig], 0, sizeof(g_sig[sig])); g_sig_thread[sig] = 0; }
    ReleaseSRWLockExclusive(&g_sig_lock);
    if (!mine) return false;
    t_info.si_signo = sig; t_info.si_code = 0;
    t_info.si_addr = code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2
                   ? (void *)(uintptr_t)e->ExceptionRecord->ExceptionInformation[1] : (void *)(uintptr_t)e->ContextRecord->Pc;
    CONTEXT *c = e->ContextRecord;
    c->X0 = (DWORD64)sig;
    c->X1 = (DWORD64)(uintptr_t)&t_info;
    c->X2 = 0;
    c->Sp = (c->Sp - 512) & ~(DWORD64)15;
    c->Lr = (DWORD64)(uintptr_t)handler_returned;
    c->Pc = (DWORD64)(uintptr_t)(void *)sa.sa_handler;   /* sa_sigaction shares the slot */
    return true;
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (to_handler(e)) return EXCEPTION_CONTINUE_EXECUTION;
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
    if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2) {
        /* What the address is now: never allocated (free), reserved but given back (a heap's
         * decommitted pages), or committed (a protection fault). */
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery((void *)(uintptr_t)e->ExceptionRecord->ExceptionInformation[1], &mi, sizeof(mi)))
            tl_log_line("CRASH: that page is %s (region %p+%#llx, allocated at %p, protect %#lx)",
                        mi.State == MEM_FREE ? "free" : mi.State == MEM_RESERVE ? "reserved, not committed" : "committed",
                        mi.BaseAddress, (unsigned long long)mi.RegionSize, mi.AllocationBase, mi.Protect);
    }
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
