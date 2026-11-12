/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Reflourished 1.4.2's preview art, read from the player's OBB.
 *
 * The OBB is the same 1bsr file the game and the Mac build read. The manifest
 * group holds an RTON stream naming each group and resource with its atlas
 * rectangle; the two texture groups are zstd-compressed RGBA8888 atlases.
 *
 * This file is built by the Windows host now. The Mac host still uses its own
 * pvz2-cursor-art.m for this release and can adopt this API in a separate task.
 * AltVerZ's zlib + ETC1 + alpha OBB is not handled here; that loader lives in
 * AltVerZ-Darwin/runtime/tools/pvz2-cursor-art.m.
 */
#include "pvz2-cursor-images.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t ZSTD_decompress(void *, size_t, const void *, size_t);
size_t ZSTD_findFrameCompressedSize(const void *, size_t);
unsigned ZSTD_isError(size_t);

#ifdef _WIN32
#define cursor_seek(f, off) _fseeki64((f), (long long)(off), SEEK_SET)
#define cursor_end(f) _fseeki64((f), 0, SEEK_END)
#define cursor_tell(f) _ftelli64(f)
#else
#define cursor_seek(f, off) fseeko((f), (off_t)(off), SEEK_SET)
#define cursor_end(f) fseeko((f), 0, SEEK_END)
#define cursor_tell(f) ftello(f)
#endif

static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void set_why(char *why, size_t n, const char *fmt, ...)
{
    if (!why || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, n, fmt, ap);
    va_end(ap);
}

/* The readers are bounded: a damaged or different OBB must not affect the game. */
static unsigned char *read_at(FILE *f, uint64_t offset, size_t length, uint64_t size)
{
    if (offset > size || length > size - offset || length > 100000000) return NULL;
    unsigned char *data = malloc(length ? length : 1);
    if (!data) return NULL;
    if (cursor_seek(f, offset) != 0 || fread(data, 1, length, f) != length) {
        free(data);
        return NULL;
    }
    return data;
}

/* One pgsr part: a zstd frame, or a stored part whose length is its size. */
static unsigned char *group_part(FILE *f, uint64_t file_size,
                                 const unsigned char *entry, int part, size_t *out_size)
{
    *out_size = 0;
    uint64_t base = u32(entry + 0x80);
    unsigned char *header = read_at(f, base, 0x50, file_size);
    if (!header || memcmp(header, "pgsr", 4) != 0) {
        free(header);
        return NULL;
    }
    uint32_t off = u32(header + part);
    uint32_t compressed = u32(header + part + 4);
    uint32_t size = u32(header + part + 8);
    free(header);
    if (!size || size > 100000000 || off > u32(entry + 0x84) ||
        compressed > u32(entry + 0x84) - off)
        return NULL;
    unsigned char *data = read_at(f, base + off, compressed, file_size);
    if (!data) return NULL;
    if (compressed >= 4 && u32(data) == 0xfd2fb528u) {
        size_t frame = ZSTD_findFrameCompressedSize(data, compressed);
        unsigned char *out = NULL;
        if (!ZSTD_isError(frame)) {
            out = malloc(size);
            if (out) {
                size_t n = ZSTD_decompress(out, size, data, frame);
                if (ZSTD_isError(n) || n != size) {
                    free(out);
                    out = NULL;
                }
            }
        }
        free(data);
        if (out) *out_size = size;
        return out;
    }
    if (compressed == size) {
        *out_size = size;
        return data;
    }
    free(data);
    return NULL;
}

/* ------------------------------------------------------------------- RTON
 *
 * A purpose-built reader for the manifest: it walks the root object, the
 * groups array, each group's resources array and the six fields the preview
 * needs. Every other value is skipped with the size its type dictates. The
 * two string tables are pointers into the decompressed manifest; the resource
 * ids are copied out before the manifest is freed.
 */
typedef struct {
    const unsigned char *p;
    uint32_t len;
} rtstr;

typedef struct {
    char id[64];
    int x, y, w, h;
} cursor_res;

typedef struct {
    char id[64];
    cursor_res *v;
    size_t n, cap;
} cursor_group;

typedef struct {
    const unsigned char *b;
    size_t n, pos;
    rtstr *tab[2];
    size_t ntab[2], cap[2];
    int bad;
} rton;

