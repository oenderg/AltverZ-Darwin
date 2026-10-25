/* pvz2-keys.h -- the keyboard's mapping of the seed packets, shared by the
 * Mac host and the iPad host.
 *
 * The mapping is one thing in two places, so it is written once here:
 *
 *   - which key means which packet (1..8) and which means the shovel (S);
 *   - where the seed bank is, in the game's own 1748x804.
 *
 * The positions are *measured*, in the guest's own frame (the app's own frame
 * dump, at the game's own resolution), on a level with four packets (Tester's
 * House, Day 4):
 *
 *   - the seed bank is a *column* down the left of the screen, not a row: the
 *     cards are stacked one under the other, the first one's panel spans
 *     y 93..166 and x 12..125, its plant art x 30..83 (the art's centre is
 *     x 56), and every card below it is 79 pixels further down (the panels
 *     are 73 tall with a gap of 6);
 *   - the shovel is the round button at the bottom *right*, its circle
 *     x 1626..1696, y 722..790.
 *
 * The old numbers (a row of packets 120 apart at y 161, the shovel at x 640)
 * were never measured: they put the second packet outside the seed bank
 * altogether and the shovel in the middle of the lawn.
 *
 * The Mac host can correct the numbers in its preferences window; the iPad
 * host has no preferences, so it uses these.
 */
#ifndef PVZ2_KEYS_H
#define PVZ2_KEYS_H

#include <string.h>
#include <strings.h>

#define PVZ2_SLOT_COUNT 8

/* The keys the packets answer to, out of the box. */
static const char *const pvz2_default_slot_keys[PVZ2_SLOT_COUNT] = {
    "1", "2", "3", "4", "5", "6", "7", "8"
};
#define PVZ2_DEFAULT_SHOVEL_KEY "S"

/* The other in-level shortcuts' keys, out of the box: fast-forward, picking
 * up plant food, and the level's power-up buttons from left to right. */
#define PVZ2_DEFAULT_FAST_FORWARD_KEY "F"
#define PVZ2_DEFAULT_PLANT_FOOD_KEY   "Q"
#define PVZ2_POWERUP_COUNT            3
static const char *const pvz2_default_powerup_keys[PVZ2_POWERUP_COUNT] = {
    "Z", "X", "C"
};

/* Where the seed bank is, in the game's own 1748x804. */
#define PVZ2_PACKET_X     56.0
#define PVZ2_PACKET_Y    129.0
#define PVZ2_PACKET_STEP  79.0
#define PVZ2_SHOVEL_X   1661.0
#define PVZ2_SHOVEL_Y    756.0

/* Which slot a key means, or -1. The shovel is PVZ2_SLOT_COUNT.
 *
 * The keys are compared without case: the "S" key and the "s" key are the
 * same key on a keyboard that has a shift key. */
static inline int pvz2_slot_for_key(const char *characters,
                                    const char *const *slot_keys,
                                    const char *shovel_key)
{
    if (!characters || !*characters) return -1;
    for (int i = 0; i < PVZ2_SLOT_COUNT; i++) {
        const char *k = slot_keys ? slot_keys[i] : NULL;
        if (k && *k && strcasecmp(k, characters) == 0) return i;
    }
    if (shovel_key && *shovel_key && strcasecmp(shovel_key, characters) == 0)
        return PVZ2_SLOT_COUNT;
    return -1;
}

/* Where a packet is, in the game's own 1748x804. The seed bank is a column,
 * so a packet's place is the first card's centre and every card below it is
 * one step further down. */
static inline void pvz2_packet_position(int slot, double *x, double *y)
{
    *x = PVZ2_PACKET_X;
    *y = PVZ2_PACKET_Y + PVZ2_PACKET_STEP * slot;
}

/* The guest's own answer about the shovel button: its rect (x, y, w, h) in the
 * game's own 1748x804, as the translation layer asked the game (the game's own
 * widget tree, on the game's own thread, twice a second).
 *
 * The guest's numbers are used only when they agree with the measurement (a
 * few pixels): the measurement is what the shortcut was proved with, and the
 * game's own numbers are welcome only when they say the same thing. When they
 * do not agree, the measured ones stand and the host says so. */
