/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../pvz2-open-url.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

void tl_log_line(const char *fmt, ...)
{
    va_list args; va_start(args, fmt); vprintf(fmt, args); va_end(args); putchar('\n');
}

static void pump_until(dispatch_semaphore_t done)
{
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:5];
    while (dispatch_semaphore_wait(done, DISPATCH_TIME_NOW)) {
        assert(deadline.timeIntervalSinceNow > 0);
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
}

int main(void)
{
    @autoreleasepool {
        __block int launches = 0;
        BOOL (^opener)(NSURL *) = ^BOOL(NSURL *url) {
            assert(NSThread.isMainThread && url.host.length);
            ++launches; return YES;
        };
        unsetenv("TL_PVZ2_OPEN_URL");
        const char *refused[] = {NULL, "", "not a URL", "file:///tmp/test", "javascript:alert(1)",
            "mailto:test@example.com", "ftp://example.com", "https:///", "https://example.com/a\nb"};
        for (unsigned i = 0; i < sizeof refused / sizeof *refused; ++i)
            assert(!pvz2_open_url(refused[i], opener));
        assert(launches == 0);
        assert(pvz2_open_url("https://example.com/help?x=1#faq", opener));
        assert(pvz2_open_url("HTTP://example.com/", opener));
        assert(launches == 2);
        assert(!pvz2_open_url("https://example.com/fail", ^BOOL(NSURL *url) { (void)url; return NO; }));
        assert(!pvz2_open_url("https://example.com/throw", ^BOOL(NSURL *url) {
            (void)url;
            @throw [NSException exceptionWithName:@"Test" reason:@"test failure" userInfo:nil];
        }));
        setenv("TL_PVZ2_OPEN_URL", "log", 1);
        assert(pvz2_open_url("https://example.com/test", opener));
        assert(!pvz2_open_url("file:///tmp/test", opener));
        assert(launches == 2);
        unsetenv("TL_PVZ2_OPEN_URL");

        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
            assert(pvz2_open_url("https://example.com/worker", opener));
            dispatch_semaphore_signal(done);
        });
        pump_until(done);
        assert(launches == 3);

        /* A lifecycle wait on main must refuse queued launch work without opening later. */
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
            assert(!pvz2_open_url("https://example.com/busy", opener));
            dispatch_semaphore_signal(done);
        });
        assert(!dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC)));
        dispatch_async(dispatch_get_main_queue(), ^{ dispatch_semaphore_signal(done); });
        pump_until(done);
        assert(launches == 3);
        puts("PASS: URL schemes, log-only mode, failures, exceptions, main-thread launch and busy-main cancellation; no external apps opened");
    }
}