static int rton_byte(rton *r)
{
    if (r->pos >= r->n) {
        r->bad = 1;
        return 0;
    }
    return r->b[r->pos++];
}

static uint64_t rton_varint(rton *r)
{
    uint64_t value = 0;
    int shift = 0;
    for (;;) {
        if (r->pos >= r->n || shift >= 64) {
            r->bad = 1;
            return 0;
        }
        unsigned c = r->b[r->pos++];
        value |= (uint64_t)(c & 127) << shift;
        if (!(c & 128)) return value;
        shift += 7;
    }
}

static uint64_t rton_fixed(rton *r, size_t n)
{
    if (n > 8 || r->pos > r->n || n > r->n - r->pos) {
        r->bad = 1;
        return 0;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < n; i++)
        value |= (uint64_t)r->b[r->pos + i] << (8 * i);
    r->pos += n;
    return value;
}

static int rton_add_string(rton *r, int table, const unsigned char *p, uint32_t len)
{
    if (r->ntab[table] == r->cap[table]) {
        size_t cap = r->cap[table] ? r->cap[table] * 2 : 64;
        if (cap > (1u << 20)) {
            r->bad = 1;
            return 0;
        }
        rtstr *v = realloc(r->tab[table], cap * sizeof *v);
        if (!v) {
            r->bad = 1;
            return 0;
        }
        r->tab[table] = v;
        r->cap[table] = cap;
    }
    r->tab[table][r->ntab[table]].p = p;
    r->tab[table][r->ntab[table]].len = len;
    r->ntab[table]++;
    return 1;
}

static int rton_raw(rton *r, int utf8, rtstr *out)
{
    if (utf8) rton_varint(r);
    uint64_t len = rton_varint(r);
    if (r->bad || len > r->n - r->pos) {
        r->bad = 1;
        return 0;
    }
    out->p = r->b + r->pos;
    out->len = (uint32_t)len;
    r->pos += (size_t)len;
    return 1;
}

static int rton_is_string_type(int type)
{
    return type == 0x81 || type == 0x82 || type == 0x90 || type == 0x91 ||
           type == 0x92 || type == 0x93;
}

static int rton_string(rton *r, int type, rtstr *out)
{
    if (type == 0x81) return rton_raw(r, 0, out);
    if (type == 0x82) return rton_raw(r, 1, out);
    if (type == 0x90 || type == 0x92) {
        int table = type == 0x92;
        if (!rton_raw(r, table, out)) return 0;
        return rton_add_string(r, table, out->p, out->len);
    }
    if (type == 0x91 || type == 0x93) {
        int table = type == 0x93;
        uint64_t i = rton_varint(r);
        if (r->bad || i >= r->ntab[table]) {
            r->bad = 1;
            return 0;
        }
        *out = r->tab[table][i];
        return 1;
    }
    r->bad = 1;
    return 0;
}

static int rton_skip(rton *r, int type, int depth);

static int rton_skip_object(rton *r, int depth)
{
    for (;;) {
        int key_type = rton_byte(r);
        if (r->bad) return 0;
        if (key_type == 0xff) return 1;
        rtstr key;
        if (!rton_string(r, key_type, &key)) return 0;
        int value_type = rton_byte(r);
        if (r->bad || !rton_skip(r, value_type, depth + 1)) return 0;
    }
}

static int rton_skip_array(rton *r, int depth)
{
    if (rton_byte(r) != 0xfd) {
        r->bad = 1;
        return 0;
    }
    uint64_t count = rton_varint(r);
    if (r->bad || count > 1000000) {
        r->bad = 1;
        return 0;
    }
    for (uint64_t i = 0; i < count; i++) {
        int type = rton_byte(r);
        if (r->bad || !rton_skip(r, type, depth + 1)) return 0;
    }
    if (rton_byte(r) != 0xfe) {
        r->bad = 1;
        return 0;
    }
    return 1;
}

