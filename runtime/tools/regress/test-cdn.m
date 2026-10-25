/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise cache replacement and offline headers without contacting a server. */
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#include <assert.h>
#include <stdarg.h>
#include <zlib.h>
#include "husk-tl-cdn.h"

void tl_log_line(const char *fmt, ...)
{
    va_list args; va_start(args, fmt); vprintf(fmt, args); putchar('\n'); va_end(args);
}
static NSData *archive(unsigned char fill)
{
    unsigned char input[2048]; memset(input, fill, sizeof input);
    unsigned char output[4096]; z_stream z = {0};
    assert(deflateInit2(&z, 6, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    z.next_in = input; z.avail_in = sizeof input; z.next_out = output; z.avail_out = sizeof output;
    assert(deflate(&z, Z_FINISH) == Z_STREAM_END);
    NSData *data = [NSData dataWithBytes:output length:z.total_out]; deflateEnd(&z); return data;
}
static tl_cdn_response *answer;
static dispatch_semaphore_t finished;
static void completed(uint64_t token, tl_cdn_response *r)
{
    assert(token == 42); answer = r; dispatch_semaphore_signal(finished);
}
int main(void)
{
    @autoreleasepool {
        assert(tl_cdn_allowed_url("https://reflourished.ducnguyen.org/cdn/1.4.2.main.tar"));
        const char *rejected[] = {"http://reflourished.ducnguyen.org/", "https://reflourished.ducnguyen.org.evil.test/",
            "https://evil.test/reflourished.ducnguyen.org/", "https://user@reflourished.ducnguyen.org/",
            "https://reflourished.ducnguyen.org:444/", "file:///tmp/main.tar", "not a URL"};
        for (unsigned i = 0; i < sizeof rejected / sizeof *rejected; ++i) assert(!tl_cdn_allowed_url(rejected[i]));
        NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *bundle = [dir stringByAppendingPathComponent:@"bundle.tar"], *cache = [dir stringByAppendingPathComponent:@"local/main.tar"];
        NSData *old = archive('a'), *fresh = archive('b'), *update = archive('c');
        [old writeToFile:bundle atomically:YES];
        tl_cdn_stage(bundle.UTF8String, cache.UTF8String);
        assert([[NSData dataWithContentsOfFile:cache] isEqual:old]);
        [fresh writeToFile:cache atomically:YES];
        tl_cdn_stage(bundle.UTF8String, cache.UTF8String);
        assert([[NSData dataWithContentsOfFile:cache] isEqual:fresh]);
        [update writeToFile:bundle atomically:YES];
        [NSFileManager.defaultManager setAttributes:@{NSFileModificationDate:[NSDate dateWithTimeIntervalSinceNow:2]}
            ofItemAtPath:bundle error:nil];
        tl_cdn_stage(bundle.UTF8String, cache.UTF8String);
        assert([[NSData dataWithContentsOfFile:cache] isEqual:update]);
        /* Matching metadata is required before an offline answer can reuse an ETag. */
        NSString *sidecar = [cache stringByAppendingString:@".cdn.plist"];
        NSMutableDictionary *meta = [[NSDictionary dictionaryWithContentsOfFile:sidecar] mutableCopy];
        meta[@"headers"] = @{@"etag":@"fixture-etag", @"date":@"Mon, 01 Jan 2001 00:00:00 GMT"};
        [meta writeToFile:sidecar atomically:YES];
        setenv("TL_PVZ2_CDN", "offline", 1);
        finished = dispatch_semaphore_create(0);
        tl_cdn_fetch("https://reflourished.ducnguyen.org/cdn/1.4.2.main.tar", "GET", NULL, 0, cache.UTF8String, 42, completed);
        assert(!dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)));
        assert(answer && answer->status == 200 && answer->length == update.length);
        assert(!memcmp(answer->body, update.bytes, update.length));
        char *etag = tl_cdn_header(answer, "ETag"), *date = tl_cdn_header(answer, "DATE");
        assert(!strcmp(etag, "fixture-etag"));
        NSDateFormatter *format = [NSDateFormatter new]; format.locale = [[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"];
        format.timeZone = [NSTimeZone timeZoneForSecondsFromGMT:0]; format.dateFormat = @"EEE, dd MMM yyyy HH:mm:ss 'GMT'";
        NSDate *parsed = [format dateFromString:@(date)]; assert(parsed && fabs(parsed.timeIntervalSinceNow) < 5);
        free(etag); free(date); tl_cdn_response_retain(answer); tl_cdn_response_free(answer); tl_cdn_response_free(answer);
        [@"broken gzip" writeToFile:cache atomically:YES encoding:NSUTF8StringEncoding error:nil];
        answer = NULL;
        tl_cdn_fetch("https://reflourished.ducnguyen.org/cdn/1.4.2.main.tar", "GET", NULL, 0, cache.UTF8String, 42, completed);
        assert(!dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) && !answer);
        [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
        puts("PASS: host allowlist, cache retention, bundle upgrade, fresh offline Date, cached ETag, gzip rejection");
    }
}
