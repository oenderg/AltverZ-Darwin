/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The guest on x86-64 (husk-tl-x64.h): ScarletNX's translation of the game's ARM64 code, and the calls across.
 *
 * Guest to host: every host function the guest may call has a stub in the stub page, `svc #n; ret`. The SVC handler
 * finds entry n and either moves the arguments by its signature into a Windows x64 call (tl_x64_invoke) or hands the
 * CPU to a handler that does the work itself. Arguments follow AAPCS64 on the way in: integers in x0-x7, floating point
 * in v0-v7, the rest on the guest's stack in 8-byte slots, in order. Windows x64 takes the first four in rcx, rdx, r8,
 * r9 or xmm0-xmm3 by position; tl_x64_invoke loads each of the four into both, so the callee finds it whichever it is.
 *
 * Host to guest: tl_guest_callv puts the arguments where AAPCS64 wants them on this thread's CPU, returns to a stop
 * address, and runs the translated code until it gets there. A host function the guest called may call the guest
 * again (qsort's comparator): the CPU is saved around the inner run.
 *
 * A guest function given to the host as a callback (pthread_create's start routine) is wrapped in a host function made
 * here, a few bytes of x64 that call tl_guest_callv with the guest address and its signature.
 */
#include "husk-tl-x64.h"

#include <windows.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/cpu/recompiler/snx_recompiler.h"
#include "common/utilities/common.h"
#include "husk-tl-ld.h"
#include "husk-tl-va.h"
#include "husk-tl-xmem.h"

void tl_log_line(const char *fmt, ...);

/* ------------------------------------------------------------------ state */

typedef enum { K_FREE, K_SIG, K_VA, K_RAW } kind;
typedef struct {
    kind k;
    const char *name;
    void *fn;            /* the host function (K_SIG), the variadic implementation (K_VA) */
    const char *sig;
    tl_x64_raw raw;      /* K_RAW */
    void *user;
    bool trace;          /* TL_X64_TRACE names it */
} entry;
static int g_trace_left = 400;   /* the first calls of traced functions, for TL_X64_TRACE */

static entry g_ent[TL_X64_STUBS];
static atomic_ullong g_svc_calls;                /* every guest call into the host, for TL_X64_SAMPLE */
static unsigned g_svc_count[TL_X64_STUBS];      /* per stub (racy increments: an estimate, for the sampler) */
static uint8_t *g_stubs;                         /* the page: TL_X64_STUBS x 8 bytes, then the guest's setjmp and longjmp */
#define STUB_ADDR(i) ((uint64_t)(uintptr_t)(g_stubs + (size_t)(i) * 8))
static uint32_t g_next = 1;                      /* stub 0 is never handed out */
static uint64_t g_stop;                          /* the last stub's address: a run ends when pc gets there */
static SRWLOCK g_lock = SRWLOCK_INIT;
static struct snx_recompiler *g_rec;
static struct snx_recompiled *g_mod;
static RtAPI g_rt;
static uint64_t g_tp;                            /* the guest's thread pointer (the loader's fake thread block) */

/* ---------------------------------------------------- the Windows x64 call */

/*
 * uint64_t tl_x64_invoke(void *fn, const uint64_t *slots, uint64_t n, void *xmm0_out)
 * Calls fn with n 64-bit arguments (each of the first four in its integer register and in its xmm register, the
 * rest on the stack past the 32-byte home area) and returns rax, with xmm0 stored at xmm0_out (16 bytes).
 * `slots` has at least four readable entries.
 */
uint64_t tl_x64_invoke(void *fn, const uint64_t *slots, uint64_t n, void *xmm0_out);
__asm__(
    ".text\n.p2align 4\n.globl tl_x64_invoke\ntl_x64_invoke:\n"
    ".intel_syntax noprefix\n"
    "  push rbp\n  mov rbp, rsp\n  push rbx\n  push rsi\n  push rdi\n  push r12\n"
    "  mov r10, rcx\n  mov rsi, rdx\n  mov rbx, r8\n  mov r12, r9\n"
    "  mov rax, rbx\n  sub rax, 4\n  jg 1f\n  xor eax, eax\n"
    "1:\n  lea rcx, [rax*8 + 32 + 15]\n  and rcx, -16\n  sub rsp, rcx\n"
    "  xor ecx, ecx\n"
    "2:\n  cmp rcx, rax\n  jge 3f\n  mov rdx, [rsi + rcx*8 + 32]\n  mov [rsp + rcx*8 + 32], rdx\n  inc rcx\n  jmp 2b\n"
    "3:\n  mov rcx, [rsi]\n  mov rdx, [rsi + 8]\n  mov r8, [rsi + 16]\n  mov r9, [rsi + 24]\n"
    "  movq xmm0, rcx\n  movq xmm1, rdx\n  movq xmm2, r8\n  movq xmm3, r9\n"
    "  call r10\n"
    "  movdqu [r12], xmm0\n"
    "  lea rsp, [rbp - 32]\n  pop r12\n  pop rdi\n  pop rsi\n  pop rbx\n  pop rbp\n  ret\n"
    ".att_syntax prefix\n");

/* ------------------------------------------------------ arguments, AAPCS64 */

static uint64_t narrow(char t, uint64_t v)
{
    switch (t) {
    case 'c': return (uint64_t)(int64_t)(int8_t)v;
    case 'C': return (uint8_t)v;
    case 's': return (uint64_t)(int64_t)(int16_t)v;
    case 'S': return (uint16_t)v;
    case 'i': return (uint64_t)(int64_t)(int32_t)v;
    case 'I': return (uint32_t)v;
    default:  return v;
    }
}

static bool is_fp(char t) { return t == 'f' || t == 'd'; }

void *tl_x64_host_fn(uint64_t pc, const char *sig);

/* "F{ill}" at p: the inner signature, interned (the host function made for a guest one keeps a pointer to it) */
static const char *inner_sig(const char *p, const char **after)
{
    const char *open = p + 1, *close = strchr(open, '}');
    *after = close;
    static struct { char s[24]; } pool[256];
    static int n;
    size_t len = (size_t)(close - open - 1);
    AcquireSRWLockExclusive(&g_lock);
    const char *r = NULL;
    for (int i = 0; i < n; i++) if (strlen(pool[i].s) == len && !memcmp(pool[i].s, open + 1, len)) { r = pool[i].s; break; }
    if (!r && n < 256 && len < sizeof(pool[0].s)) { memcpy(pool[n].s, open + 1, len); pool[n].s[len] = 0; r = pool[n++].s; }
    ReleaseSRWLockExclusive(&g_lock);
    return r ? r : "v";
}

/* The arguments of a guest-to-host call, in host order, from the CPU by `sig` (past its return letter). */
static int gather(const CPU *c, const char *sig, uint64_t *slots, int cap)
{
    int ngr = 0, nfr = 0, n = 0;
    const uint64_t *stk = (const uint64_t *)(uintptr_t)c->x[31];
    for (const char *p = sig; *p && n < cap; p++) {
        uint64_t v;
        if (*p == 'f') v = nfr < 8 ? c->v[nfr++].s[0] : (uint32_t)*stk++;
        else if (*p == 'd') v = nfr < 8 ? c->v[nfr++].d[0] : *stk++;
        else if (*p == 'F') {
            uint64_t g = ngr < 8 ? c->x[ngr++] : *stk++;
            const char *in = inner_sig(p, &p);
            v = (uint64_t)(uintptr_t)tl_x64_host_fn(g, in);
        }
        else v = narrow(*p, ngr < 8 ? c->x[ngr++] : *stk++);
        slots[n++] = v;
    }
    while (n < 4) slots[n++] = 0;
    return n;
}

static void put_result(CPU *c, char t, uint64_t rax, const uint64_t xmm[2])
{
    switch (t) {
    case 'v': break;
    case 'f': memset(&c->v[0], 0, 16); c->v[0].s[0] = (uint32_t)xmm[0]; break;
    case 'd': memset(&c->v[0], 0, 16); c->v[0].d[0] = xmm[0]; break;
    case 'q': c->v[0].d[0] = xmm[0]; c->v[0].d[1] = xmm[1]; break;
    default:  c->x[0] = narrow(t, rax); break;
    }
}

/* ------------------------------------------------------------- the SVCs */

static void call_va(CPU *c, const entry *e)
{
    tl_va_frame f;
    for (int i = 0; i < 8; i++) { f.gp[i] = c->x[i]; memcpy(&f.fp[i], &c->v[i], 16); }
    f.stack = (void *)(uintptr_t)c->x[31];
    uint64_t slots[4] = { (uint64_t)(uintptr_t)&f, 0, 0, 0 }, xmm[2];
    uint64_t rax = tl_x64_invoke(e->fn, slots, 1, xmm);
    /* the implementation returns an integer or a double: both go back, the guest reads the one it expects */
    c->x[0] = rax;
    memset(&c->v[0], 0, 16); c->v[0].d[0] = xmm[0];
}

static void on_svc(CPU *c, uint32_t imm)
{
    atomic_fetch_add_explicit(&g_svc_calls, 1, memory_order_relaxed);
    if (imm < TL_X64_STUBS) g_svc_count[imm]++;
    const entry *e = imm < TL_X64_STUBS ? &g_ent[imm] : NULL;
    if (!e || e->k == K_FREE) {
        tl_log_line("x64: the guest called stub %u, which is not in use (from %#llx)", imm, (unsigned long long)c->x[30]);
        c->x[0] = 0;
        return;
    }
    if (e->k == K_RAW) { e->raw(c, e->user); return; }
    if (e->k == K_VA) { call_va(c, e); return; }
    uint64_t slots[24], xmm[2];
    int n = gather(c, e->sig + 1, slots, 24);
    if (e->trace && g_trace_left > 0 && !strncmp(e->name, "inflate", 7) && slots[0]) {
        /* z_stream as the guest lays it out: next_in, avail_in, total_in, next_out, avail_out */
        const uint8_t *zs = (const uint8_t *)(uintptr_t)slots[0];
        uint64_t in, out; uint32_t ain, aout; memcpy(&in, zs, 8); memcpy(&ain, zs + 8, 4); memcpy(&out, zs + 24, 8); memcpy(&aout, zs + 32, 4);
        const uint8_t *b = (const uint8_t *)(uintptr_t)in;
        tl_log_line("x64 trace: %s stream: next_in %llx avail_in %u [%02x %02x %02x %02x %02x %02x %02x %02x] next_out %llx avail_out %u", e->name,
                    (unsigned long long)in, ain, b && ain ? b[0] : 0, b && ain > 1 ? b[1] : 0, b && ain > 2 ? b[2] : 0, b && ain > 3 ? b[3] : 0,
                    b && ain > 4 ? b[4] : 0, b && ain > 5 ? b[5] : 0, b && ain > 6 ? b[6] : 0, b && ain > 7 ? b[7] : 0, (unsigned long long)out, aout);
    }
    uint64_t rax = tl_x64_invoke(e->fn, slots, (uint64_t)n, xmm);
    put_result(c, e->sig[0], rax, xmm);
    if (e->trace && g_trace_left > 0) {
        g_trace_left--;
        tl_log_line("x64 trace: %s(%llx, %llx, %llx, %llx) -> %llx [%s] from %#llx", e->name, (unsigned long long)slots[0], (unsigned long long)slots[1],
                    (unsigned long long)slots[2], (unsigned long long)slots[3], (unsigned long long)rax, e->sig, (unsigned long long)c->x[30]);
    }
}

static void on_undef(CPU *c, uint64_t pc, uint32_t word)
{
    const char *lib = NULL; const void *sa = NULL;
    const char *sym = tl_ld_symbol_at((const void *)(uintptr_t)pc, &lib, &sa);
    tl_log_line("x64: undefined instruction %08x at %#llx (%s %s), lr %#llx", word, (unsigned long long)pc,
                lib ? lib : "?", sym ? sym : "?", (unsigned long long)c->x[30]);
    c->pc = g_stop;
}

/* IC IVAU: the guest wrote code in the 64-byte line at `addr` (libRFLib installing a hook): what was translated from it
 * goes, and the next call compiles the line as it is now */
static atomic_ulong g_code_changes;
static void on_code_changed(CPU *c, uint64_t addr)
{
    (void)c;
    if (!g_mod) return;
    size_t n = snx_recompiled_invalidate(g_mod, addr & ~(uint64_t)63, 64, NULL, NULL);
    unsigned long k = atomic_fetch_add(&g_code_changes, 1);
    if (k < 8 || getenv("TL_X64_CODE_TRACE"))
        tl_log_line("x64: code changed at %#llx, %zu translated function(s) retired", (unsigned long long)addr, n);
}

static void on_brk(CPU *c, uint64_t pc, uint32_t imm)
{
    tl_log_line("x64: brk #%u at %#llx, lr %#llx", imm, (unsigned long long)pc, (unsigned long long)c->x[30]);
    c->pc = g_stop;
}

/* ---------------------------------------------------------------- stubs */

/*
 * setjmp and longjmp are guest code: they save and restore the guest's own registers, and longjmp's return lands
 * wherever setjmp's did, which only the translated code can follow. The buffer holds x19-x30, sp, then d8-d15.
 */
static const uint32_t k_setjmp[] = {
    0x910003f0u, 0xa9005013u, 0xa9015815u, 0xa9026017u, 0xa9036819u, 0xa904701bu, 0xa905781du, 0xf9003010u,
    0x6d06a408u, 0x6d07ac0au, 0x6d08b40cu, 0x6d09bc0eu, 0xd2800000u /* mov x0, #0 */, 0xd65f03c0u /* ret */,
};
static const uint32_t k_longjmp[] = {
    0xa9405013u, 0xa9415815u, 0xa9426017u, 0xa9436819u, 0xa944701bu, 0xa945781du, 0xf9403010u, 0x6d46a408u,
    0x6d47ac0au, 0x6d48b40cu, 0x6d49bc0eu, 0x9100021fu /* mov sp, x16 */, 0x7100003fu /* cmp w1, #0 */,
    0x1a9f1420u /* csinc w0, w1, wzr, ne */, 0xd65f03c0u /* ret */,
};
#define HELPER_SETJMP  ((size_t)TL_X64_STUBS * 8)
#define HELPER_LONGJMP (HELPER_SETJMP + 64)
#define STUB_PAGE      (HELPER_SETJMP + 4096)

uint64_t tl_x64_guest_setjmp(void) { return (uint64_t)(uintptr_t)(g_stubs + HELPER_SETJMP); }
uint64_t tl_x64_guest_longjmp(void) { return (uint64_t)(uintptr_t)(g_stubs + HELPER_LONGJMP); }

bool tl_x64_open(char *err, size_t errlen)
{
    if (g_stubs) return true;
    uint8_t *rx, *rw;
    if (!tl_xmem_alloc(STUB_PAGE, &rx, &rw)) { snprintf(err, errlen, "no room for the stub page"); return false; }
    uint32_t *w = (uint32_t *)rw;
    for (uint32_t i = 0; i < TL_X64_STUBS; i++) {
        w[2 * i] = 0xD4000001u | (i << 5);   /* svc #i */
        w[2 * i + 1] = 0xD65F03C0u;          /* ret */
    }
    memcpy(rw + HELPER_SETJMP, k_setjmp, sizeof(k_setjmp));
    memcpy(rw + HELPER_LONGJMP, k_longjmp, sizeof(k_longjmp));
    g_stubs = rx;
    g_stop = STUB_ADDR(TL_X64_STUBS - 1);
    snx_runtime_defaults(&g_rt);
    g_rt.svc = on_svc;
    g_rt.undef = on_undef;
    g_rt.brk = on_brk;
    g_rt.code_changed = on_code_changed;
    return true;
}

static uint64_t add_entry(entry e)
{
    AcquireSRWLockExclusive(&g_lock);
    uint64_t at = 0;
    for (uint32_t i = 1; i < g_next; i++) {
        const entry *o = &g_ent[i];
        if (o->k == e.k && o->fn == e.fn && o->raw == e.raw && o->user == e.user && (o->sig == e.sig || (o->sig && e.sig && !strcmp(o->sig, e.sig)))) { at = STUB_ADDR(i); break; }
    }
    if (!at && g_next < TL_X64_STUBS - 1) {
        /* TL_X64_TRACE=name,name: log their first calls with arguments and result */
        const char *tr = getenv("TL_X64_TRACE");
        if (tr && e.name) {
            size_t l = strlen(e.name);
            for (const char *p = tr; (p = strstr(p, e.name)); p += l)
                if ((p == tr || p[-1] == ',') && (p[l] == ',' || !p[l])) { e.trace = true; break; }
        }
        g_ent[g_next] = e; at = STUB_ADDR(g_next); g_next++;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!at) tl_log_line("x64: the stub page is full (%s)", e.name ? e.name : "?");
    return at;
}

uint64_t tl_x64_export(const char *name, void *fn, const char *sig)
{
    return add_entry((entry){ .k = K_SIG, .name = name, .fn = fn, .sig = sig });
}

uint64_t tl_x64_export_raw(const char *name, tl_x64_raw fn, void *user)
{
    return add_entry((entry){ .k = K_RAW, .name = name, .raw = fn, .user = user });
}

/* TL_VA_STUB on x86-64 (husk-tl-va.h) leaves `ud2`, the bytes "TLVAX6", then the implementation's address. */
static void *va_impl(const void *fn)
{
    static const uint8_t mark[8] = { 0x0F, 0x0B, 'T', 'L', 'V', 'A', 'X', '6' };
    if (!fn || memcmp(fn, mark, 8)) return NULL;
    void *impl; memcpy(&impl, (const uint8_t *)fn + 8, 8);
    return impl;
}

/* ------------------------------------------------------------- signatures */

typedef struct { const char *name, *sig; } sigent;
static sigent k_sigs[] = {
#include "sigs-tables.inc"
#include "sigs-gl.inc"
};
static int sig_cmp(const void *a, const void *b) { return strcmp(((const sigent *)a)->name, ((const sigent *)b)->name); }
static INIT_ONCE g_sig_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK sort_sigs(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    qsort(k_sigs, sizeof(k_sigs) / sizeof(k_sigs[0]), sizeof(k_sigs[0]), sig_cmp);
    return TRUE;
}

const char *tl_x64_sig_of(const char *name)
{
    InitOnceExecuteOnce(&g_sig_once, sort_sigs, NULL, NULL);
    sigent key = { name, NULL };
    const sigent *s = bsearch(&key, k_sigs, sizeof(k_sigs) / sizeof(k_sigs[0]), sizeof(k_sigs[0]), sig_cmp);
    return s ? s->sig : NULL;
}


void *tl_x64_import(const char *name, void *host)
{
    if (!host) return NULL;
    if (!strcmp(name, "setjmp") || !strcmp(name, "_setjmp") || !strcmp(name, "sigsetjmp")) return (void *)(uintptr_t)tl_x64_guest_setjmp();
    if (!strcmp(name, "longjmp") || !strcmp(name, "_longjmp") || !strcmp(name, "siglongjmp")) return (void *)(uintptr_t)tl_x64_guest_longjmp();
    void *impl = va_impl(host);
    if (impl) return (void *)(uintptr_t)add_entry((entry){ .k = K_VA, .name = name, .fn = impl });
    const char *sig = tl_x64_sig_of(name);
    if (!sig) {
        tl_log_line("x64: %s has no signature: the guest gets a stub that answers 0", name);
        return (void *)(uintptr_t)tl_x64_export(name, NULL, NULL);
    }
    if (sig[0] == '*') return host;                       /* data: the guest reads it where it is */
    if (sig[0] == '!') {
        tl_log_line("x64: %s needs a handler of its own (%s): the guest gets a stub that answers 0", name, sig);
        return (void *)(uintptr_t)tl_x64_export(name, NULL, NULL);
    }
    return (void *)(uintptr_t)tl_x64_export(name, host, sig);
}

/* ------------------------------------------------------------ guest calls */

typedef struct {
    CPU cpu;
    uint8_t *stack_lo, *stack_hi;
    int depth;
    DWORD tid;           /* the Windows thread, so the sampler can read where it runs */
} tl_thread;
static __thread tl_thread *t_th;

#define GUEST_STACK (8u << 20)

/* TL_X64_SAMPLE=<seconds>: a thread that logs where each guest thread is, every so often (pc as the translated code
 * last stored it: a function's entry or a call's return, not each instruction). The guest pc says only where the last
 * call went, so each sample also suspends the thread for a moment and reads the host's RIP: inside translated code
 * (and which guest function), the JIT's, or a host function the guest called */
static tl_thread *g_threads[256];
static atomic_int g_nthreads;

static bool sample_host(const tl_thread *t, uint64_t *rip, uint64_t *rsp)
{
    bool got = false;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, t->tid);
    if (!th) return false;
    if (SuspendThread(th) != (DWORD)-1) {
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(th, &ctx)) { *rip = (uint64_t)ctx.Rip; *rsp = (uint64_t)ctx.Rsp; got = true; }
        ResumeThread(th);
    }
    CloseHandle(th);
    return got;
}