static int rton_skip(rton *r, int type, int depth)
{
    if (depth > 64) {
        r->bad = 1;
        return 0;
    }
    switch (type) {
    case 0x00: case 0x01:
    case 0x09: case 0x0b: case 0x11: case 0x13: case 0x21: case 0x23:
    case 0x27: case 0x41: case 0x43: case 0x47: case 0x84:
        return 1;
    case 0x08: case 0x0a: rton_fixed(r, 1); return !r->bad;
    case 0x10: case 0x12: rton_fixed(r, 2); return !r->bad;
    case 0x20: case 0x22: case 0x26: rton_fixed(r, 4); return !r->bad;
    case 0x40: case 0x42: case 0x46: rton_fixed(r, 8); return !r->bad;
    case 0x24: case 0x28: case 0x44: case 0x48:
    case 0x25: case 0x29: case 0x45: case 0x49:
        rton_varint(r);
        return !r->bad;
    case 0x81: case 0x82: case 0x90: case 0x91: case 0x92: case 0x93: {
        rtstr ignored;
        return rton_string(r, type, &ignored);
    }
    case 0x83: {
        int kind = rton_byte(r);
        if (r->bad) return 0;
        if (kind == 0) return 1;
        if (kind == 2) {
            rtstr ignored;
            return rton_raw(r, 1, &ignored) && (rton_varint(r), !r->bad) &&
                   (rton_varint(r), !r->bad) && (rton_fixed(r, 4), !r->bad);
        }
        if (kind == 3) {
            rtstr ignored;
            return rton_raw(r, 1, &ignored) && rton_raw(r, 1, &ignored);
        }
        r->bad = 1;
        return 0;
    }
    case 0x85:
        return rton_skip_object(r, depth);
    case 0x86:
        return rton_skip_array(r, depth);
    default:
        r->bad = 1;
        return 0;
    }
}

static int rton_int(rton *r, int type, long long *out)
{
    switch (type) {
    case 0x00: *out = 0; return 1;
    case 0x01: *out = 1; return 1;
    case 0x09: case 0x0b: case 0x11: case 0x13: case 0x21: case 0x23:
    case 0x27: case 0x41: case 0x43: case 0x47:
        *out = 0;
        return 1;
    case 0x08: case 0x0a: *out = (long long)rton_fixed(r, 1); return !r->bad;
    case 0x10: case 0x12: *out = (long long)rton_fixed(r, 2); return !r->bad;
    case 0x20: case 0x22: case 0x26: *out = (long long)rton_fixed(r, 4); return !r->bad;
    case 0x40: case 0x42: case 0x46: *out = (long long)rton_fixed(r, 8); return !r->bad;
    case 0x24: case 0x28: case 0x44: case 0x48:
        *out = (long long)rton_varint(r);
        return !r->bad;
    case 0x25: case 0x29: case 0x45: case 0x49: {
        uint64_t v = rton_varint(r);
        *out = (long long)((v >> 1) ^ (uint64_t)-(int64_t)(v & 1));
        return !r->bad;
    }
    default:
        return 0;
    }
}

static int key_is(const rtstr *key, const char *s)
{
    size_t n = strlen(s);
    return key->len == n && memcmp(key->p, s, n) == 0;
}

static int group_add(cursor_group *g, const cursor_res *res)
{
    if (g->n == g->cap) {
        size_t cap = g->cap ? g->cap * 2 : 64;
        if (cap > 100000) return 0;
        cursor_res *v = realloc(g->v, cap * sizeof *v);
        if (!v) return 0;
        g->v = v;
        g->cap = cap;
    }
    g->v[g->n++] = *res;
    return 1;
}

static void group_free(cursor_group *g)
{
    free(g->v);
    memset(g, 0, sizeof *g);
}

static void copy_id(char dst[64], const rtstr *src)
{
    dst[0] = 0;
    if (src->len >= 64) return;
    memcpy(dst, src->p, src->len);
    dst[src->len] = 0;
}

