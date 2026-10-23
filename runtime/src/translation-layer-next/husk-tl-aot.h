/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * husk-tl-aot.h -- guest libraries that were converted ahead of time.
 *
 * With TL_AOT_DIR pointing at a directory of converted dylibs (scripts/aot-convert.py
 * output), tl_ld_load() hands back one of these instead of mapping the ELF, and tl_ld_sym()
 * resolves names through the converter's symbol map. The process then never makes memory
 * executable and never writes code, which is what an iPadOS that enforces TXM requires.
 *
 * See husk-tl-aot.c for what the driver gets and what it costs.
 */
#ifndef HUSK_TL_AOT_H
#define HUSK_TL_AOT_H

#include <stdbool.h>

struct tl_lib;

/* The directory the converted libraries live in (TL_AOT_DIR), or NULL when the route is
 * not in use. */
const char *tl_aot_dir(void);

/* Load every converted library in `dir`. Safe to call twice; returns how many loaded. */
int tl_aot_init(const char *dir);

/* True when at least one converted library was found. */
bool tl_aot_ready(void);

/* Is there a converted dylib for this .so name ("libPVZ2.so")? */
bool tl_aot_has(const char *so_name);

/* The library's image base (the address its guest vaddr 0 maps to), or NULL. */
void *tl_aot_base(const char *so_name);

/* The host address of a guest address in a converted library's code, or NULL when the
 * address is not in that library's code.
 *
 * A converted library keeps the guest's code as native code: the converter rewrites the
 * references, not the instructions. So the guest's own addresses still name it, and the
 * host can call a guest function whose address it knows -- which is what the guest's
 * UI queries need (the game's own "find the widget named UI_SeedPackets" and "the
 * widget's anchored rect").
 *
 * In the translating route there is no such place: the guest's code is translated into
 * an arena, and the arena does not answer to the guest's own numbers. */
void *tl_aot_guest_code_ptr(const char *so_name, uint64_t guest_vaddr);

/* The same, for whichever converted library holds the address. When more than one does
 * the library with the most code wins: the game is the largest of the guest's
 * libraries, and the mod's is smaller than the game's addresses, so the game's own
 * function is never mistaken for the mod's. */
void *tl_aot_guest_code_ptr_any(uint64_t guest_vaddr);

/* The host address of a guest address in a converted library's data segment, or
 * NULL.
 *
 * The guest's data segment is mapped in this process, at a known place (the
 * converter lays it out at code + (data_base - code_base)), so the host can
 * read and write the guest's own globals without the kernel's help -- which
 * matters on iOS, where mach_vm_read and mach_vm_write are not there. */
void *tl_aot_guest_data_ptr(const char *so_name, uint64_t guest_vaddr);
void *tl_aot_guest_data_ptr_any(uint64_t guest_vaddr);

/* The layout, for the record: the two segments' runtime addresses, the guest
 * address the data segment starts at, and the two sizes. */
void tl_aot_layout(const char *so_name, uint64_t *code, uint64_t *data,
                   uint64_t *data_base, uint64_t *code_size, uint64_t *data_size);

/* The guest's thread block: Android's TLS, with the stack-protector cookie in it. */
void *tl_aot_tcb(const char *so_name);

/* Put the guest's thread pointer where the guest's code will find it. Every thread that is
 * about to run converted code needs this, including one the guest created itself. */
void tl_aot_enter(void);

/* A symbol by name, or NULL. The names are the guest's own, so this is how the driver
 * finds JNI_OnLoad and the JNI entry points. */
void *tl_aot_sym(const char *so_name, const char *sym);

/* The stub library the loader hands out for a converted library. */
struct tl_lib *tl_aot_stub(const char *so_name);

/* Run a converted library's constructors, in the guest's own order. Called from
 * tl_ld_init, which is where the ELF's loader would have run them: the guest's
 * constructors call back into Java, so they cannot run when the dylib is loaded. */
bool tl_aot_run_ctors(const char *so_name);

/* Is this library one of the converted ones, and what is it called? */
bool tl_aot_is_aot(const struct tl_lib *L);
const char *tl_aot_name(const struct tl_lib *L);

#endif /* HUSK_TL_AOT_H */