static void host_module(uint64_t addr, char *out, size_t n)
{
    HMODULE h = NULL;
    out[0] = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &h) && h) {
        char path[MAX_PATH];
        DWORD len = GetModuleFileNameA(h, path, sizeof(path));
        if (len) {
            const char *base = strrchr(path, '\\');
            snprintf(out, n, "%s+%#llx", base ? base + 1 : path, (unsigned long long)(addr - (uint64_t)(uintptr_t)h));
            return;
        }
    }
    snprintf(out, n, "%#llx", (unsigned long long)addr);
}

static DWORD WINAPI sampler(void *arg)
{
    DWORD ms = (DWORD)(uintptr_t)arg;
    unsigned long long last = 0;
    for (;;) {
        Sleep(ms);
        unsigned long long now = atomic_load(&g_svc_calls);
        tl_log_line("x64 sample: %llu host calls since the last sample", now - last);
        last = now;
        {
            static unsigned prev[TL_X64_STUBS];
            unsigned top[15] = { 0 }; uint32_t at[15] = { 0 };
            for (uint32_t i = 1; i < TL_X64_STUBS; i++) {
                unsigned d = g_svc_count[i] - prev[i]; prev[i] = g_svc_count[i];
                for (int k = 0; k < 15; k++) if (d > top[k]) { for (int m = 14; m > k; m--) { top[m] = top[m - 1]; at[m] = at[m - 1]; } top[k] = d; at[k] = i; break; }
            }
            for (int k = 0; k < 15 && top[k]; k++) tl_log_line("x64 sample:   %u x %s", top[k], g_ent[at[k]].name ? g_ent[at[k]].name : "?");
        }
        int n = atomic_load(&g_nthreads);
        for (int i = 0; i < n; i++) {
            tl_thread *t = g_threads[i];
            if (!t) continue;
            uint64_t pc = t->cpu.pc, lr = t->cpu.x[30];
            const char *lib = NULL, *lib2 = NULL; const void *sa = NULL;
            const char *sym = tl_ld_symbol_at((const void *)(uintptr_t)pc, &lib, &sa);
            if (g_stubs && pc >= STUB_ADDR(0) && pc < STUB_ADDR(TL_X64_STUBS)) { lib = "stub"; sym = g_ent[(pc - STUB_ADDR(0)) / 8].name; }
            const char *sym2 = tl_ld_symbol_at((const void *)(uintptr_t)lr, &lib2, &sa);
            uint64_t slot = 0; int stale = 0;
            int has_slot = g_mod && snx_recompiled_function_state(g_mod, pc, &slot, &stale);
            tl_log_line("x64 sample: cpu %d depth %d pc %#llx (%s %s) lr %#llx (%s %s) dispatch %#llx%s", i, t->depth, (unsigned long long)pc,
                        lib ? lib : "?", sym ? sym : "?", (unsigned long long)lr, lib2 ? lib2 : "?", sym2 ? sym2 : "?",
                        (unsigned long long)slot, has_slot && stale ? " STALE" : "");
            uint64_t rip = 0, rsp = 0;
            if (t->tid && sample_host(t, &rip, &rsp)) {
                uint64_t gs = 0, ge = 0, goff = 0;
                int in_aot = g_mod && snx_recompiled_host_function(g_mod, rip, &gs, &ge, &goff);
                if (in_aot) {
                    const char *hlib = NULL; const void *hsa = NULL;
                    const char *hsym = tl_ld_symbol_at((const void *)(uintptr_t)gs, &hlib, &hsa);
                    tl_log_line("x64 sample: cpu %d host rip %#llx sp %#llx in %s %s (%#llx..%#llx) +%#llx", i, (unsigned long long)rip,
                                (unsigned long long)rsp, hlib ? hlib : "?", hsym ? hsym : "?", (unsigned long long)gs,
                                (unsigned long long)ge, (unsigned long long)goff);
                } else {
                    char mod[160];
                    host_module(rip, mod, sizeof(mod));
                    tl_log_line("x64 sample: cpu %d host rip %#llx sp %#llx at %s (not translated guest code)", i, (unsigned long long)rip,
                                (unsigned long long)rsp, mod);
                }
            }
        }
    }
    return 0;
}
static void register_thread(tl_thread *t)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    int i = atomic_fetch_add(&g_nthreads, 1);
    if (i < 256) g_threads[i] = t; else atomic_store(&g_nthreads, 256);
    BOOL pending = FALSE;
    if (getenv("TL_X64_SAMPLE") && InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
        CloseHandle(CreateThread(NULL, 0, sampler, (void *)(uintptr_t)(atof(getenv("TL_X64_SAMPLE")) * 1000), 0, NULL));
        InitOnceComplete(&once, 0, NULL);
    }
}

