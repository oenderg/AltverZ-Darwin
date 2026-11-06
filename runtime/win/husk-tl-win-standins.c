/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The runtime's Android framework pieces that the Darwin build makes from Apple's
 * frameworks (CoreText/ImageIO bitmaps, media, sensors, SQLite, web views) and that
 * PvZ2 does not use: on Windows they answer "not here". The game's Java side is the
 * driver's own (husk-tl-pvz2.c), which reaches none of them.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "husk-tl-dvm-internal.h"

dvm_native_fn tl_gfx_native(const char *name, const char *sig) { (void)name; (void)sig; return NULL; }
dvm_native_fn tl_audio_native(const char *name, const char *sig) { (void)name; (void)sig; return NULL; }
dvm_native_fn tl_sensors_native(const char *name, const char *sig) { (void)name; (void)sig; return NULL; }
dvm_native_fn tl_sqlite_native(const char *name, const char *sig) { (void)name; (void)sig; return NULL; }
dvm_native_fn tl_media_native(const char *name, const char *sig) { (void)name; (void)sig; return NULL; }
dvm_native_fn tl_web_native(const char *name, const char *sig) { (void)name; (void)sig; return NULL; }
bool tl_web_any_visible(void) { return false; }
bool tl_ui_frame_take(const uint8_t **px, int *w, int *h, bool *any) { (void)px; (void)w; (void)h; if (any) *any = false; return false; }
void tl_ui_frame_done(void) {}
bool tl_ui_has_frame(void) { return false; }

/* Code memory is writable on Windows (one view, read-write-execute), so the guest's
 * stores into its own code need no emulation. */
void tl_codewrite_enable(void) {}
bool tl_codewrite_enabled(void) { return false; }
long tl_codewrite_count(void) { return 0; }
void *tl_codewrite_handler(void) { return NULL; }
void tl_codewrite_note_protect(void *addr, size_t len, int prot) { (void)addr; (void)len; (void)prot; }
