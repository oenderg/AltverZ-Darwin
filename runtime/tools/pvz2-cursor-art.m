#import "pvz2-cursor-art.h"
#import <CoreGraphics/CoreGraphics.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t ZSTD_decompress(void *, size_t, const void *, size_t);
size_t ZSTD_findFrameCompressedSize(const void *, size_t);
unsigned ZSTD_isError(size_t);

/* These readers are bounded: a damaged or different OBB must not affect the game. */
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static NSData *read_at(FILE *f, uint64_t offset, size_t length, uint64_t size)
{
    if (offset > size || length > size - offset || length > 100000000 || fseeko(f, (off_t)offset, SEEK_SET)) return nil;
    NSMutableData *data = [NSMutableData dataWithLength:length];
    return fread(data.mutableBytes, 1, length, f) == length ? data : nil;
}

static NSData *group_part(FILE *f, uint64_t file_size, const uint8_t *entry, int part)
{
    uint64_t base = u32(entry + 0x80);
    NSData *header = read_at(f, base, 0x50, file_size);
    if (header.length != 0x50 || memcmp(header.bytes, "pgsr", 4)) return nil;
    const uint8_t *h = header.bytes;
    uint32_t off = u32(h + part), compressed = u32(h + part + 4), size = u32(h + part + 8);
    if (!size || size > 100000000 || off > u32(entry + 0x84) || compressed > u32(entry + 0x84) - off) return nil;
    NSData *data = read_at(f, base + off, compressed, file_size);
    if (!data) return nil;
    if (data.length >= 4 && u32(data.bytes) == 0xfd2fb528) {
        size_t frame = ZSTD_findFrameCompressedSize(data.bytes, data.length);
        if (ZSTD_isError(frame)) return nil;
        NSMutableData *out = [NSMutableData dataWithLength:size];
        size_t n = ZSTD_decompress(out.mutableBytes, size, data.bytes, frame);
        return !ZSTD_isError(n) && n == size ? out : nil;
    }
    return data.length == size ? data : nil;
}

/* The game's RTON is a compact typed object stream with two string tables. */
@interface PVZ2RTON : NSObject
@property(nonatomic, strong) NSData *data;
@property NSUInteger pos;
@property(nonatomic, strong) NSMutableArray<NSString *> *strings;
@property(nonatomic, strong) NSMutableArray<NSString *> *utf8strings;
- (id)value:(uint8_t)type depth:(int)depth;
- (NSDictionary *)object:(int)depth;
@end

