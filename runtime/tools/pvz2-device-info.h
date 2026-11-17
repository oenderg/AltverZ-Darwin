/* The first lines of pvz2.log on the Mac, iPhone and iPad: the app, the system and the
 * device, so a bug report carries them even when its form is filled in by guesswork. No
 * paths, names or IDs: the log is meant to be posted in public. Windows has its own in
 * pvz2-windows.c. */
#pragma once

#import <Foundation/Foundation.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#include <sys/utsname.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

void tl_log_line(const char *fmt, ...);

static inline NSString *pvz2_sysctl_string(const char *name)
{
    char buf[256];
    size_t len = sizeof buf;
    if (sysctlbyname(name, buf, &len, NULL, 0) != 0 || len == 0) return nil;
    buf[sizeof buf - 1] = 0;
    return [NSString stringWithUTF8String:buf];
}

/* How the iPhone or iPad app got there, as far as its own files tell. */
#if TARGET_OS_IPHONE
static inline const char *pvz2_ios_install_kind(void)
{
    NSString *bundle = NSBundle.mainBundle.bundlePath;
    NSString *parent = bundle.stringByDeletingLastPathComponent;
    NSFileManager *fm = NSFileManager.defaultManager;
    if ([bundle rangeOfString:@"ontainers/Bundle/Application/"].location == NSNotFound)
        return "running from another app's files (LiveContainer or similar)";
    if ([fm fileExistsAtPath:[parent stringByAppendingPathComponent:@"_TrollStore"]] ||
        [fm fileExistsAtPath:[parent stringByAppendingPathComponent:@"_TrollStoreLite"]])
        return "installed by TrollStore";
    if ([fm fileExistsAtPath:[bundle stringByAppendingPathComponent:@"embedded.mobileprovision"]])
        return "signed with a provisioning profile (AltStore, SideStore, Sideloadly or a signing app)";
    return "installed without a provisioning profile";
}
#endif

/* host is "macos" or "ios"; full says whether the bundle carries the game's data. */
static inline void pvz2_log_device(const char *host, bool full)
{
    NSDictionary *info = NSBundle.mainBundle.infoDictionary;
    NSString *name = info[@"CFBundleDisplayName"] ?: info[@"CFBundleName"] ?: @"?";
    NSString *app = info[@"CFBundleShortVersionString"] ?: @"?";
    NSString *game = info[@"PVZ2GameVersion"] ?: @"?";
    tl_log_line("%s: %s %s (the game %s), the %s app", host, name.UTF8String, app.UTF8String,
                game.UTF8String, full ? "full" : "slim");

    NSProcessInfo *proc = NSProcessInfo.processInfo;
    NSOperatingSystemVersion v = proc.operatingSystemVersion;
    /* "Version 26.1 (Build 25B78)": the Simulator's own build, where kern.osversion is the Mac's. */
    NSString *build = nil;
    NSRange b = [proc.operatingSystemVersionString rangeOfString:@"Build "];
    if (b.location != NSNotFound)
        build = [[proc.operatingSystemVersionString substringFromIndex:NSMaxRange(b)]
                    stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@") "]];
    if (!build.length) build = pvz2_sysctl_string("kern.osversion") ?: @"?";
    double gb = (double)proc.physicalMemory / (1024.0 * 1024.0 * 1024.0);
#if TARGET_OS_IPHONE
    NSString *os = UIDevice.currentDevice.systemName ?: @"iOS";
    const char *sim = getenv("SIMULATOR_MODEL_IDENTIFIER");
    struct utsname u;
    NSString *model = sim ? [NSString stringWithFormat:@"%s (Simulator)", sim]
                          : (uname(&u) == 0 ? [NSString stringWithUTF8String:u.machine] : @"?");
    tl_log_line("%s: %s %ld.%ld.%ld (%s) on %s, %.1f GB of memory%s", host, os.UTF8String,
                (long)v.majorVersion, (long)v.minorVersion, (long)v.patchVersion, build.UTF8String,
                model.UTF8String, gb, proc.lowPowerModeEnabled ? ", Low Power Mode on" : "");
    tl_log_line("%s: the app is %s", host, pvz2_ios_install_kind());
#else
    NSString *model = pvz2_sysctl_string("hw.model") ?: @"?";
    NSString *chip = pvz2_sysctl_string("machdep.cpu.brand_string") ?: @"?";
    tl_log_line("%s: macOS %ld.%ld.%ld (%s) on %s, %s, %.0f GB of memory%s", host,
                (long)v.majorVersion, (long)v.minorVersion, (long)v.patchVersion, build.UTF8String,
                model.UTF8String, chip.UTF8String, gb, proc.lowPowerModeEnabled ? ", Low Power Mode on" : "");
#endif
}