CPU *tl_x64_cpu(void)
{
    if (t_th) return &t_th->cpu;
    tl_thread *t = calloc(1, sizeof(*t));
    t->stack_lo = VirtualAlloc(NULL, GUEST_STACK, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!t->stack_lo) { tl_log_line("x64: no guest stack for this thread"); abort(); }
    t->stack_hi = t->stack_lo + GUEST_STACK;
    t->cpu.x[31] = (uint64_t)(uintptr_t)t->stack_hi - 64;
    t->cpu.mem = NULL;
    t->cpu.rt = &g_rt;
    t->cpu.tpidr = g_tp;
    t->cpu.thread = t;
    t->tid = GetCurrentThreadId();
    if (g_mod && snx_recompiled_bind(g_mod, &t->cpu) != 0) tl_log_line("x64: cannot bind this thread's CPU");
    t_th = t;
    register_thread(t);
    return &t->cpu;
}

void tl_x64_set_tpidr(uint64_t tp)
{
    g_tp = tp;
    if (t_th) t_th->cpu.tpidr = tp;
}

/* The run itself: x0-x7 and v0-v7 (their low 64 bits) from the arrays, `nstk` stack slots, then the translated code
 * until it returns. x0 comes back, and v0's low 64 bits in *fpret. */
uint64_t tl_guest_call_regs8(uint64_t pc, const uint64_t gp[8], const uint64_t fp[8], const uint64_t *stk, size_t nstk, uint64_t x8, uint64_t *fpret);
uint64_t tl_guest_call_regs(uint64_t pc, const uint64_t gp[8], const uint64_t fp[8], const uint64_t *stk, size_t nstk, uint64_t *fpret)
{
    return tl_guest_call_regs8(pc, gp, fp, stk, nstk, 0, fpret);
}