/* One resource object: id, ax, ay, aw, ah. */
static int parse_resource(rton *r, cursor_res *res)
{
    memset(res, 0, sizeof *res);
    for (;;) {
        int key_type = rton_byte(r);
        if (r->bad) return 0;
        if (key_type == 0xff) return 1;
        rtstr key;
        if (!rton_string(r, key_type, &key)) return 0;
        int value_type = rton_byte(r);
        if (r->bad) return 0;
        if (key_is(&key, "id")) {
            if (rton_is_string_type(value_type)) {
                rtstr value;
                if (!rton_string(r, value_type, &value)) return 0;
                copy_id(res->id, &value);
            } else if (!rton_skip(r, value_type, 1)) {
                return 0;
            }
        } else if (key_is(&key, "ax") || key_is(&key, "ay") ||
                   key_is(&key, "aw") || key_is(&key, "ah")) {
            long long value;
            if (rton_int(r, value_type, &value)) {
                if (key_is(&key, "ax")) res->x = (int)value;
                else if (key_is(&key, "ay")) res->y = (int)value;
                else if (key_is(&key, "aw")) res->w = (int)value;
                else res->h = (int)value;
            } else if (!rton_skip(r, value_type, 1)) {
                return 0;
            }
        } else if (!rton_skip(r, value_type, 1)) {
            return 0;
        }
    }
}

static int parse_resources(rton *r, int type, cursor_group *g)
{
    if (type != 0x86) return rton_skip(r, type, 1);
    if (rton_byte(r) != 0xfd) {
        r->bad = 1;
        return 0;
    }
    uint64_t count = rton_varint(r);
    if (r->bad || count > 100000) {
        r->bad = 1;
        return 0;
    }
    for (uint64_t i = 0; i < count; i++) {
        int type = rton_byte(r);
        if (r->bad) return 0;
        if (type == 0x85) {
            cursor_res res;
            if (!parse_resource(r, &res)) return 0;
            if (res.id[0] && !group_add(g, &res)) {
                r->bad = 1;
                return 0;
            }
        } else if (!rton_skip(r, type, 2)) {
            return 0;
        }
    }
    if (rton_byte(r) != 0xfe) {
        r->bad = 1;
        return 0;
    }
    return 1;
}

static int parse_group(rton *r, cursor_group *g)
{
    memset(g, 0, sizeof *g);
    for (;;) {
        int key_type = rton_byte(r);
        if (r->bad) return 0;
        if (key_type == 0xff) return 1;
        rtstr key;
        if (!rton_string(r, key_type, &key)) return 0;
        int value_type = rton_byte(r);
        if (r->bad) return 0;
        if (key_is(&key, "id")) {
            if (rton_is_string_type(value_type)) {
                rtstr value;
                if (!rton_string(r, value_type, &value)) return 0;
                copy_id(g->id, &value);
            } else if (!rton_skip(r, value_type, 1)) {
                return 0;
            }
        } else if (key_is(&key, "resources")) {
            if (!parse_resources(r, value_type, g)) return 0;
        } else if (!rton_skip(r, value_type, 1)) {
            return 0;
        }
    }
}

static int parse_groups(rton *r, int type, cursor_group *seed, cursor_group *hud)
{
    if (type != 0x86) return rton_skip(r, type, 1);
    if (rton_byte(r) != 0xfd) {
        r->bad = 1;
        return 0;
    }
    uint64_t count = rton_varint(r);
    if (r->bad || count > 100000) {
        r->bad = 1;
        return 0;
    }
    for (uint64_t i = 0; i < count; i++) {
        int element_type = rton_byte(r);
        if (r->bad) return 0;
        if (element_type == 0x85) {
            cursor_group g;
            if (!parse_group(r, &g)) {
                group_free(&g);
                return 0;
            }
            if (g.id[0] && !strcmp(g.id, "UI_SeedPackets_1536") && seed->n == 0)
                *seed = g;
            else if (g.id[0] && !strcmp(g.id, "UI_AlwaysLoaded_768") && hud->n == 0)
                *hud = g;
            else
                group_free(&g);
        } else if (!rton_skip(r, element_type, 2)) {
            return 0;
        }
    }
    if (rton_byte(r) != 0xfe) {
        r->bad = 1;
        return 0;
    }
    return 1;
}

static int parse_root(rton *r, cursor_group *seed, cursor_group *hud)
{
    for (;;) {
        int key_type = rton_byte(r);
        if (r->bad) return 0;
        if (key_type == 0xff) return 1;
        rtstr key;
        if (!rton_string(r, key_type, &key)) return 0;
        int value_type = rton_byte(r);
        if (r->bad) return 0;
        if (key_is(&key, "groups")) {
            if (!parse_groups(r, value_type, seed, hud)) return 0;
        } else if (!rton_skip(r, value_type, 1)) {
            return 0;
        }
    }
}

