/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HUSK_TL_CURSOR_GRID_H
#define HUSK_TL_CURSOR_GRID_H

#include <math.h>
#include "husk-tl-pvz2.h"

/* Match the integer and single-precision stages in the guest's input path. */
static inline int tl_cursor_grid_axis_offset(const tl_pvz2_cursor_snapshot *s, int axis, int pixel, int row_offset)
{
    int surface = axis ? s->surface_h : s->surface_w;
    int app = axis ? s->app_h : s->app_w;
    int board = axis ? s->board_y : s->board_x;
    float pivot = axis ? s->camera_y : s->camera_x;
    if (!s->grid_valid || surface <= 0 || app <= 0 || s->camera_scale <= 0 || s->app_scale <= 0)
        return -1000;
    int u = (int)((double)pixel * app / surface) - board;
    float converted = pivot - (float)board + floorf(((float)u - pivot + (float)board) / s->camera_scale);
    int cursor = (int)((float)(int)converted / s->app_scale);
    if (axis) return (int)floorf(((float)(cursor + row_offset) - 160.0f) / 76.0f);
    return (int)floorf(((float)cursor - 200.0f) / 64.0f);
}

static inline int tl_cursor_grid_axis(const tl_pvz2_cursor_snapshot *s, int axis, int pixel)
{
    return tl_cursor_grid_axis_offset(s, axis, pixel, (int)s->row_offset);
}

/* A plant's pickup offset selects the row, but does not move the row's rectangle. */
static inline int tl_cursor_grid_band_axis(const tl_pvz2_cursor_snapshot *s, int axis, int pixel)
{
    return tl_cursor_grid_axis_offset(s, axis, pixel, 0);
}
#endif