/* The same with x8, the indirect result register (a function returning a structure in memory). */
uint64_t tl_guest_call_regs8(uint64_t pc, const uint64_t gp[8], const uint64_t fp[8], const uint64_t *stk, size_t nstk, uint64_t x8, uint64_t *fpret)
{
    CPU *c = tl_x64_cpu();
    tl_thread *t = t_th;
    if (!g_mod) { tl_log_line("x64: a guest call (%#llx) before the code is translated", (unsigned long long)pc); return 0; }
    CPU saved;
    if (t->depth) saved = *c;
    t->depth++;
    for (int i = 0; i < 8; i++) { c->x[i] = gp[i]; memset(&c->v[i], 0, 16); c->v[i].d[0] = fp[i]; }
    uint64_t sp = (c->x[31] - 256 - (uint64_t)nstk * 8) & ~(uint64_t)15;   /* below whatever the guest has in flight */
    if (nstk) memcpy((void *)(uintptr_t)sp, stk, nstk * 8);
    c->x[31] = sp;
    c->x[8] = x8;
    c->x[30] = g_stop;
    c->pc = pc;
    snx_recompiled_run(g_mod, c, pc, g_stop);
    uint64_t r = c->x[0];
    if (fpret) *fpret = c->v[0].d[0];
    t->depth--;
    if (t->depth) *c = saved;
    else c->x[31] = (uint64_t)(uintptr_t)t->stack_hi - 64;
    return r;
}

