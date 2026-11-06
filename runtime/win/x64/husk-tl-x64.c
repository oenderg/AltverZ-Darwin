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
} entry;

static entry g_ent[TL_X64_STUBS];
static uint32_t g_next = 1;                      /* stub 0 is never handed out */
static uint8_t *g_stubs;                         /* the page: TL_X64_STUBS x 8 bytes */
static uint64_t g_stop;                          /* the last stub's address: a run ends when pc gets there */
static SRWLOCK g_lock = SRWLOCK_INIT;
static struct snx_recompiler *g_rec;
static struct snx_recompiled *g_mod;
static RtAPI g_rt;
static uint64_t g_tp;                            /* the guest's thread pointer (the loader's fake thread block) */

#define STUB_ADDR(i) ((uint64_t)(uintptr_t)(g_stubs + (size_t)(i) * 8))

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
    uint64_t rax = tl_x64_invoke(e->fn, slots, (uint64_t)n, xmm);
    put_result(c, e->sig[0], rax, xmm);
}

static void on_undef(CPU *c, uint64_t pc, uint32_t word)
{
    const char *lib = NULL; const void *sa = NULL;
    const char *sym = tl_ld_symbol_at((const void *)(uintptr_t)pc, &lib, &sa);
    tl_log_line("x64: undefined instruction %08x at %#llx (%s %s), lr %#llx", word, (unsigned long long)pc,
                lib ? lib : "?", sym ? sym : "?", (unsigned long long)c->x[30]);
    c->pc = g_stop;
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
    if (!at && g_next < TL_X64_STUBS - 1) { g_ent[g_next] = e; at = STUB_ADDR(g_next); g_next++; }
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
static const sigent k_sigs[] = {
#include "sigs-tables.inc"
};
static int sig_cmp(const void *a, const void *b) { return strcmp(((const sigent *)a)->name, ((const sigent *)b)->name); }

const char *tl_x64_sig_of(const char *name)
{
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
} tl_thread;
static __thread tl_thread *t_th;

#define GUEST_STACK (8u << 20)

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
    if (g_mod && snx_recompiled_bind(g_mod, &t->cpu) != 0) tl_log_line("x64: cannot bind this thread's CPU");
    t_th = t;
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

void *tl_x64_host_fn(uint64_t pc, const char *sig)
{
    if (!pc) return NULL;
    static struct { uint64_t pc; const char *sig; void *fn; } cache[4096];
    static int n;
    static uint8_t *page; static size_t used = 1 << 16;
    AcquireSRWLockExclusive(&g_lock);
    void *fn = NULL;
    for (int i = 0; i < n; i++) if (cache[i].pc == pc && !strcmp(cache[i].sig, sig)) { fn = cache[i].fn; break; }
    if (!fn && n < 4096) {
        if (used + 48 > (1 << 16)) { page = VirtualAlloc(NULL, 1 << 16, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE); used = 0; }
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
        if (type != 1 /* PT_LOAD */ || !(flags & 1) /* PF_X */) continue;
        uint64_t a = (bias + vaddr) & ~(uint64_t)3, sz = (memsz + 3) & ~(uint64_t)3;
        R->r[R->n++] = (struct snx_code_range){ .address = a, .code = (const void *)(uintptr_t)a, .size = (size_t)sz };
        tl_log_line("x64: code of %s: %#llx, %zu KiB", name, (unsigned long long)a, (size_t)(sz >> 10));
    }
    return 0;
}

/* Every function the unwind tables and the dynamic symbols know of is a hint (husk-tl-ld.c). */
size_t tl_ld_function_starts(uint64_t **out);

bool tl_x64_translate(const char *cache_dir, char *err, size_t errlen)
{
    ranges R = { 0 };
    tl_ld_iterate(add_lib, &R);
    R.r[R.n++] = (struct snx_code_range){ .address = STUB_ADDR(0), .code = g_stubs, .size = STUB_PAGE };
    uint64_t *hints = NULL;
    size_t nh = tl_ld_function_starts(&hints);
    if (!g_rec) {
        g_rec = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
        if (!g_rec) { snprintf(err, errlen, "the recompiler cannot target this computer"); return false; }
        /* the default target, an Armv8.0 CPU: pointer authentication and BTI (libRFLib's PACIASP) are hints there and run
         * as no-ops; the MAX target decodes them as the real instructions, which the backend has no code for */
        snx_recompiler_set_a64_target(g_rec, SNX_RECOMPILER_A64_SWITCH);
        snx_recompiler_set_verbose(g_rec, getenv("TL_X64_VERBOSE") ? 1 : 0);
        if (cache_dir) snx_recompiler_set_cache(g_rec, cache_dir);
    }
    tl_log_line("x64: translating %zu code ranges with %zu function starts", R.n, nh);
    ULONGLONG t0 = GetTickCount64();
    g_mod = snx_recompile_ranges(g_rec, R.r, R.n, hints, nh);
    free(hints);
    if (!g_mod) { snprintf(err, errlen, "%s", snx_recompiler_error(g_rec)); return false; }
    tl_log_line("x64: %zu functions translated in %.1f s", snx_recompiled_functions(g_mod), (double)(GetTickCount64() - t0) / 1000.0);
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
