/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <Foundation/Foundation.h>

/* The GL thread can be a plain pthread with no Cocoa run loop. Metal's
 * presentDrawable: creates autoreleased objects even though EGL is a C API.
 * Drain them after every swap, once ANGLE has retained what it still needs. */
unsigned tl_egl_swap_buffers_pooled(unsigned (*swap)(void *, void *), void *display, void *surface)
{
    @autoreleasepool {
        return swap(display, surface);
    }
}
