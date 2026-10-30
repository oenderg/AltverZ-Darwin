/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "husk-tl-cursor-grid.h"

int main(void)
{
    tl_pvz2_cursor_snapshot s = {
        .grid_valid = 1, .surface_w = 1748, .surface_h = 804,
        .app_w = 3339, .app_h = 1536, .board_x = 1064,
        .app_scale = 2.56f, .camera_scale = 1.27f,
        .camera_x = 2349.27f, .camera_y = 1383.0f,
        .columns = 9, .rows = 5,
    };
    assert(tl_cursor_grid_axis(&s, 0, 825) == 0);
    assert(tl_cursor_grid_axis(&s, 0, 1261) == 4);
    assert(tl_cursor_grid_axis(&s, 0, 1043) == 2);
    assert(tl_cursor_grid_axis(&s, 0, 1044) == 3);
    assert(tl_cursor_grid_axis(&s, 1, 465) == 2);
    assert(tl_cursor_grid_axis(&s, 1, 466) == 3);
    s.row_offset = 15;
    assert(tl_cursor_grid_axis(&s, 1, 465) == 3);
    assert(tl_cursor_grid_band_axis(&s, 1, 465) == 2);
    assert(tl_cursor_grid_band_axis(&s, 1, 466) == 3);
    s.row_offset = 0;
    s.surface_w = 1200; s.surface_h = 552;
    assert(tl_cursor_grid_axis(&s, 0, 716) == 2);
    assert(tl_cursor_grid_axis(&s, 0, 717) == 3);
    for (int axis = 0; axis < 2; axis++) {
        int last = tl_cursor_grid_axis(&s, axis, 0);
        int extent = axis ? s.surface_h : s.surface_w;
        for (int pixel = 1; pixel < extent; pixel++) {
            int next = tl_cursor_grid_axis(&s, axis, pixel);
            assert(next >= last);
            last = next;
        }
    }
    s.grid_valid = 0;
    assert(tl_cursor_grid_axis(&s, 0, 500) == -1000);
    puts("cursor grid: ok");
    return 0;
}
