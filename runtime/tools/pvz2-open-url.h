/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PVZ2_OPEN_URL_H
#define PVZ2_OPEN_URL_H

#import <Foundation/Foundation.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

void tl_log_line(const char *fmt, ...);

/* Both hosts validate before the test bypass so refused schemes stay refused in tests. */
static bool pvz2_open_url(const char *text, BOOL (^open)(NSURL *))
{
    @autoreleasepool {
        NSString *value = text ? [NSString stringWithUTF8String:text] : nil;
        NSURL *url = value ? [NSURL URLWithString:value] : nil;
        NSString *scheme = url.scheme.lowercaseString;
        if (!([scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"]) ||
            !url.host.length || [value rangeOfCharacterFromSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].location != NSNotFound) {
            tl_log_line("pvz2: open URL %s refused (only valid HTTP(S) links are supported)", text ?: "(null)");
            return false;
        }
        const char *mode = getenv("TL_PVZ2_OPEN_URL");
        if (mode && !strcmp(mode, "log")) {
            tl_log_line("pvz2: open URL %s (test: not opened)", text);
            return true;
        }

        NSCondition *state = [NSCondition new];
        __block BOOL finished = NO, expired = NO, opened = NO;
        void (^launch)(void) = ^{
            [state lock];
            BOOL cancelled = expired;
            [state unlock];
            if (cancelled) return;
            BOOL result = NO;
            /* An opener may cause lifecycle callbacks; do not hold the waiter's lock. */
            @try { result = open(url); }
            @catch (NSException *exception) {
                tl_log_line("pvz2: open URL %s failed: %s", value.UTF8String, exception.reason.UTF8String);
            }
            [state lock];
            opened = result;
            finished = YES;
            [state signal];
            [state unlock];
        };
        if (NSThread.isMainThread) launch();
        else dispatch_async(dispatch_get_main_queue(), launch);

        /* Lifecycle callbacks can wait for the guest, so a busy main thread must not
         * leave the GL thread waiting forever. Timed-out queued work is cancelled. */
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:2];
        [state lock];
        while (!finished) {
            if (![state waitUntilDate:deadline] && !finished) { expired = YES; break; }
        }
        BOOL result = finished && opened;
        BOOL timedOut = expired;
        [state unlock];
        tl_log_line("pvz2: open URL %s %s", text, timedOut ? "failed (main thread busy)" : result ? "launched" : "failed");
        return result;
    }
}
#endif