@implementation PVZ2RTON
- (uint8_t)byte
{
    if (_pos >= _data.length) @throw @"truncated RTON";
    return ((const uint8_t *)_data.bytes)[_pos++];
}
- (uint64_t)varint
{
    uint64_t result = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        uint8_t c = [self byte];
        result |= (uint64_t)(c & 127) << shift;
        if (!(c & 128)) return result;
    }
    @throw @"invalid RTON varint";
}
- (uint64_t)fixed:(NSUInteger)n
{
    if (n > _data.length - _pos) @throw @"truncated RTON";
    uint64_t v = 0;
    memcpy(&v, (const uint8_t *)_data.bytes + _pos, n);
    _pos += n;
    return v;
}
- (NSString *)string:(BOOL)utf8
{
    if (utf8) [self varint];
    uint64_t len = [self varint];
    if (len > _data.length - _pos) @throw @"truncated RTON string";
    NSString *s = [[NSString alloc] initWithBytes:(const uint8_t *)_data.bytes + _pos length:(NSUInteger)len encoding:NSUTF8StringEncoding];
    _pos += (NSUInteger)len;
    if (!s) @throw @"invalid RTON string";
    return s;
}
- (id)value:(uint8_t)t depth:(int)depth
{
    if (depth > 64) @throw @"RTON nesting";
    switch (t) {
        case 0x00: return @NO; case 0x01: return @YES;
        case 0x09: case 0x0b: case 0x11: case 0x13: case 0x21: case 0x23: case 0x27: case 0x41: case 0x43: case 0x47: return @0;
        case 0x08: case 0x0a: return @([self fixed:1]);
        case 0x10: case 0x12: return @([self fixed:2]);
        case 0x20: case 0x22: case 0x26: return @([self fixed:4]);
        case 0x40: case 0x42: case 0x46: return @([self fixed:8]);
        case 0x24: case 0x28: case 0x44: case 0x48: return @([self varint]);
        case 0x25: case 0x29: case 0x45: case 0x49: { uint64_t n = [self varint]; return @((int64_t)((n >> 1) ^ -(n & 1))); }
        case 0x81: return [self string:NO];
        case 0x82: return [self string:YES];
        case 0x84: return @"RTID(0)";
        case 0x83: {
            uint8_t k = [self byte];
            if (k == 0) return @"RTID(0)";
            if (k == 2) { [self string:YES]; [self varint]; [self varint]; [self fixed:4]; return @"RTID"; }
            if (k == 3) { [self string:YES]; [self string:YES]; return @"RTID"; }
            @throw @"unknown RTON reference";
        }
        case 0x85: return [self object:depth + 1];
        case 0x86: {
            if ([self byte] != 0xfd) @throw @"invalid RTON array";
            uint64_t count = [self varint];
            if (count > _data.length - _pos) @throw @"invalid RTON array size";
            NSMutableArray *a = [NSMutableArray arrayWithCapacity:(NSUInteger)count];
            for (uint64_t i = 0; i < count; i++) [a addObject:[self value:[self byte] depth:depth + 1]];
            if ([self byte] != 0xfe) @throw @"invalid RTON array end";
            return a;
        }
        case 0x90: { NSString *s = [self string:NO]; [_strings addObject:s]; return s; }
        case 0x92: { NSString *s = [self string:YES]; [_utf8strings addObject:s]; return s; }
        case 0x91: case 0x93: {
            uint64_t i = [self varint]; NSArray *a = t == 0x91 ? _strings : _utf8strings;
            if (i >= a.count) @throw @"invalid RTON string index";
            return a[(NSUInteger)i];
        }
        default: @throw [NSString stringWithFormat:@"unknown RTON type %02x", t];
    }
}
- (NSDictionary *)object:(int)depth
{
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    for (;;) {
        uint8_t t = [self byte];
        if (t == 0xff) return d;
        id key = [self value:t depth:depth + 1];
        if (![key isKindOfClass:NSString.class]) @throw @"invalid RTON key";
        d[key] = [self value:[self byte] depth:depth + 1];
    }
}
@end

static CGImageRef crop_image(NSData *atlas, int width, int height, NSDictionary *r)
{
    int x = [r[@"ax"] intValue], y = [r[@"ay"] intValue];
    int w = [r[@"aw"] intValue], h = [r[@"ah"] intValue];
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x > width - w || y > height - h ||
        atlas.length < (size_t)width * height * 4) return NULL;
    NSMutableData *out = [NSMutableData dataWithLength:(size_t)w * h * 4];
    const uint8_t *source = atlas.bytes;
    uint8_t *dest = out.mutableBytes;
    for (int row = 0; row < h; row++) {
        const uint8_t *p = source + ((size_t)(y + row) * width + x) * 4;
        for (int col = 0; col < w; col++, p += 4, dest += 4) {
            unsigned a = p[3];
            dest[0] = (p[0] * a + 127) / 255;
            dest[1] = (p[1] * a + 127) / 255;
            dest[2] = (p[2] * a + 127) / 255;
            dest[3] = a;
        }
    }
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)out);
    CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
    CGImageRef image = CGImageCreate(w, h, 8, 32, w * 4, color,
                                     kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big,
                                     provider, NULL, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(color);
    CGDataProviderRelease(provider);
    return image;
}