uint64_t tl_guest_callv(uint64_t pc, const char *sig, const uint64_t *args)
{
    /* AAPCS64: integers to x0-x7, floating point to v0-v7, the rest to the stack in order, 8 bytes each */
    uint64_t gp[8] = { 0 }, fp[8] = { 0 }, stk[32];
    int nstk = 0, ngr = 0, nfr = 0, i = 0;
    for (const char *p = sig + 1; *p; p++, i++) {
        uint64_t v = args[i];
        if (is_fp(*p)) {
            if (*p == 'f') v = (uint32_t)v;
            if (nfr < 8) fp[nfr++] = v; else if (nstk < 32) stk[nstk++] = v;
        } else {
            v = narrow(*p, v);
            if (ngr < 8) gp[ngr++] = v; else if (nstk < 32) stk[nstk++] = v;
        }
    }
    uint64_t fr = 0;
    uint64_t r = tl_guest_call_regs(pc, gp, fp, stk, (size_t)nstk, &fr);
    if (sig[0] == 'f') return (uint32_t)fr;
    if (sig[0] == 'd') return fr;
    return r;
}

/* --------------------------------------------------- host functions for guest ones */

/*
 * A host function that calls guest function `pc` with signature `sig`: 32 bytes of x64,
 *     mov r10, <ctx>; mov rax, <tl_x64_reverse>; jmp rax
 * and tl_x64_reverse spills the four register arguments (both kinds) and calls reverse_dispatch with them and the
 * caller's stack arguments.
 */
