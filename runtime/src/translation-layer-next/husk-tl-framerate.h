/* -- the rate the host presents at, and the game's own setting.
 *
 * The guest's Java implements Graphics_SetFramerate(int) as a *sleep* before
 * every frame, and that sleep never runs here: the host calls the guest's
 * update directly. So the setting reaches the host as a request, and the host
 * decides the rate.
 *
 * The game's switch is "Target 60 FPS":
 *
 *   ON   -- the guest asks for 60, and the host presents at the display's own
 *           maximum (120 on the iPad Pro, 60 on a 60 Hz screen).
 *   OFF  -- the guest asks for 30, and the host presents at 60. Thirty is the
 *           game's stock rate on a slow phone; on a display that can do more
 *           it is a slideshow, and the game is delta-driven (its speed follows
 *           the clock, not the frame count), so presenting more often makes it
 *           smoother and not faster.
 *
 * The display's maximum is the ceiling in both cases: a host never presents
 * faster than the screen can show.
 *
 * This is a function of its own, and it is tested (scripts/framerate-test.c).
 */
#ifndef HUSK_TL_FRAMERATE_H
#define HUSK_TL_FRAMERATE_H

/* The rate to present at. `asked` is what the guest asked for, `max` is what
 * the display can do. */
static inline int tl_framerate_target(int asked, int max)
{
    if (max < 1) max = 1;
    int target = (asked >= 60) ? max : 60;   /* on: the display's maximum; off: 60 */
    if (target > max) target = max;         /* never faster than the screen */
    if (target < 1) target = 1;
    return target;
}

#endif /* HUSK_TL_FRAMERATE_H */
