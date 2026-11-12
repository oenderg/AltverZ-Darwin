/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The plant pictures the pointer preview shows, read from the player's OBB.
 *
 * This is the Windows port's loader. It reads Reflourished's data (zstd parts,
 * RGBA8888 atlases) and AltVerZ's (zlib parts, ETC1 colour with an alpha plane).
 * The Mac host uses its own pvz2-cursor-art.m; it can adopt this plain C file in a
 * separate task.
 */
#ifndef PVZ2_CURSOR_IMAGES_H
#define PVZ2_CURSOR_IMAGES_H

#include <stddef.h>

typedef struct {
    char id[64];
    int width, height;
    unsigned char *rgba;   /* premultiplied RGBA, 4 bytes per pixel, top-down */
} pvz2_cursor_image;

typedef struct {
    pvz2_cursor_image *items;
    int count;
} pvz2_cursor_images;

/* Load the packet art and the two HUD pictures the preview needs. 1 on
 * success, 0 on failure with a short reason in why. The path is the player's
 * OBB, never a bundle path. */
int pvz2_cursor_images_load(const char *obb_path, pvz2_cursor_images *out,
                            char *why, size_t whyn);

void pvz2_cursor_images_free(pvz2_cursor_images *images);

#endif /* PVZ2_CURSOR_IMAGES_H */