typedef struct { uint64_t pc; const char *sig; } rev_ctx;

__attribute__((used)) static uint64_t reverse_dispatch(const rev_ctx *ctx, const uint64_t *gpr, const uint64_t *xmm, const uint64_t *stack, uint64_t *xmm0_out)
{
    uint64_t args[16];
    int i = 0;
    for (const char *p = ctx->sig + 1; *p && i < 16; p++, i++) args[i] = i < 4 ? (is_fp(*p) ? xmm[i * 2] : gpr[i]) : stack[i - 4];
    uint64_t r = tl_guest_callv(ctx->pc, ctx->sig, args);
    xmm0_out[0] = r; xmm0_out[1] = 0;
    return r;
}

void tl_x64_reverse(void);
__asm__(
    ".text\n.p2align 4\n.globl tl_x64_reverse\ntl_x64_reverse:\n"
    ".intel_syntax noprefix\n"
    /* on entry: r10 = ctx, rcx/rdx/r8/r9 and xmm0-3 the arguments, [rsp+8+32..] the stack ones */
    "  push rbp\n  mov rbp, rsp\n"
    "  sub rsp, 160\n"                                    /* gpr[4] at +48, xmm[4] at +80 (16 each), out at +144 */
    "  mov [rsp + 48], rcx\n  mov [rsp + 56], rdx\n  mov [rsp + 64], r8\n  mov [rsp + 72], r9\n"
    "  movdqu [rsp + 80], xmm0\n  movdqu [rsp + 96], xmm1\n  movdqu [rsp + 112], xmm2\n  movdqu [rsp + 128], xmm3\n"
    "  mov rcx, r10\n  lea rdx, [rsp + 48]\n  lea r8, [rsp + 80]\n  lea r9, [rbp + 16 + 32]\n"
    "  lea rax, [rsp + 144]\n  mov [rsp + 32], rax\n"
    "  call reverse_dispatch\n"
    "  movdqu xmm0, [rsp + 144]\n"
    "  mov rsp, rbp\n  pop rbp\n  ret\n"
    ".att_syntax prefix\n");

/* A guest function wrapped for the host. `pc` must be a guest address: a value that is already one of
 * these host thunks is a double wrap (GUEST_FN over tl_jni_native's host form, say), and wrapping it
 * again would ask the translator to run host code as guest code. Such a call is refused loudly. */
#define THUNK_PAGE (1u << 16)
static uint8_t *g_thunk_pages[64];
static int g_thunk_page_count;

