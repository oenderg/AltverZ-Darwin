/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <Foundation/Foundation.h>
#include <stdbool.h>
#include <stdio.h>

/* The override is parsed as JSON even when its contents have RTON magic. */
int tl_pvz2_level_valid(const char *path, char *err, size_t n)
{
    @autoreleasepool {
        NSString *name = [NSString stringWithUTF8String:path];
        NSDictionary *attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:name error:nil];
        if (!attrs) {
            snprintf(err, n, "cannot stat level");
            return -1;
        }
        if ([attrs fileSize] > 16 * 1024 * 1024) {
            snprintf(err, n, "level is too large to validate");
            return -1;
        }
        NSData *data = [NSData dataWithContentsOfFile:name];
        if (!data) {
            snprintf(err, n, "cannot read level");
            return -1;
        }
        NSError *error = nil;
        id root = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
        if (![root isKindOfClass:[NSDictionary class]]) {
            snprintf(err, n, "not a JSON level: %s", error.localizedDescription.UTF8String ?: "cannot read file");
            return false;
        }
        id objects = root[@"objects"];
        if ([objects isKindOfClass:[NSArray class]]) {
            for (id object in objects) {
                if (![object isKindOfClass:[NSDictionary class]] ||
                    ![object[@"objclass"] isEqual:@"LevelDefinition"] ||
                    ![object[@"objdata"] isKindOfClass:[NSDictionary class]]) continue;
                NSDictionary *level = object[@"objdata"];
                if (([level[@"StageModule"] isKindOfClass:[NSString class]] &&
                     [level[@"StageModule"] length] > 0 &&
                     [level[@"Modules"] isKindOfClass:[NSArray class]] &&
                     [level[@"Modules"] count] > 0) ||
                    ([level[@"RandomLevelSet"] isKindOfClass:[NSArray class]] &&
                     [level[@"RandomLevelSet"] count] > 0)) return true;
            }
        }
        snprintf(err, n, "JSON has no usable LevelDefinition object");
        return false;
    }
}
