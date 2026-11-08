/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HUSK_OBB_WIN_H
#define HUSK_OBB_WIN_H

/* Make sure the game's OBB is in <data>/sdcard/Android/obb/com.ea.game.pvz2_rfl,
 * downloading it first if it is not (or is another version's). Shows a small
 * window with a progress bar while it runs. Returns 1 when the game may start,
 * 0 when the player cancelled. */
int tl_win_obb_ensure(const char *data_dir, const char *exe_dir);

#endif
