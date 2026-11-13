/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Windows loads the game's libraries at run time (husk-tl-ld.c), so there are no
 * converted libraries: the AOT interface answers "none", and its address helpers,
 * which the driver uses for the game's own addresses (GUEST_*), map a guest address
 * into the library loaded at run time instead.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "husk-tl-aot.h"
#include "husk-tl-ld.h"

const char *tl_aot_dir(void) { return NULL; }
int tl_aot_init(const char *dir) { (void)dir; return 0; }
bool tl_aot_ready(void) { return false; }
bool tl_aot_has(const char *so_name) { (void)so_name; return false; }
void *tl_aot_base(const char *so_name) { tl_lib *L = tl_ld_find_lib(so_name); return L ? tl_ld_lib_base(L) : NULL; }
static void *at(const char *so_name, uint64_t vaddr)
{
    tl_lib *L = tl_ld_find_lib(so_name ? so_name : "libPVZ2.so");
    return L ? (uint8_t *)tl_ld_lib_base(L) + vaddr : NULL;
}
void *tl_aot_guest_code_ptr(const char *so_name, uint64_t v) { return at(so_name, v); }
void *tl_aot_guest_code_ptr_any(uint64_t v) { return at(NULL, v); }
void *tl_aot_guest_data_ptr(const char *so_name, uint64_t v) { return at(so_name, v); }
void *tl_aot_guest_data_ptr_any(uint64_t v) { return at(NULL, v); }
void tl_aot_layout(const char *so_name, uint64_t *code, uint64_t *data, uint64_t *data_base, uint64_t *code_size, uint64_t *data_size)
{
    (void)so_name;
    if (code) *code = 0; if (data) *data = 0; if (data_base) *data_base = 0; if (code_size) *code_size = 0; if (data_size) *data_size = 0;
}
void *tl_aot_tcb(const char *so_name) { (void)so_name; return NULL; }
void tl_aot_enter(void) {}
void *tl_aot_sym(const char *so_name, const char *sym) { (void)so_name; (void)sym; return NULL; }
struct tl_lib *tl_aot_stub(const char *so_name) { (void)so_name; return NULL; }
bool tl_aot_run_ctors(const char *so_name) { (void)so_name; return false; }
bool tl_aot_is_aot(const struct tl_lib *L) { (void)L; return false; }
const char *tl_aot_name(const struct tl_lib *L) { (void)L; return NULL; }