/* --------------------------------------------------------------- the atlas */

static int crop_image(const unsigned char *atlas, size_t atlas_size,
                      int width, int height, const cursor_res *res,
                      pvz2_cursor_image *image)
{
    int x = res->x, y = res->y, w = res->w, h = res->h;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || w > width || h > height ||
        x > width - w || y > height - h ||
        atlas_size < (size_t)width * (size_t)height * 4 ||
        (uint64_t)w * (uint64_t)h * 4 > 100000000)
        return 0;
    unsigned char *rgba = malloc((size_t)w * (size_t)h * 4);
    if (!rgba) return 0;
    unsigned char *dst = rgba;
    for (int row = 0; row < h; row++) {
        const unsigned char *src = atlas + ((size_t)(y + row) * (size_t)width + (size_t)x) * 4;
        for (int col = 0; col < w; col++, src += 4, dst += 4) {
            unsigned a = src[3];
            dst[0] = (unsigned char)((src[0] * a + 127) / 255);
            dst[1] = (unsigned char)((src[1] * a + 127) / 255);
            dst[2] = (unsigned char)((src[2] * a + 127) / 255);
            dst[3] = (unsigned char)a;
        }
    }
    image->width = w;
    image->height = h;
    image->rgba = rgba;
    return 1;
}

static int images_add(pvz2_cursor_images *out, const cursor_res *res,
                      const unsigned char *atlas, size_t atlas_size,
                      int width, int height)
{
    pvz2_cursor_image image;
    memset(&image, 0, sizeof image);
    if (!res->id[0]) return 1;
    if (!crop_image(atlas, atlas_size, width, height, res, &image)) return 1;
    if (out->count >= 100000) {
        free(image.rgba);
        return 0;
    }
    pvz2_cursor_image *items = realloc(out->items, (size_t)(out->count + 1) * sizeof *items);
    if (!items) {
        free(image.rgba);
        return 0;
    }
    out->items = items;
    snprintf(image.id, sizeof image.id, "%s", res->id);
    out->items[out->count++] = image;
    return 1;
}

static int load_group(FILE *f, uint64_t file_size, const unsigned char *entry,
                      const unsigned char *textures, uint32_t texcount,
                      uint32_t texstride, const cursor_group *group, int hud,
                      pvz2_cursor_images *out, char *why, size_t whyn)
{
    if (u32(entry + 0xc4) != 1) {
        set_why(why, whyn, "the OBB texture group is marked absent");
        return 0;
    }
    uint32_t index = u32(entry + 0xc8);
    if (index >= texcount) {
        set_why(why, whyn, "the OBB texture group has no texture");
        return 0;
    }
    const unsigned char *t = textures + (size_t)index * texstride;
    uint32_t width = u32(t), height = u32(t + 4), pitch = u32(t + 8), format = u32(t + 12);
    if (!width || !height || width > 8192 || height > 8192 ||
        pitch != width * 4 || format != 0) {
        set_why(why, whyn, "the OBB texture is not RGBA8888");
        return 0;
    }
    size_t atlas_size = 0;
    unsigned char *atlas = group_part(f, file_size, entry, 0x28, &atlas_size);
    if (!atlas || atlas_size < (size_t)width * (size_t)height * 4) {
        free(atlas);
        set_why(why, whyn, "the OBB texture atlas is missing or short");
        return 0;
    }
    for (size_t i = 0; i < group->n; i++) {
        const cursor_res *res = &group->v[i];
        int wanted;
        if (hud)
            wanted = !strcmp(res->id, "IMAGE_UI_HUD_INGAME_SHOVEL_ICON") ||
                     !strcmp(res->id, "IMAGE_UI_HUD_INGAME_PLANTFOOD_BUTTON");
        else
            wanted = !strncmp(res->id, "IMAGE_UI_PACKETS_", 17);
        if (!wanted) continue;
        if (!images_add(out, res, atlas, atlas_size, (int)width, (int)height)) {
            free(atlas);
            set_why(why, whyn, "out of memory");
            return 0;
        }
    }
    free(atlas);
    return 1;
}

