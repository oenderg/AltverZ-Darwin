/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

int tl_pvz2_level_valid(const char *, char *, size_t);

int main(void)
{
    @autoreleasepool {
        char path[1024];
        snprintf(path, sizeof(path), "%s/pvz2-level-test.XXXXXX", NSTemporaryDirectory().fileSystemRepresentation);
        int fd = mkstemp(path);
        assert(fd >= 0);
        close(fd);
        NSArray *cases = @[
            @[@"RTON PlayerInfo", @0],
            @[@"{", @0],
            @[@"{\"objects\":[{\"objclass\":\"LevelDefinition\",\"objdata\":{}}]}", @0],
            @[@"{\"objects\":[{\"objclass\":\"LevelDefinition\",\"objdata\":{\"StageModule\":\"RTID(A@B)\",\"Modules\":[]}}]}", @0],
            @[@"{\"objects\":[{\"objclass\":\"LevelDefinition\",\"objdata\":{\"StageModule\":\"RTID(A@B)\",\"Modules\":[\"RTID(C@B)\"]}}]}", @1],
            @[@"{\"objects\":[{\"objclass\":\"LevelDefinition\",\"objdata\":{\"RandomLevelSet\":[\"test\"]}}]}", @1]
        ];
        char err[256];
        for (NSArray *test in cases) {
            assert([test[0] writeToFile:@(path) atomically:NO encoding:NSUTF8StringEncoding error:nil]);
            assert(tl_pvz2_level_valid(path, err, sizeof(err)) == [test[1] intValue]);
        }
        unlink(path);
        assert(tl_pvz2_level_valid(path, err, sizeof(err)) == -1);
        puts("level validation checks passed");
    }
}