NSDictionary<NSString *, id> *pvz2_cursor_art(NSString *path)
{
    FILE *f = fopen(path.fileSystemRepresentation, "rb");
    if (!f) return nil;
    NSDictionary *result = nil;
    @try {
        if (fseeko(f, 0, SEEK_END)) @throw @"OBB seek";
        off_t size = ftello(f);
        if (size < 0x70) @throw @"short OBB";
        NSData *header = read_at(f, 0, 0x70, size);
        const uint8_t *h = header.bytes;
        if (!h || memcmp(h, "1bsr", 4)) @throw @"bad OBB magic";
        uint32_t count = u32(h + 0x28), offset = u32(h + 0x2c), stride = u32(h + 0x30);
        uint32_t texcount = u32(h + 0x54), texoffset = u32(h + 0x58), texstride = u32(h + 0x5c);
        if (stride < 0xcc || stride > 4096 || count > 100000 || texstride < 16 || texstride > 4096 || texcount > 100000) @throw @"bad OBB table";
        NSData *entries = read_at(f, offset, (size_t)count * stride, size);
        NSData *textures = read_at(f, texoffset, (size_t)texcount * texstride, size);
        if (!entries || !textures) @throw @"missing OBB tables";
        const uint8_t *manifest = NULL, *packets = NULL, *hud = NULL;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *e = (const uint8_t *)entries.bytes + (size_t)i * stride;
            if (!memchr(e, 0, 0x80)) continue;
            if (!strcmp((const char *)e, "__MANIFESTGROUP__")) manifest = e;
            else if (!strcmp((const char *)e, "UI_SeedPackets_1536")) packets = e;
            else if (!strcmp((const char *)e, "UI_AlwaysLoaded_768")) hud = e;
        }
        if (!manifest || !packets || !hud) @throw @"missing OBB groups";
        NSData *rton = group_part(f, size, manifest, 0x18);
        if (rton.length < 8 || memcmp(rton.bytes, "RTON", 4)) @throw @"missing resource metadata";
        PVZ2RTON *reader = [PVZ2RTON new];
        reader.data = rton; reader.pos = 8; reader.strings = [NSMutableArray array]; reader.utf8strings = [NSMutableArray array];
        NSDictionary *root = [reader object:0];
        NSMutableDictionary *images = [NSMutableDictionary dictionary];
        for (NSDictionary *group in root[@"groups"]) {
            NSString *name = group[@"id"];
            const uint8_t *entry = [name isEqualToString:@"UI_SeedPackets_1536"] ? packets :
                                   [name isEqualToString:@"UI_AlwaysLoaded_768"] ? hud : NULL;
            if (!entry || u32(entry + 0xc4) != 1) continue;
            uint32_t index = u32(entry + 0xc8);
            if (index >= texcount) continue;
            const uint8_t *t = (const uint8_t *)textures.bytes + (size_t)index * texstride;
            uint32_t width = u32(t), height = u32(t + 4), pitch = u32(t + 8), fmt = u32(t + 12);
            if (!width || !height || width > 8192 || height > 8192 || pitch != width * 4 || fmt != 0) continue;
            NSData *atlas = group_part(f, size, entry, 0x28);
            if (!atlas) continue;
            for (NSDictionary *r in group[@"resources"]) {
                NSString *key = r[@"id"];
                BOOL wanted = [name isEqualToString:@"UI_SeedPackets_1536"] ? [key hasPrefix:@"IMAGE_UI_PACKETS_"] :
                    [key isEqualToString:@"IMAGE_UI_HUD_INGAME_SHOVEL_ICON"] ||
                    [key isEqualToString:@"IMAGE_UI_HUD_INGAME_PLANTFOOD_BANK_COLLECT"];
                if (!wanted || !r[@"aw"] || !r[@"ah"]) continue;
                CGImageRef image = crop_image(atlas, width, height, r);
                if (image) { images[key] = (__bridge id)image; CGImageRelease(image); }
            }
        }
        size_t bytes = 0;
        for (id value in images.allValues) {
            CGImageRef image = (__bridge CGImageRef)value;
            bytes += CGImageGetBytesPerRow(image) * CGImageGetHeight(image);
        }
        NSLog(@"cursor art: %lu decoded image bytes", (unsigned long)bytes);
        result = images;
    } @catch (id error) {
        NSLog(@"cursor art: %@", error);
    }
    fclose(f);
    return result;
}
