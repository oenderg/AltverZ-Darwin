/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The viewport's arithmetic, in one place, so it can be tested.
 *
 * The game keeps its aspect ratio: it is drawn in the largest rectangle of
 * that shape that fits the window, and the rest of the window is black. The
 * same rectangle maps the touches: a tap in the window becomes a point in the
 * game's own pixels.
 *
 * The window can be any shape -- a portrait window on a tablet, a resized
 * window, the full screen -- and the game is the same in all of them.
 */
#ifndef HUSK_TL_VIEWPORT_H
#define HUSK_TL_VIEWPORT_H

/* The rectangle of the window's size that the game is drawn in. The game's
 * own shape is gw x gh (its pixels); the window is w x h (its points). The
 * rectangle is centred, and it is the largest one of the game's shape that
 * fits. Returns 0 when there is nothing to draw. */
static inline int tl_viewport_fit(double w, double h, double gw, double gh,
                                   double *rx, double *ry, double *rw, double *rh)
{
    if (w <= 0 || h <= 0 || gw <= 0 || gh <= 0) return 0;
    double aspect = gw / gh;
    double vw = w, vh = vw / aspect;
    if (vh > h) { vh = h; vw = vh * aspect; }
    if (rx) *rx = (w - vw) / 2.0;
    if (ry) *ry = (h - vh) / 2.0;
    if (rw) *rw = vw;
    if (rh) *rh = vh;
    return 1;
}

/* A point in the window becomes a point in the game's own pixels. Returns 0
 * when the point is outside the game's rectangle -- a tap in the black bars
 * is not a tap on the game. */
static inline int tl_viewport_map(double px, double py,
                                  double vx, double vy, double vw, double vh,
                                  double gw, double gh,
                                  double *gx, double *gy)
{
    if (vw <= 0 || vh <= 0) return 0;
    double x = (px - vx) * gw / vw;
    double y = (py - vy) * gh / vh;
    /* Half a pixel of slack: the far edge of the game is its last pixel, and
     * the arithmetic lands a hair past it now and then. */
    if (x < -0.5 || y < -0.5 || x > gw + 0.5 || y > gh + 0.5) return 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > gw) x = gw;
    if (y > gh) y = gh;
    if (gx) *gx = x;
    if (gy) *gy = y;
    return 1;
}

#ifdef __OBJC__
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

/* ANGLE derives its window-surface size from layer.bounds * contentsScale,
 * and overwrites drawableSize accordingly. Setting a smaller frame therefore
 * SHRINKS THE BACKBUFFER; the guest's unchanged viewport then crops its image.
 * Keep the layer's untransformed bounds at the logical size and scale only its
 * presentation. Core Animation composites the complete Metal drawable, including
 * depth/stencil rendering, without intercepting any guest GL state or imports.
 * Call on the UI thread; never set this layer's frame elsewhere. */
static inline void tl_viewport_present(CAMetalLayer *layer, double w, double h,
                                       double gw, double gh, double contents_scale)
{
    double x, y, vw, vh;
    if (contents_scale <= 0 || !tl_viewport_fit(w, h, gw, gh, &x, &y, &vw, &vh)) return;
    double s = vw * contents_scale / gw;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.anchorPoint = CGPointZero;
    layer.bounds = CGRectMake(0, 0, gw / contents_scale, gh / contents_scale);
    layer.contentsScale = contents_scale;
    layer.drawableSize = CGSizeMake(gw, gh);
    layer.affineTransform = CGAffineTransformMakeScale(s, s);
    layer.position = CGPointMake(x, y);
    [CATransaction commit];
}
#endif

#endif /* HUSK_TL_VIEWPORT_H */