int pvz2_cursor_images_load(const char *obb_path, pvz2_cursor_images *out,
                            char *why, size_t whyn)
{
    memset(out, 0, sizeof *out);
    if (why && whyn) why[0] = 0;
    FILE *f = fopen(obb_path, "rb");
    if (!f) {
        set_why(why, whyn, "cannot open the OBB");
        return 0;
    }
    int result = 0;
    unsigned char *header = NULL, *entries = NULL, *textures = NULL, *rton_data = NULL;
    cursor_group seed, hud;
    memset(&seed, 0, sizeof seed);
    memset(&hud, 0, sizeof hud);

    if (cursor_end(f) != 0) {
        set_why(why, whyn, "cannot read the OBB");
        goto done;
    }
    long long file_end = (long long)cursor_tell(f);
    if (file_end < 0x70) {
        set_why(why, whyn, "the OBB is too short");
        goto done;
    }
    uint64_t file_size = (uint64_t)file_end;
    header = read_at(f, 0, 0x70, file_size);
    if (!header || memcmp(header, "1bsr", 4) != 0) {
        set_why(why, whyn, "not a Reflourished OBB (1bsr missing)");
        goto done;
    }
    uint32_t count = u32(header + 0x28), offset = u32(header + 0x2c);
    uint32_t stride = u32(header + 0x30);
    uint32_t texcount = u32(header + 0x54), texoffset = u32(header + 0x58);
    uint32_t texstride = u32(header + 0x5c);
    if (stride < 0xcc || stride > 4096 || count > 100000 ||
        texstride < 16 || texstride > 4096 || texcount > 100000) {
        set_why(why, whyn, "the OBB table is not readable");
        goto done;
    }
    entries = read_at(f, offset, (size_t)count * stride, file_size);
    textures = read_at(f, texoffset, (size_t)texcount * texstride, file_size);
    if (!entries || !textures) {
        set_why(why, whyn, "the OBB table is missing");
        goto done;
    }
    const unsigned char *manifest = NULL, *packets = NULL, *hud_entry = NULL;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *e = entries + (size_t)i * stride;
        if (!memchr(e, 0, 0x80)) continue;
        if (!strcmp((const char *)e, "__MANIFESTGROUP__")) manifest = e;
        else if (!strcmp((const char *)e, "UI_SeedPackets_1536")) packets = e;
        else if (!strcmp((const char *)e, "UI_AlwaysLoaded_768")) hud_entry = e;
    }
    if (!manifest || !packets || !hud_entry) {
        set_why(why, whyn, "the OBB has no seed packet or HUD group");
        goto done;
    }
    size_t rton_size = 0;
    rton_data = group_part(f, file_size, manifest, 0x18, &rton_size);
    if (!rton_data || rton_size < 8 || memcmp(rton_data, "RTON", 4) != 0) {
        set_why(why, whyn, "the OBB resource metadata is missing");
        goto done;
    }
    rton reader;
    memset(&reader, 0, sizeof reader);
    reader.b = rton_data;
    reader.n = rton_size;
    reader.pos = 8;
    int parsed = parse_root(&reader, &seed, &hud);
    free(reader.tab[0]);
    free(reader.tab[1]);
    free(rton_data);
    rton_data = NULL;
    if (!parsed || reader.bad || seed.n == 0 || hud.n == 0) {
        set_why(why, whyn, "the OBB resource metadata is not readable");
        goto done;
    }
    if (!load_group(f, file_size, packets, textures, texcount, texstride, &seed, 0,
                    out, why, whyn) ||
        !load_group(f, file_size, hud_entry, textures, texcount, texstride, &hud, 1,
                    out, why, whyn)) {
        pvz2_cursor_images_free(out);
        goto done;
    }
    if (out->count == 0) {
        set_why(why, whyn, "the OBB has no preview pictures");
        pvz2_cursor_images_free(out);
        goto done;
    }
    result = 1;

done:
    group_free(&seed);
    group_free(&hud);
    free(rton_data);
    free(entries);
    free(textures);
    free(header);
    fclose(f);
    return result;
}

void pvz2_cursor_images_free(pvz2_cursor_images *images)
{
    if (!images) return;
    for (int i = 0; i < images->count; i++) free(images->items[i].rgba);
    free(images->items);
    memset(images, 0, sizeof *images);
}
