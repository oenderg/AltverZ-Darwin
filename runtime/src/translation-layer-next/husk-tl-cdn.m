/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#include <zlib.h>
#include "husk-tl-cdn.h"
#include "husk-tl-bionic.h"

static const NSUInteger CDN_LIMIT = 64 * 1024 * 1024;

static BOOL allowed(NSURL *url)
{
    return [url.scheme.lowercaseString isEqualToString:@"https"] &&
        [url.host.lowercaseString isEqualToString:@"reflourished.ducnguyen.org"] &&
        (!url.port || url.port.integerValue == 443) && !url.user && !url.password;
}
bool tl_cdn_allowed_url(const char *url)
{
    @autoreleasepool { return url && allowed([NSURL URLWithString:@(url)]); }
}
static NSDateFormatter *date_formatter(void)
{
    NSDateFormatter *f = [NSDateFormatter new];
    f.locale = [[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"];
    f.calendar = [[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian];
    f.timeZone = [NSTimeZone timeZoneForSecondsFromGMT:0];
    f.dateFormat = @"EEE, dd MMM yyyy HH:mm:ss 'GMT'";
    f.lenient = NO;
    return f;
}
static NSString *digest(NSData *body)
{
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(body.bytes, (CC_LONG)body.length, hash);
    NSMutableString *s = [NSMutableString new];
    for (unsigned i = 0; i < sizeof hash; ++i) [s appendFormat:@"%02x", hash[i]];
    return s;
}
/* Reject partial gzip streams before replacing the last usable offline archive. */
static BOOL valid_archive(NSData *body)
{
    if (body.length < 2 || body.length > CDN_LIMIT) return NO;
    const unsigned char *b = body.bytes;
    if (b[0] != 0x1f || b[1] != 0x8b) return NO;
    z_stream z = {0};
    if (inflateInit2(&z, MAX_WBITS + 16) != Z_OK) return NO;
    z.next_in = (Bytef *)body.bytes; z.avail_in = (uInt)body.length;
    unsigned char out[32768]; int result;
    do {
        z.next_out = out; z.avail_out = sizeof out;
        result = inflate(&z, Z_NO_FLUSH);
    } while (result == Z_OK && z.total_out <= 128 * 1024 * 1024);
    BOOL valid = result == Z_STREAM_END && !z.avail_in && z.total_out >= 1024;
    inflateEnd(&z);
    return valid;
}
static NSString *metadata_path(NSString *path) { return [path stringByAppendingString:@".cdn.plist"]; }
static NSMutableDictionary *metadata(NSString *path)
{
    return [[NSDictionary dictionaryWithContentsOfFile:metadata_path(path)] mutableCopy] ?: [NSMutableDictionary new];
}
static BOOL atomic_body(NSData *body, NSString *path)
{
    /* NSDataWritingAtomic writes a sibling temporary file, then renames it. */
    [NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent
        withIntermediateDirectories:YES attributes:nil error:nil];
    NSError *error = nil;
    BOOL ok = [body writeToFile:path options:NSDataWritingAtomic error:&error];
    if (!ok) tl_log_line("pvz2: CDN cache write failed: %s", error.localizedDescription.UTF8String);
    return ok;
}
void tl_cdn_stage(const char *bundled_path, const char *cache_path)
{
    @autoreleasepool {
        NSString *src = @(bundled_path), *dst = @(cache_path);
        NSData *bundle = [NSData dataWithContentsOfFile:src];
        if (!bundle || !valid_archive(bundle)) return;
        NSFileManager *fm = NSFileManager.defaultManager;
        NSDictionary *source = [fm attributesOfItemAtPath:src error:nil];
        NSDictionary *cached = [fm attributesOfItemAtPath:dst error:nil];
        NSMutableDictionary *meta = metadata(dst);
        NSString *hash = digest(bundle);
        BOOL changed = ![meta[@"bundleHash"] isEqualToString:hash];
        BOOL newer = [source[NSFileModificationDate] compare:cached[NSFileModificationDate] ?: NSDate.distantPast] == NSOrderedDescending;
        if (!cached || (changed && newer)) {
            if (!atomic_body(bundle, dst)) return;
            [meta removeObjectForKey:@"headers"];
            meta[@"bodyHash"] = hash;
            tl_log_line("pvz2: CDN cache staged bundled copy %s", cache_path);
        } else {
            tl_log_line("pvz2: CDN cache kept local copy %s", cache_path);
        }
        meta[@"bundleHash"] = hash;
        [meta writeToFile:metadata_path(dst) atomically:YES];
    }
}
static tl_cdn_response *response(NSData *body, NSDictionary *headers, int status)
{
    tl_cdn_response *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->body = malloc(body.length ?: 1);
    if (!r->body) { free(r); return NULL; }
    memcpy(r->body, body.bytes, body.length);
    r->length = body.length; r->status = status; r->refs = 1;
    r->headers = (void *)CFBridgingRetain(headers);
    return r;
}
char *tl_cdn_header(const tl_cdn_response *r, const char *key)
{
    @autoreleasepool {
        NSDictionary *headers = r ? (__bridge NSDictionary *)r->headers : nil;
        NSString *value = headers[[@(key) lowercaseString]];
        return strdup(value.UTF8String ?: "");
    }
}
void tl_cdn_response_retain(tl_cdn_response *r)
{
    if (r) __atomic_add_fetch(&r->refs, 1, __ATOMIC_RELAXED);
}
void tl_cdn_response_free(tl_cdn_response *r)
{
    if (!r || __atomic_sub_fetch(&r->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (r->headers) CFRelease(r->headers);
    free(r->body); free(r);
}

@interface PVCdnFetch : NSObject <NSURLSessionDataDelegate>
@property(nonatomic, strong) NSURLSession *session;
@property(nonatomic, strong) NSMutableData *data;
@property(nonatomic, strong) NSHTTPURLResponse *http;
@property(nonatomic, copy) NSString *url;
@property(nonatomic, copy) NSString *cache;
@property(nonatomic, copy) NSString *failure;
@property(nonatomic) uint64_t token;
@property(nonatomic) tl_cdn_completion completion;
@end

@implementation PVCdnFetch
- (void)fallback:(NSString *)reason
{
    NSData *body = self.cache ? [NSData dataWithContentsOfFile:self.cache] : nil;
    if (!body || !valid_archive(body)) {
        tl_log_line("pvz2: CDN fallback (%s) -> no usable local copy", reason.UTF8String);
        self.completion(self.token, NULL);
        return;
    }
    NSMutableDictionary *meta = metadata(self.cache);
    NSMutableDictionary *headers = [meta[@"bodyHash"] isEqualToString:digest(body)]
        ? [meta[@"headers"] mutableCopy] : nil;
    if (!headers) headers = [NSMutableDictionary new];
    headers[@"date"] = [date_formatter() stringFromDate:NSDate.date];
    headers[@"content-length"] = [NSString stringWithFormat:@"%lu", (unsigned long)body.length];
    headers[@"content-type"] = @"application/x-tar";
    tl_log_line("pvz2: CDN fallback (%s) -> local copy, Date %s", reason.UTF8String, [headers[@"date"] UTF8String]);
    tl_log_line("pvz2: CDN local copy %s (%lu bytes)", self.cache.UTF8String, (unsigned long)body.length);
    self.completion(self.token, response(body, headers, 200));
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)http newRequest:(NSURLRequest *)request
    completionHandler:(void (^)(NSURLRequest *))completionHandler
{
    if (!allowed(request.URL)) self.failure = @"redirect outside CDN allowlist";
    completionHandler(self.failure ? nil : request);
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task
    didReceiveResponse:(NSURLResponse *)http completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler
{
    if ([http isKindOfClass:NSHTTPURLResponse.class]) self.http = (NSHTTPURLResponse *)http;
    if (http.expectedContentLength > (long long)CDN_LIMIT) self.failure = @"response exceeds 64 MiB";
    completionHandler(self.failure ? NSURLSessionResponseCancel : NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data
{
    if (self.data.length + data.length > CDN_LIMIT) {
        self.failure = @"response exceeds 64 MiB"; [task cancel];
    } else [self.data appendData:data];
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error
{
    NSMutableDictionary *headers = [NSMutableDictionary new];
    for (NSString *key in self.http.allHeaderFields)
        headers[key.lowercaseString] = [self.http.allHeaderFields[key] description];
    NSDateFormatter *formatter = date_formatter();
    NSString *date = headers[@"date"];
    NSDate *parsed = date ? [formatter dateFromString:date] : nil;
    NSString *failure = self.failure ?: (error ? error.localizedDescription : nil);
    if (!failure && (self.http.statusCode < 200 || self.http.statusCode >= 300))
        failure = [NSString stringWithFormat:@"HTTP %ld", (long)self.http.statusCode];
    if (!failure && (!parsed || ![[formatter stringFromDate:parsed] isEqualToString:date]))
        failure = @"missing or invalid server Date";
    if (!failure && self.cache && !valid_archive(self.data)) failure = @"invalid gzip archive";
    if (failure) {
        [self fallback:failure];
    } else {
        if (self.cache && atomic_body(self.data, self.cache)) {
            NSMutableDictionary *meta = metadata(self.cache);
            meta[@"bodyHash"] = digest(self.data); meta[@"headers"] = headers;
            [meta writeToFile:metadata_path(self.cache) atomically:YES];
            tl_log_line("pvz2: CDN cache updated local copy %s", self.cache.UTF8String);
        }
        tl_log_line("pvz2: CDN live %s -> %ld, %lu bytes, Date %s", self.url.UTF8String,
            (long)self.http.statusCode, (unsigned long)self.data.length, date.UTF8String);
        self.completion(self.token, response(self.data, headers, (int)self.http.statusCode));
    }
    [session finishTasksAndInvalidate];
    self.session = nil;
}
@end

void tl_cdn_fetch(const char *url, const char *method, const void *body, size_t length,
                  const char *cache_path, uint64_t token, tl_cdn_completion completion)
{
    @autoreleasepool {
        PVCdnFetch *fetch = [PVCdnFetch new];
        fetch.url = @(url);
        fetch.cache = cache_path && [[[NSURL URLWithString:fetch.url] path] isEqualToString:@"/cdn/1.4.2.main.tar"] ? @(cache_path) : nil;
        fetch.token = token; fetch.completion = completion; fetch.data = [NSMutableData new];
        if (!tl_cdn_allowed_url(url)) { completion(token, NULL); return; }
        const char *mode = getenv("TL_PVZ2_CDN");
        if (mode && !strcmp(mode, "offline")) {
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ [fetch fallback:@"forced offline"]; });
            return;
        }
        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:fetch.url]
            cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:30];
        request.HTTPMethod = @(method);
        if (length) request.HTTPBody = [NSData dataWithBytes:body length:length];
        /* The .tar is itself gzip, and the guest inflates it. Do not request a second HTTP encoding. */
        [request setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];
        NSURLSessionConfiguration *config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        /* As Android: 30 s without data (HttpURLConnection, SetTimeout(30)). Android has no total limit and
         * its loading screen waits for the reply, so a total of 60 s bounds that wait. */
        config.timeoutIntervalForRequest = 30; config.timeoutIntervalForResource = 60;
        config.URLCache = nil; config.HTTPCookieStorage = nil; config.URLCredentialStorage = nil;
        NSOperationQueue *queue = [NSOperationQueue new]; queue.maxConcurrentOperationCount = 1;
        fetch.session = [NSURLSession sessionWithConfiguration:config delegate:fetch delegateQueue:queue];
        [[fetch.session dataTaskWithRequest:request] resume];
    }
}