#define PVZ2_GEOMETRY_TOLERANCE 6.0

static inline int pvz2_geometry_agrees(const int *shovel_rect)
{
    if (!shovel_rect || shovel_rect[2] <= 0 || shovel_rect[3] <= 0) return 0;
    double sx = (double)shovel_rect[0] + shovel_rect[2] / 2.0;
    double sy = (double)shovel_rect[1] + shovel_rect[3] / 2.0;
    double dx = sx - PVZ2_SHOVEL_X, dy = sy - PVZ2_SHOVEL_Y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx <= PVZ2_GEOMETRY_TOLERANCE && dy <= PVZ2_GEOMETRY_TOLERANCE;
}

/* The shovel's place: the game's own button when its answer agrees with the
 * measurement, and the measured one otherwise. `from_guest` says which. */
static inline void pvz2_shovel_position(const int *shovel_rect, double *x, double *y, int *from_guest)
{
    *from_guest = 0;
    if (pvz2_geometry_agrees(shovel_rect)) {
        *x = (double)shovel_rect[0] + shovel_rect[2] / 2.0;
        *y = (double)shovel_rect[1] + shovel_rect[3] / 2.0;
        *from_guest = 1;
        return;
    }
    *x = PVZ2_SHOVEL_X;
    *y = PVZ2_SHOVEL_Y;
}

/* ---- the guest's surface size: the DISPLAY's, never the window's ----------
 *
 * The guest reads its size once, at start-up, and keeps it: its aspect and its
 * touch mapping are frozen there. So the surface must come from the display,
 * whatever shape the window the scene starts in happens to be.
 *
 * The sources are gathered by the host (in pixels) and this function picks
 * one. Every candidate must be *plausible*: a side between 800 and 8192
 * pixels, which covers every iPhone and iPad screen and rules out the two
 * things that are not sizes at all -- zero, and the "no limit" sentinel
 * CGFLOAT_MAX (iPadOS hands that out as the scene's maximum size when the
 * scene may be any size, and taking it for a size gave the guest a surface
 * of -1x-1 and a black screen).
 *
 * The largest plausible candidate wins: the display's own pixels are the
 * biggest thing there is, and a window is never bigger than the display.
 * The result is in pixels, in landscape order (the long side first), which is
 * the shape the guest is given.
 *
 * This is a pure function on purpose: scripts/display-size-test.c checks it
 * on the Mac, with the numbers that broke it. */
#define PVZ2_SURFACE_MIN 800.0
#define PVZ2_SURFACE_MAX 8192.0

static inline int pvz2_display_size_plausible(double w, double h)
{
    return w >= PVZ2_SURFACE_MIN && w <= PVZ2_SURFACE_MAX &&
           h >= PVZ2_SURFACE_MIN && h <= PVZ2_SURFACE_MAX;
}

/* The candidates, in pixels, in any order. The biggest plausible one wins;
 * *which_out is its index (-1 when there is none). */
static inline void pvz2_display_size(const double *cand_w, const double *cand_h, int n,
                                     int *out_w, int *out_h, int *which_out)
{
    double best_area = 0;
    int best = -1;
    for (int i = 0; i < n; i++) {
        double w = cand_w[i], h = cand_h[i];
        if (!pvz2_display_size_plausible(w, h)) continue;
        double area = w * h;
        if (area > best_area) { best_area = area; best = i; }
    }
    if (best < 0) {
        *out_w = 0; *out_h = 0;
        if (which_out) *which_out = -1;
        return;
    }
    double w = cand_w[best], h = cand_h[best];
    *out_w = (int)(w > h ? w : h);
    *out_h = (int)(w > h ? h : w);
    if (which_out) *which_out = best;
}

#endif /* PVZ2_KEYS_H */