void *tl_x64_host_fn(uint64_t pc, const char *sig)
{
    if (!pc) return NULL;
    static struct { uint64_t pc; const char *sig; void *fn; } cache[4096];
    static int n;
    static uint8_t *page; static size_t used = THUNK_PAGE;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_thunk_page_count; i++)
        if ((uint8_t *)(uintptr_t)pc >= g_thunk_pages[i] && (uint8_t *)(uintptr_t)pc < g_thunk_pages[i] + THUNK_PAGE) {
            tl_log_line("x64: %#llx is already a host thunk, not guest code: not wrapping it again",
                        (unsigned long long)pc);
            ReleaseSRWLockExclusive(&g_lock);
            return NULL;
        }
    void *fn = NULL;
    for (int i = 0; i < n; i++) if (cache[i].pc == pc && !strcmp(cache[i].sig, sig)) { fn = cache[i].fn; break; }
    if (!fn && n < 4096) {
        if (used + 48 > THUNK_PAGE) {
            page = VirtualAlloc(NULL, THUNK_PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            used = 0;
            if (page && g_thunk_page_count < 64) g_thunk_pages[g_thunk_page_count++] = page;
        }
        if (page) {
            rev_ctx *ctx = malloc(sizeof(*ctx));
            ctx->pc = pc; ctx->sig = sig;
            uint8_t *t = page + used; used += 48;
            uint64_t c64 = (uint64_t)(uintptr_t)ctx, r64 = (uint64_t)(uintptr_t)tl_x64_reverse;
            t[0] = 0x49; t[1] = 0xBA; memcpy(t + 2, &c64, 8);          /* mov r10, imm64 */
            t[10] = 0x48; t[11] = 0xB8; memcpy(t + 12, &r64, 8);       /* mov rax, imm64 */
            t[20] = 0xFF; t[21] = 0xE0;                                 /* jmp rax */
            FlushInstructionCache(GetCurrentProcess(), t, 32);
            fn = t;
            cache[n].pc = pc; cache[n].sig = sig; cache[n].fn = fn; n++;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return fn;
}

/* ------------------------------------------------------------ translation */

typedef struct { struct snx_code_range r[32]; size_t n; uint64_t *hints; size_t nh, caph; } ranges;

static int add_lib(uintptr_t bias, const char *name, const void *phdr, unsigned phnum, void *user)
{
    ranges *R = user;
    tl_lib *L = tl_ld_find_lib(name);
    (void)L;
    const uint8_t *ph = phdr;
    for (unsigned i = 0; i < phnum && R->n < 31; i++, ph += 56) {
        uint32_t type, flags; uint64_t vaddr, memsz;
        memcpy(&type, ph, 4); memcpy(&flags, ph + 4, 4); memcpy(&vaddr, ph + 16, 8); memcpy(&memsz, ph + 40, 8);
        /* the executable segments; libRFLib's writable ones too, where A64HookFunction builds its trampolines (code the
         * game reaches after the hooks are in) */
        if (type != 1 /* PT_LOAD */ || (!(flags & 1) /* PF_X */ && strcmp(name, "libRFLib.so"))) continue;
        uint64_t a = (bias + vaddr) & ~(uint64_t)3, sz = (memsz + 3) & ~(uint64_t)3;
        R->r[R->n++] = (struct snx_code_range){ .address = a, .code = (const void *)(uintptr_t)a, .size = (size_t)sz };
        tl_log_line("x64: code of %s: %#llx, %zu KiB", name, (unsigned long long)a, (size_t)(sz >> 10));
    }
    return 0;
}

/* Every function the unwind tables and the dynamic symbols know of is a hint (husk-tl-ld.c). */
size_t tl_ld_function_starts(uint64_t **out);

/*
 * ScarletNX loads a module through a copy in %TEMP% (".snx-<pid>-..."), removed when the module is freed. A game that
 * ends (or is ended) without freeing it leaves the copy, 200 MB each time: those of processes no longer running go.
 */
static void remove_tree(const char *dir)
{
    char pat[MAX_PATH + 4]; snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char p[MAX_PATH]; snprintf(p, sizeof(p), "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p); else DeleteFileA(p);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}
static void remove_stale_copies(void)
{
    char tmp[MAX_PATH]; DWORD n = GetTempPathA(sizeof(tmp), tmp);
    if (!n || n >= sizeof(tmp) - 16) return;
    char pat[MAX_PATH + 16]; snprintf(pat, sizeof(pat), "%s.snx-*", tmp);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int removed = 0;
    do {
        unsigned long pid = strtoul(fd.cFileName + 5, NULL, 10);
        if (!pid || pid == GetCurrentProcessId()) continue;
        HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
        bool alive = ph && WaitForSingleObject(ph, 0) == WAIT_TIMEOUT;
        if (ph) CloseHandle(ph);
        if (alive) continue;
        char p[MAX_PATH + 64]; snprintf(p, sizeof(p), "%s%s", tmp, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p); else DeleteFileA(p);
        removed++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (removed) tl_log_line("x64: removed %d translation copies earlier runs left in %s", removed, tmp);
}

/* ScarletNX's lines go to the driver's log when asked for: the invalidator's (TL_X64_CODE_TRACE) say which
 * translated functions a code change retired and which dispatch slots it cleared, the difference between a hook
 * running and a stale function still being reached; a hooking game changes code many times, so those are off by
 * default. The profile lines (TL_X64_PROFILE) time the translation's phases. */
static void snx_log(const char *text)
{
    static int trace = -1, profile = -1;
    if (trace < 0) trace = getenv("TL_X64_CODE_TRACE") ? 1 : 0;
    if (profile < 0) profile = getenv("TL_X64_PROFILE") ? 1 : 0;
    if ((profile && !strncmp(text, "profile:", 8)) || (trace && strncmp(text, "profile:", 8)))
        tl_log_line("%s", text);
}

/* ScarletNX's warnings and errors (snx_error) go to stderr unless a program takes them, and a Windows program's
 * stderr goes nowhere: the log has them, such as a temp folder that falls back to the owner-rights SID. */
static void snx_error_line(const char *line)
{
    tl_log_line("scarletnx: %s", line);
}

/* The cold translation's progress, for the preparing window: the workers report each finished unit's weight, and the
 * UI thread reads the last values. The weight is what the units were sized by, so the bar follows the work. */
static atomic_ullong g_progress_weight, g_progress_weight_total;
static atomic_ullong g_progress_functions, g_progress_functions_total;
static atomic_int g_progress_running;

static void x64_progress(void *user, unsigned long long weight_done, unsigned long long weight_total,
                         unsigned long long functions_done, unsigned long long functions_total)
{
    (void)user;
    atomic_store_explicit(&g_progress_weight, weight_done, memory_order_relaxed);
    atomic_store_explicit(&g_progress_weight_total, weight_total, memory_order_relaxed);
    atomic_store_explicit(&g_progress_functions, functions_done, memory_order_relaxed);
    atomic_store_explicit(&g_progress_functions_total, functions_total, memory_order_relaxed);
}

void tl_x64_progress(unsigned long long *weight_done, unsigned long long *weight_total,
                     unsigned long long *functions_done, unsigned long long *functions_total, int *running)
{
    if (weight_done) *weight_done = atomic_load_explicit(&g_progress_weight, memory_order_relaxed);
    if (weight_total) *weight_total = atomic_load_explicit(&g_progress_weight_total, memory_order_relaxed);
    if (functions_done) *functions_done = atomic_load_explicit(&g_progress_functions, memory_order_relaxed);
    if (functions_total) *functions_total = atomic_load_explicit(&g_progress_functions_total, memory_order_relaxed);
    if (running) *running = atomic_load_explicit(&g_progress_running, memory_order_relaxed);
}

bool tl_x64_translate(const char *cache_dir, char *err, size_t errlen)
{
    atomic_store(&g_progress_weight, 0);
    atomic_store(&g_progress_weight_total, 0);
    atomic_store(&g_progress_functions, 0);
    atomic_store(&g_progress_functions_total, 0);
    atomic_store(&g_progress_running, 1);
    remove_stale_copies();
    ranges R = { 0 };
    tl_ld_iterate(add_lib, &R);
    R.r[R.n++] = (struct snx_code_range){ .address = STUB_ADDR(0), .code = g_stubs, .size = STUB_PAGE };
    uint64_t *hints = NULL;
    size_t nh = tl_ld_function_starts(&hints);
    if (!g_rec) {
        g_rec = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
        if (!g_rec) { snprintf(err, errlen, "the recompiler cannot target this computer"); return false; }
        snx_recompiler_set_log(snx_log);
        snx_error_sink(snx_error_line);
        snx_recompiler_set_progress(g_rec, x64_progress, NULL);
        /* the default target, an Armv8.0 CPU: pointer authentication and BTI (libRFLib's PACIASP) are hints there and run
         * as no-ops; the MAX target decodes them as the real instructions, which the backend has no code for */
        snx_recompiler_set_a64_target(g_rec, SNX_RECOMPILER_A64_SWITCH);
        snx_recompiler_set_verbose(g_rec, getenv("TL_X64_VERBOSE") ? 1 : 0);
        /* libRFLib patches the game's code after it is translated (its hooks): the JIT reads the code where it is, each
         * translated function checks whether its code changed, and IC IVAU reports the change (on_code_changed) */
        snx_recompiler_set_live_code(g_rec, 1);
        snx_recompiler_set_hook_direct_calls(g_rec, 1);
        const char *jobs = getenv("TL_X64_JOBS");
        if (jobs) snx_recompiler_set_jobs(g_rec, (unsigned)strtoul(jobs, NULL, 10));
        const char *opt = getenv("TL_X64_OPT");
        if (opt && opt[0] >= '0' && opt[0] <= '3') snx_recompiler_set_optimization(g_rec, opt[0]);
        if (cache_dir) snx_recompiler_set_cache(g_rec, cache_dir);
    }
    tl_log_line("x64: translating %zu code ranges with %zu function starts", R.n, nh);
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    ULONGLONG cpu0 = ((ULONGLONG)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
                     ((ULONGLONG)user.dwHighDateTime << 32 | user.dwLowDateTime);
    ULONGLONG t0 = GetTickCount64();
    g_mod = snx_recompile_ranges(g_rec, R.r, R.n, hints, nh);
    free(hints);
    if (!g_mod) {
        atomic_store(&g_progress_running, 0);
        snprintf(err, errlen, "%s", snx_recompiler_error(g_rec));
        return false;
    }
    /* The units are all in the module, descriptor, link and load included: what is left is the guest's start. */
    unsigned long long total = atomic_load(&g_progress_weight_total);
    if (total) atomic_store(&g_progress_weight, total);
    total = atomic_load(&g_progress_functions_total);
    if (total) atomic_store(&g_progress_functions, total);
    atomic_store(&g_progress_running, 0);
    ULONGLONG wall = GetTickCount64() - t0;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    ULONGLONG cpu = ((ULONGLONG)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
                    ((ULONGLONG)user.dwHighDateTime << 32 | user.dwLowDateTime) - cpu0;
    tl_log_line("x64: %zu functions translated in %.1f s", snx_recompiled_functions(g_mod), (double)wall / 1000.0);
    tl_log_line("x64: translation used %.1f s CPU over %.1f s wall (%.1f cores)", (double)cpu / 10000000.0,
                (double)wall / 1000.0, wall ? (double)cpu / 10000.0 / (double)wall : 0.0);
    if (t_th) snx_recompiled_bind(g_mod, &t_th->cpu);
    return true;
}

/* For the crash report: where this thread's guest was, when it faults inside translated code. */
void tl_x64_crash_note(void)
{
    if (!t_th) return;
    const CPU *c = &t_th->cpu;
    const char *lib = NULL; const void *sa = NULL;
    const char *sym = tl_ld_symbol_at((const void *)(uintptr_t)c->pc, &lib, &sa);
    tl_log_line("CRASH: guest pc %#llx (%s %s), lr %#llx, sp %#llx, depth %d", (unsigned long long)c->pc, lib ? lib : "?", sym ? sym : "?",
                (unsigned long long)c->x[30], (unsigned long long)c->x[31], t_th->depth);
    for (int i = 0; i < 30; i += 6)
        tl_log_line("CRASH: guest x%-2d %llx %llx %llx %llx %llx %llx", i, (unsigned long long)c->x[i], (unsigned long long)c->x[i + 1],
                    (unsigned long long)c->x[i + 2], (unsigned long long)c->x[i + 3], (unsigned long long)c->x[i + 4], (unsigned long long)c->x[i + 5]);
}

/* The fault guard's snapshot (husk-tl-pvz2.c): a fault inside translated code longjmps past tl_guest_call_regs8, so the
 * guard puts the CPU and the nesting depth back as they were when it went up. */
typedef struct { CPU cpu; int depth; int has; } x64_mark;
_Static_assert(sizeof(x64_mark) <= TL_X64_MARK_BYTES, "TL_X64_MARK_BYTES");
void tl_x64_mark(void *buf)
{
    x64_mark *m = buf;
    m->has = t_th != NULL;
    if (t_th) { m->cpu = t_th->cpu; m->depth = t_th->depth; }
}
void tl_x64_recover(const void *buf)
{
    const x64_mark *m = buf;
    if (!m->has || !t_th) return;
    t_th->cpu = m->cpu;
    t_th->depth = m->depth;
}
