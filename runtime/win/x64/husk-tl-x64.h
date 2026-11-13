/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The guest on x86-64: the game's ARM64 code translated by ScarletNX's recompiler, run on CPU states this file owns.
 *
 * Memory is shared: a guest address is a host address (CPU.mem is NULL), so pointers cross the boundary as they are.
 * What does not cross by itself is a call. The guest reaches a host function through a stub, `svc #n; ret`, in a
 * page the recompiler translates with the libraries; the SVC handler moves the arguments from the guest's registers
 * into Windows x64's by the function's signature (gen-sigs.py) and back. The host reaches a guest function with
 * tl_guest_call, which runs it on this thread's CPU until it returns.
 */
#ifndef HUSK_TL_X64_H
#define HUSK_TL_X64_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The stub page: TL_X64_STUBS stubs of 8 bytes, reserved before the libraries are translated so that the code range
 * covers it. Set up by tl_x64_open, which the loader calls once its memory exists. */
#define TL_X64_STUBS 65536u
bool tl_x64_open(char *err, size_t errlen);

/* A guest-callable address for host function `fn` with signature `sig` (gen-sigs.py's letters). The same function
 * and signature give the same stub. 0 when the page is full. */
uint64_t tl_x64_export(const char *name, void *fn, const char *sig);

/* A guest address for a host function that reads and writes the guest's registers itself: setjmp, longjmp,
 * pthread_create and the others that take a guest function, or a variadic one. `cpu` is the calling CPU. */
struct CPU;
typedef void (*tl_x64_raw)(struct CPU *cpu, void *user);
uint64_t tl_x64_export_raw(const char *name, tl_x64_raw fn, void *user);

/* The signature gen-sigs.py found for an import, or NULL (sigs-tables.inc and the others). */
const char *tl_x64_sig_of(const char *name);

/* The host address the loader gives an import: the stub for a host function, the address itself for data. NULL when
 * a function has no signature (logged once). */
void *tl_x64_import(const char *name, void *host);

/* Translates every library loaded so far, with the stub page, into one module, and makes it current. The cache folder
 * keeps the result: the next start with the same libraries at the same addresses loads it instead. */
bool tl_x64_translate(const char *cache_dir, char *err, size_t errlen);

/* The progress of the cold translation: the units' weight (instructions plus basic-block leaders) done and total, the
 * functions done and total, and whether a translation is running. Written by the workers, safe to read from the UI
 * thread. The totals are 0 until the units are made; a caller's bar should wait for them. */
void tl_x64_progress(unsigned long long *weight_done, unsigned long long *weight_total,
                     unsigned long long *functions_done, unsigned long long *functions_total, int *running);

/* Calls guest function `pc` with arguments laid out by `sig` (the return type's letter, then one per argument) from
 * `args` (integers as they are, a float or double as its bits), on this thread's CPU, and returns x0 (or the bits of
 * v0's low 64 for f or d). Reentrant: a guest call inside a host function the guest called nests. */
uint64_t tl_guest_callv(uint64_t pc, const char *sig, const uint64_t *args);
#define tl_guest_call0(pc, sig) tl_guest_callv((pc), (sig), NULL)
#define tl_guest_call(pc, sig, ...) tl_guest_callv((pc), (sig), (const uint64_t[]){ __VA_ARGS__ })

/* The same with the AAPCS64 registers given directly: x0-x7, v0-v7 (low 64 bits), stack slots, and x8 (the indirect
 * result's address). x0 comes back, v0's low 64 bits in *fpret. */
uint64_t tl_guest_call_regs(uint64_t pc, const uint64_t gp[8], const uint64_t fp[8], const uint64_t *stk, size_t nstk, uint64_t *fpret);
uint64_t tl_guest_call_regs8(uint64_t pc, const uint64_t gp[8], const uint64_t fp[8], const uint64_t *stk, size_t nstk, uint64_t x8, uint64_t *fpret);

/* A host function that runs guest function `pc` (signature `sig`, which must stay valid): for callbacks. */
void *tl_x64_host_fn(uint64_t pc, const char *sig);

/* For a fault guard around guest calls: the CPU and nesting depth before, and put back after a caught fault. */
#define TL_X64_MARK_BYTES 1024
void tl_x64_mark(void *buf);
void tl_x64_recover(const void *buf);

/* This thread's guest CPU (made on first use, with its own guest stack). */
struct CPU *tl_x64_cpu(void);

/* The guest's thread pointer (TPIDR_EL0) for this thread, as tl_aot_enter sets it on ARM64. */
void tl_x64_set_tpidr(uint64_t tp);

#ifdef __cplusplus
}
#endif

#endif /* HUSK_TL_X64_H */
