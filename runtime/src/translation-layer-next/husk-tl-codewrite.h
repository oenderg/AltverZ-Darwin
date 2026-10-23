/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stores into code, done through the JIT region's writable view (husk-tl-codewrite.c). */
#ifndef HUSK_TL_CODEWRITE_H
#define HUSK_TL_CODEWRITE_H

#include <stdbool.h>
#include <stddef.h>

/* From now on, stores into the executable view are carried out through the writable one, and the guest may map executable
 * memory of its own (it gets a piece of the JIT region). For mod loaders that hook the game (Geode). */
void tl_codewrite_enable(void);
bool tl_codewrite_enabled(void);
/* Stores carried out so far, for the log. */
long tl_codewrite_count(void);
/* The handler this module installs, or NULL while it is not installed. A watcher that
 * keeps the harness's own handler in place has to be able to tell this one apart from
 * the guest's: the code-write handler is the host's, and taking it away stops the
 * emulation that lets the guest store into its own code. */
void *tl_codewrite_handler(void);

/* The guest asked for PROT_EXEC on a range it owns.  The loader hands data out as the writable view, and a hooking
 * library may keep its trampolines in its own .bss and branch to them, so that range is made read+exec (never
 * writable+executable: the host refuses it) and later stores there are emulated.  Called from the bionic shim. */
void tl_codewrite_note_protect(void *addr, size_t len, int prot);

#endif
