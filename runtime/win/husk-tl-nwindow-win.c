/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The game's one ANativeWindow on Windows: the host's window (an HWND), which ANGLE
 * draws to directly. The Darwin build keeps this with the NDK functions
 * (husk-tl-bionic-ndk.c) and swaps through a pool of IOSurfaces; here a swap is
 * ANGLE's own.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct tl_nwindow { atomic_int refs; int width, height, format; void *layer; void *texture; void *bits; } tl_nwindow;
static tl_nwindow g_window = { 1, 1280, 720, 1, NULL, NULL, NULL };

void tl_nwindow_configure(int w, int h, void *layer) { g_window.width = w; g_window.height = h; g_window.layer = layer; }
void tl_nwindow_resize(int w, int h) { g_window.width = w; g_window.height = h; }
void *tl_nwindow_get(void) { atomic_fetch_add(&g_window.refs, 1); return &g_window; }
void *tl_nwindow_native(void *window) { return window ? ((tl_nwindow *)window)->layer : NULL; }
int tl_nwindow_width(void *window) { return window ? ((tl_nwindow *)window)->width : 0; }
int tl_nwindow_height(void *window) { return window ? ((tl_nwindow *)window)->height : 0; }
void *tl_nwindow_texture(void *window) { (void)window; return NULL; }
void tl_nwindow_post(void *window, const void *rgba, int w, int h, bool flip) { (void)window; (void)rgba; (void)w; (void)h; (void)flip; }

unsigned tl_egl_swap_buffers_pooled(unsigned (*swap)(void *, void *), void *display, void *surface) { return swap(display, surface); }
