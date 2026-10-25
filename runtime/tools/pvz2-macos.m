/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * pvz2-macos.m -- the Mac host for the JIT-free route.
 *
 * The same driver the iPad runs, in a window: AppKit on a CAMetalLayer, the mouse is
 * one finger, the Mac's keyboard is the guest's keyboard, and the guest's libraries are
 * the Mach-O dylibs the converter produced (scripts/aot-convert.py) -- so nothing in
 * this process is translated and nothing in it is made executable. That is the whole
 * difference between this and the translating host in pvz2-window.m.
 *
 * Built by tools/regress/build-macos-app.sh into
 *     build/pvz2-macos/PvZ2 Reflourished.app
 *
 * Everything it reads is in the bundle:
 *     Contents/MacOS/PvZ2 Reflourished   this program
 *     Contents/Frameworks/*.dylib        the converted guest libraries (and their
 *                                        .facts files, which carry the symbol map)
 *     Contents/Frameworks/libEGL.dylib   ANGLE: the EGL the driver needs on a Mac
 *     Contents/Game/pvz2.apk              the game's APK (the Java half)
 *     Contents/Game/obb/*.obb             the game's expansion file (its assets)
 *     Contents/Game/1.4.2.main.tar        the mod's CDN archive
 *
 * The writable state is in ~/Library/Application Support/PvZ2 Reflourished (the guest's
 * own files under sdcard/, its saves under No_Backup/, its settings under config/),
 * and the log is next to it.
 */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Sparkle/Sparkle.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/clonefile.h>
#include <sys/file.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>
#include "husk-tl-audio.h"
#include "husk-tl-egl.h"
#include "husk-tl-jni.h"
#include "husk-tl-ld.h"
#include "husk-tl-pvz2.h"
#include "husk-tl-cdn.h"
#include "husk-tl-viewport.h"
#include "pvz2-keys.h"

static tl_ga_config config;
static atomic_bool ready;

/* The packet in hand: the keyboard took it out of the seed bank and the
 * player's next click is where it goes. See packet_take_in_hand. */
static atomic_int g_packet_in_hand;
static atomic_uint g_packet_hand_generation;
static double g_packet_hand_x, g_packet_hand_y;
static void packet_carry_to(double gx, double gy);
static void cursor_set_game(double gx, double gy);
static NSString *apkPath, *dataPath, *eglPath, *glesPath;
static const int surfaceWidth = 1748, surfaceHeight = 804;

/* The paths, as plain C strings that live as long as the process does.
 *
 * An NSString's -UTF8String points into the string's own storage, and for a string
 * that is not owned by anything the compiler can see, that storage is not guaranteed
 * to be there when the driver reads the config on its own thread: the first run of
 * this host passed an EGL path that had turned into rubbish by the time the driver
 * read it, and the symptom was "egl: cannot load" with a path full of noise. So the
 * paths are copied into buffers that nothing can move. */
static char g_apk[1024], g_data[1024], g_egl[1024], g_gles[1024];

/* The view, so that a test can tap in the window's own points -- which is how
 * the mapping from the window to the game is checked. */
@interface PVZ2View : NSView
@property(nonatomic, strong) CAMetalLayer *metal;
- (void)windowTap:(int)phase at:(NSPoint)p;
- (void)applyPresentation;
- (void)followDisplayScale;
@end
static PVZ2View *g_view;
static NSWindow *g_window;

void tl_log_line(const char *fmt, ...);

/* The surface's size in pixels: the game is drawn into it, and the game's own
 * space is mapped onto it (game_to_surface). */
static atomic_int g_surface_w = 1748, g_surface_h = 804;
/* The shape the game laid itself out for: the surface it started with. The
 * guest builds a screen's layout once, when the screen opens, so a window of
 * another shape would crop the open screen or leave it short -- the picture is
 * kept in this shape (scaled, with bars), and in the adapt mode a button
 * restarts the game in the window's shape, the way Android's size
 * compatibility mode does. Main thread only. */
static int g_layout_w = 1748, g_layout_h = 804;
static NSButton *g_fit_button, *g_keep_button;
/* The window's frame the last time it had the game's shape: the cancel button
 * puts it back there. */
static NSRect g_good_frame;
/* A restart to fit the window: the new copy waits for the old one to save and
 * leave, and opens where the old window was. */
static bool g_relaunch;
static NSRect g_relaunch_frame;

/* The resize mode.
 *
 * "Fixed" (the default; "Letterbox" is its old name): the window keeps one
 * shape -- the display's own, or the one chosen in the settings -- and the game
 * is laid out for that shape and fills the window at any size: no bars.
 * "Adapt" (experimental): the window takes any shape between 4:3 and the widest
 * the game fills, and the game starts in the window's shape.
 *
 * Either way the guest lays a screen out once, for the shape it started in. A
 * window of another shape (a resize in the adapt mode, a new choice in the
 * settings) gets the picture scaled with bars, a button that restarts the game
 * in the window's shape, and one that puts the window back in the game's.
 */
static NSString *const kResizeMode = @"resizeMode";
static NSString *const kFixedAspect = @"fixedAspect";
static NSString *const kCloseAction = @"closeAction";   /* "quit" (default) or "pause" */
static bool close_keeps_game(void)
{
    return [[[NSUserDefaults standardUserDefaults] stringForKey:kCloseAction] isEqualToString:@"pause"];
}
/* Cmd-Q, and closing the window when that quits, ask first (default yes). */
static NSString *const kConfirmQuit = @"confirmQuit";
static bool confirm_quit(void)
{
    id v = [[NSUserDefaults standardUserDefaults] objectForKey:kConfirmQuit];
    return v ? [v boolValue] : true;
}
/* The scale the game draws at: the display's own -- or, with "Sharper on
 * standard displays" on, twice that on a display that is not Retina (drawn
 * large and scaled down, the way macOS's own scaled modes do it). */
static NSString *const kRenderSharp = @"renderSharp";
/* While the game is starting, its surface keeps the size it started with: a
 * new size then crashes it a few seconds later -- the size the host writes
 * into the game's original screen size (app + 0x86c) leaves its start-up half
 * built for one size and half for the other. A resize then only scales the
 * picture, and the game gets the window's size once the driver says it has
 * started (tl_pvz2_started: its save loaded and the frames smooth, about 10 s
 * in), or after 20 s at the latest (g_starting). A full-screen switch hands it
 * the final size only, not every step of the animation (g_fs_moving). */
static bool g_starting = true, g_fs_moving;
static double render_scale(NSWindow *window)
{
    /* The screen the frame is on: a window that is not on screen yet reports
     * the main display's scale as its own. */
    NSScreen *screen = window.screen ?: NSScreen.mainScreen;
    double s = screen.backingScaleFactor > 0 ? screen.backingScaleFactor : 1.0;
    if (s < 2 && [[NSUserDefaults standardUserDefaults] boolForKey:kRenderSharp]) s *= 2;
    return s;
}
static bool adapt_mode(void)
{
    NSString *mode = [[NSUserDefaults standardUserDefaults] stringForKey:kResizeMode];
    /* No setting means the default, and the default is the fixed mode.
     * (Messaging nil returns zero, and zero is NSOrderedSame, so without this
     * check an app with no preference at all would start in the *adapt* mode
     * -- the experimental one.) */
    if (!mode.length) return false;
    return [mode caseInsensitiveCompare:@"Adapt"] == NSOrderedSame;
}

/* The shapes the game fills: from the iPad's 4:3 to 2.4:1 (an ultrawide
 * display's 3440x1440, and 21:9). Measured: the title, the map and a level fill
 * 21:9 (ex39), and a level's art ends at about 2.56:1 (black past it); a window
 * narrower than 4:3 cut the title off. TL_PVZ2_MAX_ASPECT lets a test try
 * another limit. */
static double aspect_min(void) { return 4.0 / 3.0; }
static double aspect_max(void)
{
    const char *e = getenv("TL_PVZ2_MAX_ASPECT");
    double v = e ? atof(e) : 0;
    return v > 1.0 ? v : 2.4;
}
static double clamp_aspect(double a) { return fmin(fmax(a, aspect_min()), aspect_max()); }

/* A display's own shape: what the game fills in full screen -- below the camera
 * housing, on a display that has one (full screen keeps clear of it). */
static double display_aspect(NSScreen *s)
{
    if (!s) s = NSScreen.mainScreen;
    NSSize f = s.frame.size;
    double top = 0;
    if (@available(macOS 12.0, *)) top = s.safeAreaInsets.top;
    return f.height - top > 0 ? f.width / (f.height - top) : 16.0 / 10.0;
}

/* The fixed mode's shape: "display" (the default: the display the window is
 * on), "game" (the game's own 1748x804), or "w:h". */
static double fixed_aspect(NSScreen *s)
{
    NSString *v = [[NSUserDefaults standardUserDefaults] stringForKey:kFixedAspect];
    double a, x, y;
    if ([v isEqualToString:@"game"]) a = (double)surfaceWidth / surfaceHeight;
    else if (v.length && sscanf(v.UTF8String, "%lf:%lf", &x, &y) == 2 && x > 0 && y > 0) a = x / y;
    else a = display_aspect(s);
    return clamp_aspect(a);
}

/* The fixed mode's shape for this run, when the player has put the window back
 * in the game's shape (the cancel button) after choosing another one: the
 * choice is kept for the next start. 0 = none. */
static double g_fixed_lock;

/* The adapt mode's resize: kept to the shapes the game fills. A drag is
 * measured from where it began, and whether it moves the width (a side edge),
 * the height (the top or bottom edge) or both (a corner) is decided once:
 * measuring from the previous step and picking a side each step made the window
 * jump back and forth at the limit. An edge stops at the limit (like a minimum
 * size); a corner slides along the limit's shape. */
static NSSize g_drag_from;        /* the content size when the drag began (0 = none) */
static int g_drag_axes;           /* 0 = not known yet, 1 = width, 2 = height, 3 = both */

static NSSize adapt_clamp(NSSize c, NSSize was)
{
    if (c.width <= 0 || c.height <= 0) return c;
    NSSize from = g_drag_from.width > 0 ? g_drag_from : was;
    bool dw = fabs(c.width - from.width) >= 0.5, dh = fabs(c.height - from.height) >= 0.5;
    if (g_drag_from.width > 0 && !g_drag_axes && (dw || dh)) g_drag_axes = dw && dh ? 3 : dw ? 1 : 2;
    double a = c.width / c.height;
    if (a >= aspect_min() && a <= aspect_max()) return c;
    double want = clamp_aspect(a);
    switch (g_drag_axes ? g_drag_axes : dw && dh ? 3 : dw ? 1 : 2) {
    case 1: c.width = round(c.height * want); break;
    case 2: c.height = round(c.width / want); break;
    default: {
        /* the point of the limit's shape between the two sizes the corner asks for */
        double w = (c.width + c.height * want) / 2;
        c.width = round(w);
        c.height = round(w / want);
    }
    }
    return c;
}

/* The window in a shape: its width kept where the display allows, its top left
 * corner kept, and all of it on its display. */
static void window_to_aspect(NSWindow *w, double a)
{
    if (!w || a <= 0 || (w.styleMask & NSWindowStyleMaskFullScreen)) return;
    NSRect fr = w.frame;
    NSSize c = [w contentRectForFrameRect:fr].size;
    NSRect vis = (w.screen ?: NSScreen.mainScreen).visibleFrame;
    double chrome = fr.size.height - c.height;
    NSSize n = NSMakeSize(c.width, round(c.width / a));
    if (n.height + chrome > vis.size.height) { n.height = vis.size.height - chrome; n.width = round(n.height * a); }
    if (n.width > vis.size.width) { n.width = vis.size.width; n.height = round(n.width / a); }
    if (fabs(n.width - c.width) < 1 && fabs(n.height - c.height) < 1) return;
    NSRect nf = [w frameRectForContentRect:NSMakeRect(0, 0, n.width, n.height)];
    nf.origin.x = fr.origin.x;
    nf.origin.y = NSMaxY(fr) - nf.size.height;
    if (nf.origin.y < vis.origin.y) nf.origin.y = vis.origin.y;
    if (NSMaxX(nf) > NSMaxX(vis)) nf.origin.x = NSMaxX(vis) - nf.size.width;
    if (nf.origin.x < vis.origin.x) nf.origin.x = vis.origin.x;
    tl_log_line("macos: the window goes from %.0fx%.0f to %.0fx%.0f points (%.3f:1)",
                c.width, c.height, n.width, n.height, a);
    [w setFrame:nf display:YES];
}

/* The window's rules for the mode: the fixed mode locks the window's shape (a
 * resize only scales it); the adapt mode keeps it between the shapes the game
 * fills. */
static void apply_window_rules(void)
{
    NSWindow *w = g_window;
    if (!w) return;
    if (adapt_mode()) {
        w.contentResizeIncrements = NSMakeSize(1, 1);        /* no shape lock */
        w.contentMinSize = NSMakeSize(200, 150);
        NSSize c = [w contentRectForFrameRect:w.frame].size;
        if (c.height > 0) window_to_aspect(w, clamp_aspect(c.width / c.height));
    } else {
        double a = g_fixed_lock > 0 ? g_fixed_lock : fixed_aspect(w.screen);
        window_to_aspect(w, a);
        w.contentAspectRatio = NSMakeSize(round(a * 1000), 1000);
        w.contentMinSize = NSMakeSize(480, round(480 / a));
    }
}

/* The mode or the fixed shape changed (the settings window, or a test's
 * "mode:" token): the window takes the new rules at once; the game keeps the
 * shape it started in until it is restarted (the buttons say so). */
static void resize_mode_changed(void)
{
    tl_log_line("macos: the resize mode is now %s", adapt_mode() ? "adapt" : "fixed");
    dispatch_async(dispatch_get_main_queue(), ^{
        g_fixed_lock = 0;
        apply_window_rules();
        [g_view applyPresentation];
    });
}

void tl_log_line(const char *fmt, ...)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static struct timespec start;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&lock);
    if (!start.tv_sec) start = now;
    fprintf(stderr, "[%7.3f] ", (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr); fflush(stderr);
    pthread_mutex_unlock(&lock);
}

static void on_crash(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    char thread[64] = ""; pthread_getname_np(pthread_self(), thread, sizeof(thread));
    fprintf(stderr, "\nCRASH: signal=%d fault=%p thread=%s pc=%#llx lr=%#llx\n",
            sig, info->si_addr, thread, uc->uc_mcontext->__ss.__pc, uc->uc_mcontext->__ss.__lr);
    fflush(stderr);
    _exit(128 + sig);
}

/* ------------------------------------------------------------------ the paths */

/* Everything the app needs is in the bundle; the environment only overrides it, so
 * that a run from a terminal can point at another copy without a rebuild. The paths
 * are relative to the bundle's Contents directory, which is where a macOS bundle puts
 * them. */
static NSString *bundle_dir(NSString *relative)
{
    return [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:
            [@"Contents" stringByAppendingPathComponent:relative]];
}

static NSString *support_dir(void)
{
    NSArray *dirs = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,
                                                        NSUserDomainMask, YES);
    NSString *base = dirs.firstObject;
    /* PVZ2DataFolder: a test build's own folder (an update test's relaunched app has
     * no environment to point it elsewhere) */
    NSString *name = NSBundle.mainBundle.infoDictionary[@"PVZ2DataFolder"];
    return [base stringByAppendingPathComponent:name.length ? name : @"PvZ2 Reflourished"];
}

static void mkdirs(NSString *path)
{
    [[NSFileManager defaultManager] createDirectoryAtPath:path
                              withIntermediateDirectories:YES attributes:nil error:nil];
}

/* Stage the bundled fallback without replacing a newer downloaded archive. */
static NSString *stage_cdn_tar(void)
{
    if (getenv("TL_PVZ2_MAIN_TAR")) return @(getenv("TL_PVZ2_MAIN_TAR"));
    NSString *local = [@(g_data) stringByAppendingPathComponent:@"No_Backup/1.4.2.main.tar"];
    tl_cdn_stage(bundle_dir(@"Game/1.4.2.main.tar").UTF8String, local.UTF8String);
    setenv("TL_PVZ2_MAIN_TAR", local.UTF8String, 1);
    return local;
}

/* ------------------------------------------------------------ the game's data (OBB)
 *
 * A slim app carries no OBB (1.3 GB of the game's data): it downloads it on the
 * first launch, the way the mod's own Android helper does, from the links in the
 * app's Info.plist (PVZ2ObbURLs, written by the build), and checks the size and the
 * sha256 the build wrote next to them before the game ever sees it. The same thing
 * the iPad's slim app does (pvz2-ios.m), in a window of its own.
 *
 * It goes where the game looks first: the data folder's
 * sdcard/Android/obb/com.ea.game.pvz2_rfl/ (the bundle's Game/obb is the fallback,
 * and a full app that carries the OBB never downloads). An OBB there whose size is
 * not the one the app names is another version's: it is downloaded again. A test
 * can point the links elsewhere: TL_PVZ2_OBB_URLS (space-separated). */
static bool background_launch(void);
static unsigned short test_key_code(NSString *s);

static NSDictionary *obb_info(void)
{
    NSDictionary *plist = NSBundle.mainBundle.infoDictionary;
    NSArray *urls = plist[@"PVZ2ObbURLs"];
    const char *env = getenv("TL_PVZ2_OBB_URLS");
    if (env && *env)
        urls = [[@(env) componentsSeparatedByString:@" "]
                   filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"length > 0"]];
    NSString *sha = plist[@"PVZ2ObbSHA256"];
    NSNumber *size = plist[@"PVZ2ObbSize"];
    NSString *name = plist[@"PVZ2ObbName"] ?: @"main.675.com.ea.game.pvz2_rfl.obb";
    if (![urls isKindOfClass:NSArray.class] || !urls.count || !sha.length || size.longLongValue <= 0) return nil;
    return @{ @"urls": urls, @"sha": sha.lowercaseString, @"size": size, @"name": name };
}
static NSString *obb_dir(void)
{
    return [@(g_data) stringByAppendingPathComponent:@"sdcard/Android/obb/com.ea.game.pvz2_rfl"];
}
static bool obb_in_bundle(void)
{
    NSString *dir = bundle_dir(@"Game/obb");
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil])
        if ([name.pathExtension.lowercaseString isEqualToString:@"obb"]) return true;
    return false;
}
static bool obb_download_needed(void)
{
    if (obb_in_bundle()) return false;
    NSDictionary *info = obb_info();
    if (!info) return false;                       /* not a slim app: nothing to download from */
    NSString *path = [obb_dir() stringByAppendingPathComponent:info[@"name"]];
    NSDictionary *attrs = [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
    if (!attrs) return true;
    long long have = [attrs[NSFileSize] longLongValue], want = [info[@"size"] longLongValue];
    if (have == want) return false;
    tl_log_line("macos: the OBB in the data folder is %lld bytes and this app wants %lld: it is downloaded again",
                have, want);
    return true;
}

/* A new game version's OBB has a new name, and the old one is 1.3 GB that nothing
 * reads any more: once the one this app wants is in place, the others go. */
static void drop_other_obbs(NSString *keep)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *name in [fm contentsOfDirectoryAtPath:obb_dir() error:nil]) {
        if ([name isEqualToString:keep] || ![name.pathExtension.lowercaseString isEqualToString:@"obb"]) continue;
        if ([fm removeItemAtPath:[obb_dir() stringByAppendingPathComponent:name] error:nil])
            tl_log_line("macos: removed another version's OBB: %s", name.UTF8String);
    }
}

/* A full app carries the OBB in its bundle. Its first launch clones it into the data
 * folder, where a slim app keeps its own: on APFS a clone is instant and takes no
 * space of its own, and it outlives the bundle -- so an update, which is always the
 * slim app, keeps the game's data. Where no clone can be made (the app is on another
 * volume, or still in its disk image), the game reads the bundle's copy as before. */
static void adopt_bundled_obb(void)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *dir = bundle_dir(@"Game/obb");
    for (NSString *name in [fm contentsOfDirectoryAtPath:dir error:nil]) {
        if (![name.pathExtension.lowercaseString isEqualToString:@"obb"]) continue;
        NSString *from = [dir stringByAppendingPathComponent:name];
        NSString *to = [obb_dir() stringByAppendingPathComponent:name];
        long long size = [[fm attributesOfItemAtPath:from error:nil][NSFileSize] longLongValue];
        NSDictionary *have = [fm attributesOfItemAtPath:to error:nil];
        if (!have || [have[NSFileSize] longLongValue] != size) {
            mkdirs(obb_dir());
            NSString *part = [to stringByAppendingString:@".part"];
            unlink(part.fileSystemRepresentation);
            if (clonefile(from.fileSystemRepresentation, part.fileSystemRepresentation, 0) != 0) {
                tl_log_line("macos: the bundled OBB cannot be cloned into the data folder (%s): "
                            "the game reads it from the bundle", strerror(errno));
                return;
            }
            if (rename(part.fileSystemRepresentation, to.fileSystemRepresentation) != 0) {
                tl_log_line("macos: the cloned OBB cannot be put in place (%s)", strerror(errno));
                unlink(part.fileSystemRepresentation);
                return;
            }
            [[NSURL fileURLWithPath:to] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
            tl_log_line("macos: cloned the bundled OBB into the data folder (%s)", name.UTF8String);
        }
        drop_other_obbs(name);
        return;
    }
}

@interface PVZ2ObbDownload : NSObject <NSURLSessionDownloadDelegate, NSWindowDelegate>
- (instancetype)initWithDone:(void (^)(void))done;
- (void)start;
@end

@implementation PVZ2ObbDownload {
    NSString *_dir, *_name, *_sha;
    long long _size;
    NSArray<NSString *> *_urls;
    NSUInteger _urlIndex;
    void (^_done)(void);
    NSURLSession *_session;
    NSData *_resume;
    NSWindow *_window;
    NSTextField *_title, *_detail;
    NSProgressIndicator *_bar;
    NSButton *_retry;
    id _activity;
}

- (instancetype)initWithDone:(void (^)(void))done
{
    if (!(self = [super init])) return nil;
    _done = [done copy];
    return self;
}

- (void)start
{
    NSDictionary *info = obb_info();
    _urls = info[@"urls"]; _sha = info[@"sha"]; _size = [info[@"size"] longLongValue]; _name = info[@"name"];
    _dir = obb_dir();
    [self buildWindow];
    [self begin];
}

- (void)buildWindow
{
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 460, 170)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered defer:NO];
    _window.title = @"PvZ2 Reflourished";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;
    _title = [NSTextField labelWithString:@"Downloading the game's data"];
    _title.font = [NSFont boldSystemFontOfSize:15];
    _bar = [NSProgressIndicator new];
    _bar.style = NSProgressIndicatorStyleBar;
    _bar.indeterminate = NO;
    _bar.minValue = 0; _bar.maxValue = 1;
    _detail = [NSTextField wrappingLabelWithString:@"The first start downloads the game's data (1.3 GB) once."];
    _detail.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
    _detail.textColor = NSColor.secondaryLabelColor;
    _retry = [NSButton buttonWithTitle:@"Try Again" target:self action:@selector(retryPressed:)];
    _retry.keyEquivalent = @"\r";
    _retry.hidden = YES;
    NSButton *quit = [NSButton buttonWithTitle:@"Quit" target:NSApp action:@selector(terminate:)];
    NSView *spacer = [NSView new];
    [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *buttons = [NSStackView stackViewWithViews:@[ spacer, quit, _retry ]];
    buttons.spacing = 8;
    NSStackView *stack = [NSStackView stackViewWithViews:@[ _title, _bar, _detail, buttons ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 12;
    stack.edgeInsets = NSEdgeInsetsMake(20, 20, 20, 20);
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [_window.contentView addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:_window.contentView.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:_window.contentView.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:_window.contentView.topAnchor],
        [_bar.widthAnchor constraintEqualToAnchor:stack.widthAnchor constant:-40],
        [_detail.widthAnchor constraintEqualToAnchor:stack.widthAnchor constant:-40],
        [buttons.widthAnchor constraintEqualToAnchor:stack.widthAnchor constant:-40],
    ]];
    [_window center];
    if (background_launch()) [_window orderFrontRegardless];
    else { [_window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES]; }
}

/* Closing the window is giving up: the game cannot start without its data. */
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    (void)sender;
    [NSApp terminate:nil];
    return NO;
}

- (void)say:(NSString *)detail { _detail.stringValue = detail; }

- (BOOL)enoughSpace
{
    [NSFileManager.defaultManager createDirectoryAtPath:_dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSURL *url = [NSURL fileURLWithPath:_dir];
    NSNumber *free = nil;
    [url getResourceValue:&free forKey:NSURLVolumeAvailableCapacityForImportantUsageKey error:nil];
    long long need = _size + (256ll << 20);
    if (!free || free.longLongValue >= need) return YES;
    /* An OBB of another version is on its way out anyway: it goes first when both
     * do not fit. */
    NSString *old = [_dir stringByAppendingPathComponent:_name];
    if ([NSFileManager.defaultManager fileExistsAtPath:old]) {
        [NSFileManager.defaultManager removeItemAtPath:old error:nil];
        tl_log_line("macos: the old OBB was removed to make room for the new one");
        [url removeAllCachedResourceValues];
        [url getResourceValue:&free forKey:NSURLVolumeAvailableCapacityForImportantUsageKey error:nil];
        if (!free || free.longLongValue >= need) return YES;
    }
    _title.stringValue = @"Not enough free space";
    [self say:[NSString stringWithFormat:@"The game's data needs %.1f GB free on this Mac (%.1f GB is free now). "
                                          "Free some space, then try again.",
                                         need / 1e9, free.longLongValue / 1e9]];
    _retry.hidden = NO;
    tl_log_line("macos: not enough space for the OBB: %lld bytes free, %lld needed", free.longLongValue, need);
    return NO;
}

- (void)begin
{
    _retry.hidden = YES;
    _title.stringValue = @"Downloading the game's data";
    if (![self enoughSpace]) return;
    /* The Mac must not sleep in the middle of 1.3 GB. */
    if (!_activity)
        _activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiated | NSActivityIdleSystemSleepDisabled
                                                                 reason:@"Downloading the game's data"];
    if (!_session)
        _session = [NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.defaultSessionConfiguration
                                                 delegate:self delegateQueue:NSOperationQueue.mainQueue];
    NSURLSessionDownloadTask *task;
    if (_resume) {
        task = [_session downloadTaskWithResumeData:_resume];
        tl_log_line("macos: the OBB download resumes");
    } else {
        NSURL *url = [NSURL URLWithString:_urls[_urlIndex]];
        task = [_session downloadTaskWithURL:url];
        tl_log_line("macos: the OBB is downloaded from %s", url.absoluteString.UTF8String);
    }
    _resume = nil;
    [self say:@"Starting…"];
    [task resume];
}

- (void)retryPressed:(id)sender { (void)sender; [self begin]; }

- (void)URLSession:(NSURLSession *)session downloadTask:(NSURLSessionDownloadTask *)task
      didWriteData:(int64_t)bytes totalBytesWritten:(int64_t)written totalBytesExpectedToWrite:(int64_t)expected
{
    (void)session; (void)task; (void)bytes; (void)expected;
    _bar.doubleValue = (double)written / (double)_size;
    [self say:[NSString stringWithFormat:@"%.0f of %.0f MB", written / 1e6, _size / 1e6]];
}

- (void)URLSession:(NSURLSession *)session downloadTask:(NSURLSessionDownloadTask *)task
    didFinishDownloadingToURL:(NSURL *)location
{
    (void)session;
    NSInteger status = [task.response isKindOfClass:NSHTTPURLResponse.class]
                     ? ((NSHTTPURLResponse *)task.response).statusCode : 200;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *part = [_dir stringByAppendingPathComponent:[_name stringByAppendingString:@".part"]];
    [fm createDirectoryAtPath:_dir withIntermediateDirectories:YES attributes:nil error:nil];
    [fm removeItemAtPath:part error:nil];
    /* The system deletes the file when this returns: it is moved now. */
    NSError *err = nil;
    if (status < 200 || status >= 300 || ![fm moveItemAtURL:location toURL:[NSURL fileURLWithPath:part] error:&err]) {
        tl_log_line("macos: the OBB download failed (HTTP %ld%s%s)", (long)status, err ? ", " : "",
                    err ? err.localizedDescription.UTF8String : "");
        [self failed:[NSString stringWithFormat:@"The server answered %ld.", (long)status] nextLink:YES];
        return;
    }
    _title.stringValue = @"Checking the game's data";
    _bar.indeterminate = YES;
    [_bar startAnimation:nil];
    [self say:@"This takes a few seconds."];
    NSString *want = _sha, *name = _name, *dir = _dir;
    long long size = _size;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *got = nil;
        long long have = 0;
        int fd = open(part.UTF8String, O_RDONLY);
        if (fd >= 0) {
            CC_SHA256_CTX ctx; CC_SHA256_Init(&ctx);
            size_t cap = 8u << 20;
            uint8_t *buf = malloc(cap);
            ssize_t n;
            while (buf && (n = read(fd, buf, cap)) > 0) { CC_SHA256_Update(&ctx, buf, (CC_LONG)n); have += n; }
            free(buf);
            close(fd);
            unsigned char md[CC_SHA256_DIGEST_LENGTH];
            CC_SHA256_Final(md, &ctx);
            NSMutableString *hex = [NSMutableString string];
            for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [hex appendFormat:@"%02x", md[i]];
            got = hex;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [self->_bar stopAnimation:nil];
            self->_bar.indeterminate = NO;
            if (have != size || ![got isEqualToString:want]) {
                tl_log_line("macos: the downloaded OBB is wrong (%lld bytes, sha256 %s); it is deleted",
                            have, got.UTF8String ?: "?");
                [NSFileManager.defaultManager removeItemAtPath:part error:nil];
                [self failed:@"The download was damaged." nextLink:YES];
                return;
            }
            NSString *final = [dir stringByAppendingPathComponent:name];
            [NSFileManager.defaultManager removeItemAtPath:final error:nil];
            NSError *e = nil;
            if (![NSFileManager.defaultManager moveItemAtPath:part toPath:final error:&e]) {
                tl_log_line("macos: the OBB could not be put in place: %s", e.localizedDescription.UTF8String);
                [self failed:@"The game's data could not be saved." nextLink:NO];
                return;
            }
            /* 1.3 GB that can be downloaded again does not belong in a backup. */
            [[NSURL fileURLWithPath:final] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
            tl_log_line("macos: the OBB is downloaded and checked (%lld bytes)", have);
            drop_other_obbs(name);
            [self finish];
        });
    });
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error
{
    (void)session; (void)task;
    if (!error) return;
    _resume = error.userInfo[NSURLSessionDownloadTaskResumeData];
    tl_log_line("macos: the OBB download stopped: %s%s", error.localizedDescription.UTF8String,
                _resume ? " (it can resume)" : "");
    [self failed:error.localizedDescription nextLink:(_resume == nil)];
}

- (void)failed:(NSString *)why nextLink:(BOOL)next
{
    if (next && _urls.count > 1) _urlIndex = (_urlIndex + 1) % _urls.count;
    if (_activity) { [NSProcessInfo.processInfo endActivity:_activity]; _activity = nil; }
    _title.stringValue = @"The download stopped";
    [self say:[NSString stringWithFormat:@"%@ Check the connection, then try again.", why]];
    _retry.hidden = NO;
}

- (void)finish
{
    if (_activity) { [NSProcessInfo.processInfo endActivity:_activity]; _activity = nil; }
    [_session finishTasksAndInvalidate];
    _session = nil;
    _window.delegate = nil;
    [_window orderOut:nil];
    if (_done) _done();
}
@end

static PVZ2ObbDownload *g_obb_download;

/* The guest's libraries, converted ahead of time. The loader takes the directory and
 * finds libPVZ2.dylib, libRFLib.dylib, libc++_shared.dylib and libNimble.dylib there,
 * with the .facts file beside each: that file is the symbol map, and without it the
 * driver cannot ask for a name. */
static NSString *stage_aot_dylibs(void)
{
    NSString *fw = bundle_dir(@"Frameworks");
    if (![NSFileManager.defaultManager fileExistsAtPath:
            [fw stringByAppendingPathComponent:@"libPVZ2.dylib"]]) return nil;
    setenv("TL_AOT_DIR", fw.UTF8String, 1);
    return fw;
}

/* ------------------------------------------------------------- one at a time
 *
 * The game writes its save files without locking them, so two instances sharing
 * one data directory can corrupt each other's saves -- and the player is left
 * with two windows and no idea which one is the game. The claim is an flock on
 * a file in the data directory: whoever holds it is the app, and the next one
 * brings that one to the front and leaves.
 *
 * The lock is per data directory, not per machine: two runs against two
 * directories are two games, and the tests need that.
 */
static int g_instance_lock = -1;

static bool claim_instance(NSString *dataPath)
{
    NSString *lockPath = [dataPath stringByAppendingPathComponent:@"instance.lock"];
    int fd = open(lockPath.fileSystemRepresentation, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return false; }
    g_instance_lock = fd;              /* held for the life of the process */
    return true;
}

/* The other copy, if there is one: the player gets the window they already
 * have instead of a second one. */
static void activate_other_instance(void)
{
    NSString *bundleID = NSBundle.mainBundle.bundleIdentifier;
    if (!bundleID) return;
    for (NSRunningApplication *app in [NSRunningApplication runningApplicationsWithBundleIdentifier:bundleID]) {
        if (app.processIdentifier == NSProcessInfo.processInfo.processIdentifier) continue;
        [app activateWithOptions:NSApplicationActivateIgnoringOtherApps];
        break;
    }
}

/* ---------------------------------------------------------------- the game run */

static void *launch_game(void *unused)
{
    (void)unused;
    @autoreleasepool {
        pthread_setname_np("PVZ2 launch");
        if (!getenv("TL_AUDIO") || atoi(getenv("TL_AUDIO"))) tl_audio_install();
        bool ok = tl_pvz2_start(&config) && tl_pvz2_run();
        atomic_store(&ready, ok);
        tl_log_line("macos: driver startup %s", ok ? "complete" : "FAILED");
        if (!ok) dispatch_async(dispatch_get_main_queue(), ^{
            NSAlert *alert = [NSAlert new];
            alert.messageText = @"Reflourished could not start";
            alert.informativeText = @"See the runtime log (pvz2.log, in Application Support) for details.";
            [alert runModal];
        });
    }
    return NULL;
}

/* The app is not on the screen: the game pauses, the way it does on Android
 * when the activity is stopped. The guest is told (it saves on that event,
 * and it stops its own work), and the drawing stops with it -- a game that
 * nobody can see has no business burning the battery.
 *
 * This is not the same as the app not being the frontmost one: a covered
 * window keeps running (the app's own background mode says so out loud).
 *
 * There is more than one reason for the app to be away -- it can be hidden,
 * its window can be minimized, and the display can be asleep -- and each of
 * them is counted, so that one of them coming back does not bring the game
 * back while another still holds it. */
static atomic_int g_away_hidden, g_away_minimized, g_away_asleep, g_away_closed;

static int away_count(void)
{
    return atomic_load(&g_away_hidden) + atomic_load(&g_away_minimized) +
           atomic_load(&g_away_asleep) + atomic_load(&g_away_closed);
}

static void app_suspended(const char *why)
{
    tl_log_line("macos: %s -- the game pauses", why);
    /* The guest is told once, when the first reason comes: a second reason
     * (a minimized window while the app is hidden) is not a second "going
     * away". */
    tl_pvz2_lifecycle_background();
}
static void app_resumed(const char *why)
{
    /* The count has already gone down (the reason is gone). The guest is told
     * it is back only when nothing holds the game away any more -- otherwise
     * one reason coming back would bring the game back while another still
     * holds it. */
    if (away_count() > 0) {
        tl_log_line("macos: %s -- but the game stays away (%d reasons left)", why, away_count());
        return;
    }
    tl_log_line("macos: nothing holds the game away any more -- the game goes on");
    tl_pvz2_lifecycle_foreground();
}

/* -------------------------------------------------------------- scripted input
 *
 * The same tap language the off-screen harness speaks, so a run of this app can be
 * scripted the same way a run of the harness can -- which is what the self-test does:
 * TL_PVZ2_TAP="x,y;wait:3;drag:x1,y1,x2,y2;key:66;quit;..." with the coordinates in
 * the game's own 1748x804 space.
 *
 * "key:66" is Android's keycode (66 is ENTER, 67 is DEL, 4 is BACK); "quit" closes
 * the app the way the player does, so the guest is told the app is going away and
 * its save is written.
 */
static void do_drag(const char *s)
{
    int x1, y1, x2, y2;
    if (!s || sscanf(s, "%d,%d,%d,%d", &x1, &y1, &x2, &y2) != 4) return;
    tl_log_line("macos: drag (%d,%d) -> (%d,%d)", x1, y1, x2, y2);
    tl_pvz2_touch(0, 1, (float)x1, (float)y1);
    for (int i = 1; i <= 10; i++) {
        usleep(20000);
        tl_pvz2_touch(1, 1, (float)(x1 + (x2 - x1) * i / 10), (float)(y1 + (y2 - y1) * i / 10));
    }
    usleep(20000);
    tl_pvz2_touch(3, 1, (float)x2, (float)y2);
}

static void *tapper(void *arg)
{
    (void)arg;
    int delay = getenv("TL_PVZ2_TAP_DELAY") ? atoi(getenv("TL_PVZ2_TAP_DELAY")) : 3;
    sleep((unsigned)delay);
    const char *spec = getenv("TL_PVZ2_TAP");
    char *copy = spec ? strdup(spec) : NULL;
    char *save = NULL;
    for (char *tok = copy ? strtok_r(copy, ";", &save) : NULL; tok; tok = strtok_r(NULL, ";", &save)) {
        int wait;
        if (sscanf(tok, "wait:%d", &wait) == 1) {
            if (wait >= 0 && wait <= 600) sleep((unsigned)wait);
            continue;
        }
        if (!strncmp(tok, "drag:", 5)) { do_drag(tok + 5); continue; }
        if (!strncmp(tok, "key:", 4)) {
            int code = atoi(tok + 4);
            tl_log_line("macos: key %d", code);
            tl_pvz2_key(code, 1);
            usleep(120000);
            tl_pvz2_key(code, 0);
            continue;
        }
        if (!strncmp(tok, "press:", 6)) {
            /* the player's own keys, in step with the taps (the keystroke
             * test's keys run on their own clock) */
            for (const char *k = tok + 6; *k; k++) {
                NSString *s = [NSString stringWithFormat:@"%c", *k];
                tl_log_line("macos: the script presses \"%s\"", s.UTF8String);
                dispatch_semaphore_t done = dispatch_semaphore_create(0);
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (g_view) {
                        NSEvent *e = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                                                 modifierFlags:0 timestamp:0 windowNumber:0 context:nil
                                                    characters:s charactersIgnoringModifiers:s isARepeat:NO
                                                       keyCode:test_key_code(s)];
                        [g_view keyDown:e];
                    }
                    dispatch_semaphore_signal(done);
                });
                dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
                usleep(200000);
            }
            continue;
        }
        if (!strcmp(tok, "settings")) {
            /* the settings window, as Cmd-, opens it (for a test's picture) */
            tl_log_line("macos: the script opens the settings");
            dispatch_async(dispatch_get_main_queue(), ^{
                [NSApp sendAction:@selector(showPreferences:) to:nil from:nil];
            });
            sleep(2);
            continue;
        }
        if (!strncmp(tok, "settings:", 9)) {
            /* one pane of the settings window (general, keyboard, about) */
            NSString *tab = @(tok + 9);
            tl_log_line("macos: the script opens the settings at %s", tok + 9);
            dispatch_async(dispatch_get_main_queue(), ^{
                Class settings = NSClassFromString(@"PVZ2Settings");
                if ([settings respondsToSelector:@selector(showTab:)])
                    [settings performSelector:@selector(showTab:) withObject:tab];
            });
            sleep(2);
            continue;
        }
        int screen_no;
        if (sscanf(tok, "screen:%d", &screen_no) == 1) {
            /* The window onto another display (its top left corner, same size). */
            dispatch_async(dispatch_get_main_queue(), ^{
                NSArray<NSScreen *> *all = NSScreen.screens;
                if (!g_window || screen_no < 0 || screen_no >= (int)all.count) return;
                NSRect v = all[screen_no].visibleFrame;
                NSRect f = g_window.frame;
                [g_window setFrame:NSMakeRect(v.origin.x + 40, NSMaxY(v) - f.size.height - 40, f.size.width, f.size.height) display:YES];
                tl_log_line("macos: the tap script moved the window to display %d (scale %.0f)", screen_no, all[screen_no].backingScaleFactor);
            });
            sleep(2);
            continue;
        }
        if (!strcmp(tok, "escape")) {
            tl_log_line("macos: the script presses Escape");
            tl_pvz2_request_escape();
            continue;
        }
        if (!strcmp(tok, "quit")) {
            tl_log_line("macos: the script asked the app to quit");
            dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
            return NULL;
        }
        if (!strcmp(tok, "text")) {
            const char *t = getenv("TL_PVZ2_TEXT");
            if (t) { tl_log_line("macos: text \"%s\"", t); tl_pvz2_text(t); }
            sleep(1);
            continue;
        }
        if (!strncmp(tok, "size:", 5)) {
            /* The size of the window's content, in the game's own pixels: the
             * app resizes its own window (the system's way of doing it needs
             * the accessibility API, which is not always there). */
            int w = 0, h = 0;
            if (sscanf(tok + 5, "%dx%d", &w, &h) != 2) continue;
            tl_log_line("macos: the tap script sets the window to %dx%d", w, h);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!g_window) return;
                double scale = g_view ? g_view.metal.contentsScale : 1.0;
                [g_window setContentSize:NSMakeSize(w / scale, h / scale)];
                /* As a person's resize would be: kept to the shapes the game can take. */
                apply_window_rules();
            });
            sleep(4);
            continue;
        }
        if (!strncmp(tok, "aspect:", 7)) {
            /* The fixed mode's shape, chosen as the settings window does. */
            [[NSUserDefaults standardUserDefaults] setObject:@(tok + 7) forKey:kFixedAspect];
            tl_log_line("macos: the tap script sets the fixed shape to \"%s\"", tok + 7);
            sleep(3);
            continue;
        }
        if (!strcmp(tok, "close") || !strcmp(tok, "reopen")) {
            /* The window's red button, or the Dock icon clicked, by a test. */
            bool close = !strcmp(tok, "close");
            tl_log_line("macos: the tap script %s the window", close ? "closes" : "reopens");
            dispatch_async(dispatch_get_main_queue(), ^{
                if (close) [g_window performClose:nil];
                else [(id)NSApp.delegate applicationShouldHandleReopen:NSApp hasVisibleWindows:NO];
            });
            sleep(2);
            continue;
        }
        if (!strcmp(tok, "keep")) {
            /* The cancel button, pressed by a test. */
            tl_log_line("macos: the tap script presses the cancel button");
            dispatch_async(dispatch_get_main_queue(), ^{
                [(id)NSApp.delegate performSelector:@selector(keepGameShape:) withObject:nil];
            });
            sleep(2);
            continue;
        }
        if (!strcmp(tok, "fit")) {
            /* The fit button, pressed by a test. */
            tl_log_line("macos: the tap script presses the fit button");
            dispatch_async(dispatch_get_main_queue(), ^{
                [(id)NSApp.delegate performSelector:@selector(restartToFit:) withObject:nil];
            });
            sleep(2);
            continue;
        }
        if (!strncmp(tok, "mode:", 5)) {
            /* The resize mode, switched while the game runs: the same thing the
             * preferences' row does, so it is saved in the app's settings
             * (a launch argument still wins over it on the next start). */
            NSString *mode = [NSString stringWithUTF8String:tok + 5];
            [[NSUserDefaults standardUserDefaults] setObject:mode forKey:kResizeMode];
            tl_log_line("macos: the tap script sets the resize mode to \"%s\"", tok + 5);
            resize_mode_changed();
            sleep(3);
            continue;
        }
        if (!strncmp(tok, "wp:", 3)) {
            /* A tap at a percentage of the window: the shape of the window is
             * not known to the script that writes it (the display's scale is
             * not), so the point is given as a share of the window.
             *
             * The window's geometry and the tap itself are the main thread's
             * business: AppKit's view state is main-thread-only, and this
             * thread is not it. */
            double px, py;
            if (sscanf(tok + 3, "%lf,%lf", &px, &py) != 2) continue;
            dispatch_async(dispatch_get_main_queue(), ^{
                NSSize sz = g_window ? g_window.contentView.bounds.size : NSZeroSize;
                if (sz.width <= 0 || sz.height <= 0) return;
                NSPoint p = NSMakePoint(px / 100.0 * sz.width, py / 100.0 * sz.height);
                tl_log_line("macos: window tap at (%.0f,%.0f) -- %.2f%%,%.2f%% of the window",
                            p.x, p.y, px, py);
                if (!g_view) return;
                [g_view windowTap:0 at:p];
                /* The second phase (the release) goes 120 ms later, from the
                 * main thread's own queue: sleeping here would hold the run
                 * loop up. */
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.12 * NSEC_PER_SEC)),
                               dispatch_get_main_queue(), ^{
                    if (!g_view) return;
                    [g_view windowTap:3 at:p];
                });
            });
            sleep(2);
            continue;
        }
        if (!strncmp(tok, "w:", 2)) {
            /* A tap in the *window's* points, not in the game's: the host maps it
             * through the viewport (or one to one, in the adapt mode), so this is
             * how the mapping is checked. */
            int x, y;
            if (sscanf(tok + 2, "%d,%d", &x, &y) != 2) continue;
            tl_log_line("macos: window tap (%d,%d)", x, y);
            [g_view windowTap:0 at:NSMakePoint(x, y)];
            usleep(120000);
            [g_view windowTap:3 at:NSMakePoint(x, y)];
            sleep(2);
            continue;
        }
        if (!strncmp(tok, "hide:", 5)) {
            /* The app goes away, the way Cmd-H does it. The app does it to
             * itself: a test must not send anything to the system. */
            dispatch_async(dispatch_get_main_queue(), ^{
                tl_log_line("macos: the tap script hides the app");
                [NSApp hide:nil];
            });
            sleep(3);
            continue;
        }
        if (!strncmp(tok, "unhide:", 7)) {
            dispatch_async(dispatch_get_main_queue(), ^{
                tl_log_line("macos: the tap script brings the app back");
                [NSApp unhide:nil];
            });
            sleep(3);
            continue;
        }
        if (!strncmp(tok, "minimize:", 9)) {
            dispatch_async(dispatch_get_main_queue(), ^{
                tl_log_line("macos: the tap script minimizes the window");
                [g_window miniaturize:nil];
            });
            sleep(3);
            continue;
        }
        if (!strncmp(tok, "restore:", 8)) {
            dispatch_async(dispatch_get_main_queue(), ^{
                tl_log_line("macos: the tap script restores the window");
                [g_window deminiaturize:nil];
            });
            sleep(3);
            continue;
        }
        if (!strncmp(tok, "sleep:", 6)) {
            /* The display goes to sleep. The system posts this notification
             * when it does; the app posts it here, because putting the
             * person's display to sleep is not something a test may do. The
             * app's own handler runs -- the same one the system's notification
             * runs. */
            dispatch_async(dispatch_get_main_queue(), ^{
                tl_log_line("macos: the tap script says the display is going to sleep");
                [[NSNotificationCenter defaultCenter]
                    postNotificationName:NSWorkspaceWillSleepNotification object:nil];
            });
            sleep(3);
            continue;
        }
        if (!strncmp(tok, "wake:", 5)) {
            dispatch_async(dispatch_get_main_queue(), ^{
                tl_log_line("macos: the tap script says the display is awake again");
                [[NSNotificationCenter defaultCenter]
                    postNotificationName:NSWorkspaceDidWakeNotification object:nil];
            });
            sleep(3);
            continue;
        }
        if (!strncmp(tok, "cursor:", 7)) {
            /* Where the pointer is, in the game's own 1748x804 space. The
             * person's mouse is never moved: this is how a test says where
             * the cursor is, so the shortcut that plants at the cursor can be
             * proved without touching the person at the keyboard. */
            int x, y;
            if (sscanf(tok + 7, "%d,%d", &x, &y) != 2) continue;
            tl_log_line("macos: the pointer is at (%d,%d) in the game's own space", x, y);
            cursor_set_game(x, y);
            continue;
        }
        int x, y;
        if (sscanf(tok, "%d,%d", &x, &y) != 2) continue;
        tl_log_line("macos: tap (%d,%d)", x, y);
        if (atomic_load(&g_packet_in_hand)) {
            /* A packet is in hand: the tap is where it goes, exactly as the
             * mouse would carry it there and let it go. */
            packet_carry_to(x, y);
        } else {
            tl_pvz2_touch(0, 1, (float)x, (float)y);
            usleep(120000);
            tl_pvz2_touch(3, 1, (float)x, (float)y);
        }
        sleep(2);
    }
    if (getenv("TL_PVZ2_TEXT") && !strstr(spec ? spec : "", "text")) {
        usleep(500000);
        tl_pvz2_text(getenv("TL_PVZ2_TEXT"));
    }
    if (getenv("TL_PVZ2_DRAG")) { usleep(500000); do_drag(getenv("TL_PVZ2_DRAG")); }
    if (getenv("TL_PVZ2_KEY")) {
        int code = atoi(getenv("TL_PVZ2_KEY"));
        usleep(500000);
        tl_log_line("macos: key %d", code);
        tl_pvz2_key(code, 1);
        usleep(120000);
        tl_pvz2_key(code, 0);
    }
    free(copy);
    return NULL;
}

/* ------------------------------------------------------------- the settings
 *
 * A native preferences window (Cmd-,) and the app's own UserDefaults. The game's
 * own UI is not touched: a shortcut is turned into a tap on the seed packet, the
 * same tap the mouse makes, and the guest cannot tell the difference.
 *
 * The packets' positions are *calibrated*, not known: the seed bank's layout
 * depends on the level (how many packets it has, how wide they are), and the
 * measurements are in the preferences, where the player can correct them. The
 * log says where a shortcut tapped, which is how the numbers are checked.
 *
 * The defaults are measured, in the guest's own frame at the game's own
 * 1748x804, on a level with four packets (Tester's House, Day 4):
 *
 *   - the seed bank is a *column* down the left of the screen, not a row: the
 *     cards are stacked one under the other. The first card's panel spans
 *     y 93..166 and x 12..125, its plant art x 30..83 (so the art's centre is
 *     x 56), and every card below it is 79 pixels further down (the panels are
 *     73 tall with a gap of 6);
 *   - the shovel is the round button at the *bottom right*, its circle
 *     x 1626..1696, y 722..790.
 *
 * The old numbers (a row of packets 120 apart at y 161, the shovel at x 640)
 * were never measured: they put the second packet outside the seed bank
 * altogether and the shovel in the middle of the lawn.
 */

static NSString *const kSlotKey[PVZ2_SLOT_COUNT] = {
    @"seedSlot1", @"seedSlot2", @"seedSlot3", @"seedSlot4",
    @"seedSlot5", @"seedSlot6", @"seedSlot7", @"seedSlot8"
};
static NSString *const kShovelKey = @"shovelKey";
static NSString *const kPacketX = @"packetX";         /* the first packet's centre */
static NSString *const kPacketY = @"packetY";
static NSString *const kPacketStep = @"packetStep";   /* how far apart they are */
static NSString *const kShovelX = @"shovelX";
static NSString *const kNativeShovel = @"nativeShovel";

/* The native shovel: the guest's own "the shovel button was pressed"
 * (ShovelUI::OnMouseDown, guest 0x13ab0a0), which the guest itself does when
 * the button is tapped. Off by default -- the ordinary tap is what the game
 * expects -- and the environment wins over the preference, so a test can turn
 * it on for one run.
 *
 *   0  off: the tap at the shovel's place
 *   1  the touch at the button the guest itself reports, and the guest's own
 *      function as the fallback
 *   2  the guest's own function */
static int native_shovel_mode(void)
{
    const char *env = getenv("TL_PVZ2_NATIVE_SHOVEL");
    if (env && *env) return atoi(env);
    /* The guest's own shovel is the default now: it is the game's own button,
     * so it needs no coordinates and it cannot be wrong whatever size the
     * surface is. The preference still turns it off (0) or back to the touch
     * at the button the guest reports (1). */
    if ([[NSUserDefaults standardUserDefaults] boolForKey:kNativeShovel]) return 1;
    return 2;
}

/* The seed shortcut (the number keys): OFF by default.
 *
 * The synthetic touch was measured for a 1748x804 surface, and it is wrong
 * for any other one; and it leaves a finger down in the guest, and a finger
 * that is down makes every later tap do nothing (which is what the player
 * saw: the first plant worked and the clicks after it did not). The game's
 * own "the card was pressed" is the way this is done now; until that is in,
 * the shortcut says so and does nothing. The environment turns the old way on,
 * for the tests that need it. */
static bool seed_shortcut_enabled(void)
{
    const char *env = getenv("TL_PVZ2_SEED_SHORTCUT");
    return env && env[0] == '1';
}

/* The native seed pick: the game's own "this card was pressed". On by
 * default; the environment turns it off (and the old synthetic touch can be
 * turned on with TL_PVZ2_SEED_SHORTCUT=1). */
static int native_seed_pick_mode(void)
{
    const char *env = getenv("TL_PVZ2_SEED_PICK");
    if (env && *env) return atoi(env);
    /* On by default (the player's call, 2026-10-22): proved on a level with
     * every slot full -- the pick, the planting click, a second press putting
     * the packet back, the refusals for a recharging or too dear card -- and
     * played with a keyboard on the iPad. TL_PVZ2_SEED_PICK=0 turns it off
     * (the synthetic tap, and its shapes below, come back). */
    return 1;
}

static NSString *const kShovelY = @"shovelY";

/* The shape of a shortcut: what the key does.
 *
 * "Lift" (the default): the key takes the packet in hand and the player's
 * next click plants it. That is what the game wants -- it plants by a drag,
 * and a tap on a packet does nothing at all -- so the key cannot plant
 * anything by itself.
 *
 * "Plant at cursor": the key takes the packet and drops it on the cell under
 * the mouse pointer, with the drag the game wants. The player only has to
 * point at the spot.
 *
 * The key is stored under its own name; the panel shows what it does. */
static NSString *const kShortcutShape = @"shortcutShape";
static bool plant_at_cursor(void)
{
    NSString *v = [[NSUserDefaults standardUserDefaults] stringForKey:kShortcutShape];
    /* No setting means the default, and the default is Lift. (Messaging nil
     * returns zero, which is NSOrderedSame -- so without this check an app
     * with no preference at all would plant at the cursor.) */
    if (!v.length) return false;
    return [v caseInsensitiveCompare:@"Plant at cursor"] == NSOrderedSame;
}

/* Whether the app takes the focus when it starts.
 *
 * A player wants the game in front of everything. A test on somebody's working
 * Mac does not: the window is shown, but behind whatever the person is doing,
 * and the focus stays with them (TL_KEEP_RUNNING_IN_BACKGROUND=1 -- the same idea
 * as "open -g").
 *
 * The game keeps drawing and presenting either way: the Mac host has never
 * paused the guest when the window lost the focus or was covered, because the
 * guest's own loop is a thread of its own and the picture does not need the
 * window server to be looking at it. That is what this option says out loud:
 * "keep running in the background". */
static bool background_launch(void)
{
    const char *b = getenv("TL_KEEP_RUNNING_IN_BACKGROUND");
    if (!b || !*b || !strcmp(b, "0")) b = getenv("TL_PVZ2_BACKGROUND");
    if (!b || !*b || !strcmp(b, "0")) b = getenv("TL_PVZ2_QUIET");
    return b && *b && strcmp(b, "0") != 0;
}

/* The settings of the builds before the first release, under their old app ID
 * (org.husk.pvz2.macos), copied once into this ID's own: before AppKit or SwiftUI
 * reads a setting. The old domain is only read. Test launches skip it unless
 * TL_PVZ2_IMPORT_FROM names a test domain to import from instead. */
static void import_old_settings(void)
{
    const char *override = getenv("TL_PVZ2_IMPORT_FROM");
    if (background_launch() && (!override || !*override)) return;
    NSString *source = override && *override ? @(override) : @"org.husk.pvz2.macos";
    NSString *own = NSBundle.mainBundle.bundleIdentifier;
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    if ([defaults persistentDomainForName:own][@"importedFrom"]) {
        tl_log_line("macos: settings import skipped: %s already has importedFrom", own.UTF8String);
        return;
    }
    NSDictionary *old = [defaults persistentDomainForName:source];
    for (NSString *key in old) [defaults setObject:old[key] forKey:key];
    [defaults setObject:source forKey:@"importedFrom"];
    tl_log_line("macos: imported %lu settings from %s into %s", (unsigned long)old.count,
                source.UTF8String, own.UTF8String);
}

/* The keys the packets answer to, out of the box: the shared table (the iPad
 * host uses the same one). */
static NSString *default_slot_key(int slot)
{
    return @(pvz2_default_slot_keys[slot]);
}

static double setting_double(NSString *key, double fallback)
{
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    id v = [d objectForKey:key];
    if (![v respondsToSelector:@selector(doubleValue)]) return fallback;
    return [v doubleValue];
}
static NSString *setting_string(NSString *key, NSString *fallback)
{
    NSString *v = [[NSUserDefaults standardUserDefaults] stringForKey:key];
    return v.length ? v : fallback;
}

/* The key a shortcut answers to, or nil when there is none.
 *
 * An *empty* string is a setting of its own: it is what "None" writes, and it
 * means the shortcut is off. A *missing* setting is a different thing: it
 * means the default answers. (The old code could not tell them apart, so
 * "None" in the preferences left the default key working -- the panel said the
 * shortcut was off and the key still took a packet.) */
static NSString *setting_key(NSString *key)
{
    id v = [[NSUserDefaults standardUserDefaults] objectForKey:key];
    if (![v isKindOfClass:NSString.class]) return nil;
    return (NSString *)v;
}

/* What a shortcut's box shows: the key it answers to, or "None". */
static NSString *shown_key(NSString *key, NSString *fallback)
{
    NSString *v = setting_key(key);
    if (!v) return fallback;
    return v.length ? v : @"None";
}

/* A point of the surface's pixels, in the game's own 1748x804 space: the
 * other way round. */
static void surface_to_game(double sx, double sy, double *gx, double *gy)
{
    int sw = atomic_load(&g_surface_w), sh = atomic_load(&g_surface_h);
    int small = sw < sh ? sw : sh;
    if (small <= 0) { *gx = sx; *gy = sy; return; }
    double scale = (double)small / (double)surfaceHeight;
    *gx = sx / scale;
    *gy = sy / scale;
}

/* Where a packet is, in the game's own 1748x804.
 *
 * The seed bank is a column down the left of the screen, so a packet's place
 * is the first card's centre and every card below it is one step further
 * down. (It used to be a row: the packets were 120 apart to the *right* of
 * each other, which is nowhere near the seed bank -- only the first packet
 * was ever hit.) */
static void packet_position(int slot, double *x, double *y)
{
    double px = setting_double(kPacketX, PVZ2_PACKET_X);
    double py = setting_double(kPacketY, PVZ2_PACKET_Y);
    double step = setting_double(kPacketStep, PVZ2_PACKET_STEP);
    *x = px;
    *y = py + step * slot;
}

/* A point of the game's own 1748x804 space, in the surface's pixels.
 *
 * The guest's UI keeps its places in the app's space (the app space is the
 * game's own space times the design's ratio, 1536 over 804), and the app's
 * space is mapped into the surface by the app's size -- the guest turns a
 * touch at the surface's (x,y) into the app's space by (mWidth / surface
 * width) and (mHeight / surface height).
 *
 * So the two factors are:
 *     x: the design's ratio times the surface's width over the app's width,
 *     y: the design's ratio times the surface's height over the app's height.
 *
 * In the letterbox mode the app's size is the guest's own (3339x1536) and
 * the surface is the game's own size, so both factors are 1: a point of the
 * game's own space is a point of the surface. In the adapt mode the host has
 * told the guest a new app's size, and the factors follow it. */
static void game_to_surface(double gx, double gy, double *sx, double *sy)
{
    int sw = atomic_load(&g_surface_w), sh = atomic_load(&g_surface_h);
    int small = sw < sh ? sw : sh;
    if (small <= 0) {
        /* The guest has not been looked at yet: the game's own space is the
         * surface (true in the letterbox mode). */
        *sx = gx; *sy = gy;
        return;
    }
    /* The guest's UI keeps its places in the app's space (the app's space is
     * the game's own space times the design's ratio, 1536 over 804), and the
     * app's space is drawn into the surface scaled by the *smaller* side of
     * the surface over the app's height -- so the content is the surface's
     * width by the smaller side, at the top left (measured: the adapt proof's
     * pictures). The two factors cancel, so a point of the game's own space
     * is at that many pixels times the smaller side over 804 -- the rule the
     * adapt proof was made with, where the pause button was tapped where it
     * is drawn, in six shapes. */
    double scale = (double)small / (double)surfaceHeight;
    *sx = gx * scale;
    *sy = gy * scale;
}

/* The keys the preferences offer. Anything else can be typed into the box. */
static NSArray *key_choices(void)
{
    return @[@"1", @"2", @"3", @"4", @"5", @"6", @"7", @"8", @"9", @"0",
             @"Q", @"W", @"E", @"R", @"T", @"Y", @"U", @"I", @"O", @"P",
             @"A", @"S", @"D", @"F", @"G", @"H", @"J", @"K", @"L",
             @"Z", @"X", @"C", @"V", @"B", @"N", @"M",
             @"F1", @"F2", @"F3", @"F4", @"F5", @"F6", @"F7", @"F8", @"F9", @"F10",
             @"F11", @"F12", @"None"];
}

/* A key's name, the way the settings store it: "Space", "Tab", "Left",
 * "Right", "Up", "Down", "F1".."F12", or the key's own character without
 * Shift, in capitals ("S", "1", ";"). nil for a key that cannot be a shortcut
 * (Return and Escape belong to the game, Delete clears a shortcut, and keys
 * with no character). The settings window's recorder names keys the same way
 * (pvz2-macos-settings.swift, KeyName). */
static NSString *key_name(NSEvent *e)
{
    switch (e.keyCode) {
    case 49: return @"Space";
    case 48: return @"Tab";
    case 123: return @"Left";
    case 124: return @"Right";
    case 125: return @"Down";
    case 126: return @"Up";
    case 122: return @"F1";  case 120: return @"F2";  case 99: return @"F3";   case 118: return @"F4";
    case 96: return @"F5";   case 97: return @"F6";   case 98: return @"F7";   case 100: return @"F8";
    case 101: return @"F9";  case 109: return @"F10"; case 103: return @"F11"; case 111: return @"F12";
    case 36: case 76: case 53: case 51: case 117: return nil;   /* Return, Enter, Escape, Delete */
    default: break;
    }
    /* Without Shift: Shift-1 is still the "1" key. (Only when Shift is down: a
     * test's made-up key event has no real key code to apply anything to.) */
    NSString *c = (e.modifierFlags & NSEventModifierFlagShift) ? [e charactersByApplyingModifiers:0]
                                                               : e.charactersIgnoringModifiers;
    if (c.length != 1) return nil;
    unichar u = [c characterAtIndex:0];
    if (u <= 0x20 || u == 0x7f || (u >= 0xF700 && u <= 0xF8FF)) return nil;
    return c.uppercaseString;
}

/* A test's made-up key press has no key behind it: the keys that are named by
 * their key code (Space) get theirs, the rest are known by their character. */
static unsigned short test_key_code(NSString *s) { return [s isEqualToString:@" "] ? 49 : 0; }

/* Which slot a key means, or -1. The shovel is PVZ2_SLOT_COUNT.
 *
 * A slot whose key is "None" (an empty setting) answers to nothing: it is
 * skipped, so the key is free for another slot. */
static int shortcut_slot(NSString *characters)
{
    if (!characters.length) return -1;
    for (int i = 0; i < PVZ2_SLOT_COUNT; i++) {
        NSString *k = setting_key(kSlotKey[i]);
        if (!k) k = default_slot_key(i);       /* not set: the default answers */
        if (k.length && [k caseInsensitiveCompare:characters] == NSOrderedSame) return i;
    }
    NSString *sh = setting_key(kShovelKey);
    if (!sh) sh = @(PVZ2_DEFAULT_SHOVEL_KEY);
    if (sh.length && [sh caseInsensitiveCompare:characters] == NSOrderedSame) return PVZ2_SLOT_COUNT;
    return -1;
}

/* Is this a key the app can recognise? A single character, or one of the
 * function keys. Anything else ("cmd", "Escape", a word) would never match
 * what the keyboard delivers, so the shortcut would be dead -- and the
 * preferences would not say so. */
static bool usable_key(NSString *value)
{
    if (!value.length) return true;            /* "None": the shortcut is off */
    if (value.length == 1) {
        unichar c = [value characterAtIndex:0];
        return c >= 0x21 && c < 0x7f;          /* printable, no space */
    }
    return [value hasPrefix:@"F"] && value.length <= 3 &&
           [[NSCharacterSet decimalDigitCharacterSet] characterIsMember:
            [value characterAtIndex:value.length - 1]];
}

/* ------------------------------------------------------------------ preferences
 *
 * A native preferences window: the app's own, with the app's own UserDefaults.
 * Cmd-, opens it.
 */
@interface PVZ2Prefs : NSObject
+ (void)show;
@end

@implementation PVZ2Prefs

/* Which setting a control is for: the combos are the keys (the slots and the
 * shovel), the fields are the numbers. The tag says which. */
static NSComboBox *g_combos[PVZ2_SLOT_COUNT + 1];
static NSTextField *g_numbers[5];
static NSComboBox *g_mode_combo;      /* the resize mode's own row */
static NSComboBox *g_shape_combo;     /* the shortcut shape's own row */
static CGFloat g_prefs_height = 660;  /* how tall the panel's content is */
static CGFloat g_prefs_bottom = 0;    /* where the content ends */

+ (NSString *)keyForTag:(NSInteger)tag
{
    if (tag < PVZ2_SLOT_COUNT) return kSlotKey[tag];
    if (tag == PVZ2_SLOT_COUNT) return kShovelKey;
    static NSString *const numbers[5] = { kPacketX, kPacketY, kPacketStep, kShovelX, kShovelY };
    if (tag >= PVZ2_SLOT_COUNT + 1 && tag < PVZ2_SLOT_COUNT + 6) return numbers[tag - PVZ2_SLOT_COUNT - 1];
    if (tag == PVZ2_SLOT_COUNT + 6) return kResizeMode;
    if (tag == PVZ2_SLOT_COUNT + 7) return kShortcutShape;
    return nil;
}

+ (NSTextField *)label:(NSString *)text at:(NSPoint)p
{
    NSTextField *l = [NSTextField labelWithString:text];
    l.frame = NSMakeRect(p.x, p.y, 190, 20);
    l.alignment = NSTextAlignmentRight;
    return l;
}

+ (NSComboBox *)comboFor:(NSString *)key value:(NSString *)value tag:(NSInteger)tag at:(NSPoint)p
{
    NSComboBox *c = [[NSComboBox alloc] initWithFrame:NSMakeRect(p.x, p.y, 120, 24)];
    if (tag == PVZ2_SLOT_COUNT + 6) {
        [c addItemsWithObjectValues:@[@"Fixed", @"Adapt"]];
        c.editable = NO;
    } else if (tag == PVZ2_SLOT_COUNT + 7) {
        [c addItemsWithObjectValues:@[@"Lift", @"Plant at cursor"]];
        c.editable = NO;
    } else {
        [c addItemsWithObjectValues:key_choices()];
        c.editable = YES;              /* any key can be typed in */
    }
    c.stringValue = value;
    c.target = self;
    c.action = @selector(comboChanged:);
    c.tag = tag;
    return c;
}

+ (NSTextField *)numberFor:(NSString *)key value:(double)value tag:(NSInteger)tag at:(NSPoint)p
{
    NSTextField *t = [[NSTextField alloc] initWithFrame:NSMakeRect(p.x, p.y, 90, 22)];
    t.stringValue = [NSString stringWithFormat:@"%g", value];
    t.target = self;
    t.action = @selector(numberChanged:);
    t.tag = tag;
    return t;
}

+ (void)build:(NSView *)view
{
    view.wantsLayer = YES;
    view.layer.backgroundColor = [NSColor controlBackgroundColor].CGColor;
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    CGFloat y = 500;
    NSTextField *title = [NSTextField labelWithString:@"Seed packets"];
    title.font = [NSFont boldSystemFontOfSize:13];
    title.frame = NSMakeRect(20, y, 300, 20);
    [view addSubview:title];
    y -= 26;
    for (int i = 0; i < PVZ2_SLOT_COUNT; i++) {
        NSString *value = shown_key(kSlotKey[i], default_slot_key(i));
        [view addSubview:[self label:[NSString stringWithFormat:@"Seed packet %d", i + 1]
                                at:NSMakePoint(20, y)]];
        NSComboBox *c = [self comboFor:kSlotKey[i] value:value tag:i at:NSMakePoint(220, y - 2)];
        g_combos[i] = c;
        [view addSubview:c];
        y -= 30;
    }
    [view addSubview:[self label:@"Shovel" at:NSMakePoint(20, y)]];
    NSString *sh = shown_key(kShovelKey, @(PVZ2_DEFAULT_SHOVEL_KEY));
    NSComboBox *sc = [self comboFor:kShovelKey value:sh tag:PVZ2_SLOT_COUNT
                            at:NSMakePoint(220, y - 2)];
    g_combos[PVZ2_SLOT_COUNT] = sc;
    [view addSubview:sc];
    y -= 34;

    NSTextField *title2 = [NSTextField labelWithString:@"Where the packets are"];
    title2.font = [NSFont boldSystemFontOfSize:13];
    title2.frame = NSMakeRect(20, y, 300, 20);
    [view addSubview:title2];
    y -= 26;
    [view addSubview:[self label:@"The first packet is at x" at:NSMakePoint(20, y)]];
    NSArray *names = @[@"The first packet is at x", @"and at y",
                       @"The packets are this far apart", @"The shovel is at x", @"and at y"];
    NSArray *keys = @[kPacketX, kPacketY, kPacketStep, kShovelX, kShovelY];
    NSArray *fallbacks = @[@(PVZ2_PACKET_X), @(PVZ2_PACKET_Y), @(PVZ2_PACKET_STEP),
                          @(PVZ2_SHOVEL_X), @(PVZ2_SHOVEL_Y)];
    for (int i = 0; i < 5; i++) {
        [view addSubview:[self label:names[i] at:NSMakePoint(20, y)]];
        NSTextField *t = [self numberFor:keys[i]
                            value:setting_double(keys[i], [fallbacks[i] doubleValue])
                            tag:PVZ2_SLOT_COUNT + 1 + i at:NSMakePoint(220, y - 2)];
        g_numbers[i] = t;
        [view addSubview:t];
        y -= 28;
    }
    y -= 40;

    NSTextField *title3 = [NSTextField labelWithString:@"When the window is resized"];
    title3.font = [NSFont boldSystemFontOfSize:13];
    title3.frame = NSMakeRect(20, y, 300, 20);
    [view addSubview:title3];
    y -= 26;
    [view addSubview:[self label:@"Window resize" at:NSMakePoint(20, y)]];
    NSString *mode = [d stringForKey:kResizeMode];
    NSComboBox *rc = [self comboFor:kResizeMode value:mode.length ? mode : @"Fixed"
                             tag:PVZ2_SLOT_COUNT + 6 at:NSMakePoint(220, y - 2)];
    g_mode_combo = rc;
    [view addSubview:rc];
    y -= 34;

    [view addSubview:[self label:@"Native shovel" at:NSMakePoint(20, y)]];
    NSButton *ns = [NSButton checkboxWithTitle:@"(experimental)" target:self
                                         action:@selector(nativeShovelChanged:)];
    ns.state = native_shovel_mode() ? NSControlStateValueOn : NSControlStateValueOff;
    ns.frame = NSMakeRect(220, y - 2, 200, 20);
    [view addSubview:ns];
    y -= 34;
    NSTextField *title4 = [NSTextField labelWithString:@"What a shortcut does"];
    title4.font = [NSFont boldSystemFontOfSize:13];
    title4.frame = NSMakeRect(20, y, 300, 20);
    [view addSubview:title4];
    y -= 26;
    [view addSubview:[self label:@"Shortcut shape" at:NSMakePoint(20, y)]];
    NSString *shape = [d stringForKey:kShortcutShape];
    NSComboBox *sc2 = [self comboFor:kShortcutShape
                              value:shape.length ? shape : @"Lift"
                              tag:PVZ2_SLOT_COUNT + 7 at:NSMakePoint(220, y - 2)];
    g_shape_combo = sc2;
    [view addSubview:sc2];
    y -= 34;

    NSTextField *note = [NSTextField wrappingLabelWithString:
        @"A shortcut is a tap on the seed packet, the same tap the mouse makes; the "
        @"game's own UI is not changed. The positions are measured, not known: the log "
        @"(pvz2.log) says where a shortcut tapped, so the numbers can be corrected to "
        @"what is on the screen.\n\n"
        @"Letterbox keeps the game's own surface and scales the picture into the "
        @"window, so the game never notices the window's shape. Adapt (experimental) "
        @"makes the surface the window's size and tells the game, so it lays its UI "
        @"out for the shape it is in -- what Android does when a window is resized. "
        @"The change takes effect the next time the app is started.\n\n"
        @"Lift takes the packet in hand and your click plants it -- the game plants "
        @"by a drag, so a tap on a packet does nothing. Plant at cursor drops the "
        @"packet on the cell under the pointer, with that same drag."];
    note.font = [NSFont systemFontOfSize:11];
    note.frame = NSMakeRect(20, y - 195, 410, 195);
    [view addSubview:note];
    y -= 195;

    /* How tall the panel has to be: the content runs from the top (520) down
     * to here, and the window is made that tall (in a scroll view, so it fits
     * on a short screen as well). The old panel was a fixed 660 points and
     * its last rows -- the resize mode and the note -- were below the bottom
     * of the window, where nobody can reach them. */
    g_prefs_bottom = y;
    g_prefs_height = 520 - y + 20;
}

+ (void)nativeShovelChanged:(NSButton *)sender
{
    [[NSUserDefaults standardUserDefaults] setBool:(sender.state == NSControlStateValueOn)
                                            forKey:kNativeShovel];
    tl_pvz2_set_native_shovel(native_shovel_mode());
    /* The native seed pick: the game's own press. */
    tl_pvz2_set_seed_pick(native_seed_pick_mode());
    tl_log_line("macos: the native shovel is %s", native_shovel_mode() ? "on" : "off");
}

+ (void)comboChanged:(NSComboBox *)sender
{
    NSString *key = [self keyForTag:sender.tag];
    if (!key) return;
    /* A combo box's own value is still the old one when its action comes from a
     * pick in its list: the picked item is what was chosen. (Reading the value
     * is why Letterbox -> Adapt was never stored.) */
    NSString *value = (!sender.editable && sender.indexOfSelectedItem >= 0)
                    ? sender.objectValueOfSelectedItem : sender.stringValue;
    /* The two rows that are not keys -- the resize mode and the shortcut's
     * shape -- are not keys, so the key's rules are not theirs. */
    BOOL is_a_key = sender.tag < PVZ2_SLOT_COUNT + 6;
    if (is_a_key) {
        if ([value caseInsensitiveCompare:@"None"] == NSOrderedSame) value = @"";
        if (!usable_key(value)) {
            /* A key the app would never recognise: the shortcut would be dead
             * and the panel would not say so. */
            tl_log_line("macos: \"%s\" is not a key the app can use -- the preference is left as it was",
                        sender.stringValue.UTF8String);
            [self refresh];
            return;
        }
        if (value.length && [self key:value isUsedByAnother:(NSInteger)sender.tag]) {
            /* One key, one shortcut: the first match wins, so a duplicate
             * would take the other one's key away without saying so. */
            tl_log_line("macos: the key \"%s\" is already used by another shortcut -- the preference is left as it was",
                        value.UTF8String);
            [self refresh];
            return;
        }
    }
    [[NSUserDefaults standardUserDefaults] setObject:value forKey:key];
    tl_log_line("macos: the preference %@ is now \"%s\"", key.UTF8String, value.UTF8String);
    /* The resize mode is not a preference the game reads: it is how the host
     * puts the picture on the screen, so the host has to act on it at once.
     * The shortcut's shape is read when a shortcut is pressed. */
    if ([key isEqualToString:kResizeMode]) resize_mode_changed();
}

/* Is this key already another shortcut's? The one whose box is being changed
 * does not count. */
+ (BOOL)key:(NSString *)value isUsedByAnother:(NSInteger)tag
{
    if (!value.length) return NO;
    for (int i = 0; i < PVZ2_SLOT_COUNT; i++) {
        if (i == tag) continue;
        NSString *k = setting_key(kSlotKey[i]);
        if (!k) k = default_slot_key(i);
        if (k.length && [k caseInsensitiveCompare:value] == NSOrderedSame) return YES;
    }
    if (tag != PVZ2_SLOT_COUNT) {
        NSString *sh = setting_key(kShovelKey);
        if (!sh) sh = @(PVZ2_DEFAULT_SHOVEL_KEY);
        if (sh.length && [sh caseInsensitiveCompare:value] == NSOrderedSame) return YES;
    }
    return NO;
}

+ (void)numberChanged:(NSTextField *)sender
{
    NSString *key = [self keyForTag:sender.tag];
    if (!key) return;
    double value = sender.doubleValue;
    /* The numbers are places in the game's own 1748x804, so they have to be
     * places: a number that is not one (an empty field reads as 0, and a word
     * reads as 0 too) would tap a corner of the screen that has nothing in
     * it. */
    if (!isfinite(value) || !NSLocationInRange((NSUInteger)value, [self numberRangeForTag:sender.tag])) {
        tl_log_line("macos: %g is not a place in the game -- the preference is left as it was", value);
        [self refresh];
        return;
    }
    [[NSUserDefaults standardUserDefaults] setObject:@(value) forKey:key];
    tl_log_line("macos: the preference %@ is now %g", key.UTF8String, value);
}

/* What a number in the panel may be: a place in the game's own picture, or a
 * distance between two of them. */
+ (NSRange)numberRangeForTag:(NSInteger)tag
{
    /* The tags: +1 the first packet's x, +2 its y, +3 the step between the
     * packets, +4 the shovel's x, +5 its y. */
    switch (tag - PVZ2_SLOT_COUNT) {
    case 2:  return NSMakeRange(0, 804);   /* the first packet's y */
    case 3:  return NSMakeRange(1, 400);   /* the step */
    case 5:  return NSMakeRange(0, 804);   /* the shovel's y */
    default: return NSMakeRange(0, 1748);  /* an x */
    }
}

/* The panel shows what is stored, not what was there when it was built: the
 * resize mode can be changed while the game runs (the tap script does it),
 * and a panel that still said "Letterbox" would put it back when the person
 * touched anything. */
+ (void)refresh
{
    for (int i = 0; i < PVZ2_SLOT_COUNT; i++) {
        if (!g_combos[i]) continue;
        g_combos[i].stringValue = shown_key(kSlotKey[i], default_slot_key(i));
    }
    if (g_combos[PVZ2_SLOT_COUNT])
        g_combos[PVZ2_SLOT_COUNT].stringValue =
            shown_key(kShovelKey, @(PVZ2_DEFAULT_SHOVEL_KEY));
    if (g_mode_combo) {
        NSString *mode = [[NSUserDefaults standardUserDefaults] stringForKey:kResizeMode];
        g_mode_combo.stringValue = mode.length ? mode : @"Fixed";
    }
    if (g_shape_combo) {
        NSString *shape = [[NSUserDefaults standardUserDefaults] stringForKey:kShortcutShape];
        g_shape_combo.stringValue = shape.length ? shape : @"Lift";
    }
    NSString *const number_keys[5] = { kPacketX, kPacketY, kPacketStep, kShovelX, kShovelY };
    const double fallbacks[5] = { PVZ2_PACKET_X, PVZ2_PACKET_Y, PVZ2_PACKET_STEP,
                                  PVZ2_SHOVEL_X, PVZ2_SHOVEL_Y };
    for (int i = 0; i < 5; i++) {
        if (!g_numbers[i]) continue;
        g_numbers[i].stringValue = [NSString stringWithFormat:@"%g",
                                    setting_double(number_keys[i], fallbacks[i])];
    }
}

+ (void)show
{
    static NSWindow *window;
    if (!window) {
        /* The content is built into a document view inside a scroll view, and
         * the window is as tall as the content (or as tall as the screen, if
         * the screen is shorter). The panel used to be a fixed 660 points,
         * which was shorter than its own rows: the resize mode and the note
         * were below the bottom of the window, where nobody can reach them. */
        NSView *doc = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 460, 660)];
        [self build:doc];
        /* The rows are laid out from the top down, so the content ends below
         * the view's origin: the view's visible region has to be moved to
         * where the content is, or the last rows (the resize mode, the
         * shortcut shape, the note) are outside it and nobody can see them. */
        doc.bounds = NSMakeRect(0, g_prefs_bottom, 460, g_prefs_height);
        doc.frame = NSMakeRect(0, 0, 460, g_prefs_height);
        /* The window is as tall as the content, so that every row is on the
         * screen: the panel's rows used to run past the bottom of a fixed
         * 660-point window, and the resize mode and the note were out of
         * sight. If the screen is shorter, the panel scrolls. */
        CGFloat height = g_prefs_height;
        NSScreen *screen0 = [NSScreen mainScreen];
        if (screen0 && height > screen0.visibleFrame.size.height - 40)
            height = screen0.visibleFrame.size.height - 40;
        NSScrollView *scroll = [[NSScrollView alloc]
            initWithFrame:NSMakeRect(0, 0, 460, height)];
        scroll.hasVerticalScroller = YES;
        scroll.drawsBackground = NO;
        scroll.documentView = doc;
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 460, height)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
            backing:NSBackingStoreBuffered defer:NO];
        window.title = @"Reflourished Preferences";
        window.releasedWhenClosed = NO;
        window.contentView = scroll;
    }
    /* What is stored, not what was there when the panel was built. */
    [self refresh];
    [window center];
    /* The panel must fit the screen: on a short one the top of it would be
     * off the top, and the rows nobody can reach are rows nobody can fix. */
    NSScreen *screen = [NSScreen mainScreen];
    if (screen) {
        NSRect visible = screen.visibleFrame;
        NSRect frame = window.frame;
        if (frame.size.height > visible.size.height - 20) {
            frame.size.height = visible.size.height - 20;
            [window setFrame:frame display:YES];
        }
        frame = window.frame;
        if (!NSContainsRect(visible, NSMakeRect(frame.origin.x, frame.origin.y,
                                                frame.size.width, frame.size.height))) {
            [window center];
            frame = window.frame;
            if (frame.origin.y < visible.origin.y) {
                frame.origin.y = visible.origin.y;
                [window setFrame:frame display:YES];
            }
        }
    }
    [window makeKeyAndOrderFront:nil];
    /* The preferences are the app's own window; in the background mode (a test
     * that must not take the focus) it is shown without it. */
    if (!(getenv("TL_PVZ2_QUIET") && getenv("TL_PVZ2_QUIET")[0] == '1')
        && !background_launch())
        [NSApp activateIgnoringOtherApps:YES];
    /* A picture of the panel, for a test. The window is behind whatever the
     * person is doing, and the window server keeps no picture of a covered
     * window ("could not create image from window"), so the app draws its own
     * panel into a file. It is the panel's own pixels, not a picture of the
     * screen. */
    const char *shot = getenv("TL_PVZ2_PREFS_SHOT");
    if (shot && *shot) prefs_shot(window, shot);
}

/* The panel's own content, drawn into a PNG. The whole content, not just the
 * part that fits on the screen: a row that is off the screen is a row nobody
 * can see, and that is the thing to look at. */
static void prefs_shot(NSWindow *window, const char *path)
{
    /* The content of the panel: the scroll view's document, which is all of
     * it -- the rows that do not fit on the screen are the ones a person
     * cannot reach, and they are the ones to look at. */
    NSView *doc = nil;
    if ([window.contentView isKindOfClass:NSScrollView.class])
        doc = ((NSScrollView *)window.contentView).documentView;
    if (!doc) doc = window.contentView;
    NSRect bounds = doc.bounds;
    if (bounds.size.width < 1 || bounds.size.height < 1) return;
    NSBitmapImageRep *rep = [doc bitmapImageRepForCachingDisplayInRect:bounds];
    if (!rep) return;
    [doc cacheDisplayInRect:bounds toBitmapImageRep:rep];
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    if (png) {
        [png writeToFile:@(path) atomically:YES];
        tl_log_line("macos: the preferences were drawn into %s", path);
    }
}
@end

/* ------------------------------------------------------------------ the window */

@implementation PVZ2View
- (BOOL)isFlipped { return YES; }

/* How the game is put on the screen.
 *
 * Letterbox (the default): the surface is the game's own size and only the
 * presentation is scaled, so the guest's layout never changes with the window's
 * shape.
 *
 * Adapt (experimental): the surface is the window's own size -- the drawable is
 * the window's pixels -- and the guest is told, so it lays its UI out again for
 * the shape it is in. That is what Android does when a window is resized.
 */
- (void)applyPresentation
{
    NSRect b = self.bounds;
    double scale = self.metal.contentsScale;
    /* The surface keeps the shape the game laid itself out for. Its size: the
     * game's own in the letterbox mode, the window's pixels in the adapt mode
     * -- but not while the window is being dragged: then the picture is only
     * scaled, and the game is told the size once the person lets go. */
    int sw = atomic_load(&g_surface_w), sh = atomic_load(&g_surface_h);
    {
        double x, y, vw, vh;
        if (!self.inLiveResize && !g_starting && !g_fs_moving &&
            tl_viewport_fit(b.size.width, b.size.height, g_layout_w, g_layout_h, &x, &y, &vw, &vh)) {
            sw = (int)lround(vw * scale);
            sh = (int)lround(vh * scale);
        }
    }
    tl_viewport_present(self.metal, b.size.width, b.size.height, sw, sh, scale);
    if (sw > 0 && sh > 0 &&
        (sw != atomic_load(&g_surface_w) || sh != atomic_load(&g_surface_h))) {
        atomic_store(&g_surface_w, sw);
        atomic_store(&g_surface_h, sh);
        tl_pvz2_set_surface_size(sw, sh);
    }
    [self updateFitButton];
}

- (void)viewDidEndLiveResize
{
    [super viewDidEndLiveResize];
    [self applyPresentation];
}

/* The window went to a display with another pixel density (or the setting
 * changed): the game draws at that display's scale from now on. It used to be
 * fixed at launch, so a game started on a standard display stayed soft after
 * the window was moved to a Retina one. */
- (void)followDisplayScale
{
    double s = render_scale(self.window);
    if (g_starting || !self.window || fabs(s - self.metal.contentsScale) < 0.01) return;
    tl_log_line("macos: the game draws at %.0fx now (was %.0fx)", s, self.metal.contentsScale);
    self.metal.contentsScale = s;
    [self applyPresentation];
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    [self followDisplayScale];
}

/* Two buttons, shown when the window is not the shape the game is laid out
 * for (a resize in the adapt mode, a new shape chosen in the settings): one
 * restarts the game in the window's shape, the other puts the window back in
 * the game's -- the way out of a resize that was not meant (dragging a corner
 * changes the shape freely). They sit in the larger black bar. */
static NSButton *round_button(NSString *symbol, NSString *tip, SEL action, NSView *in)
{
    NSImage *icon = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:tip];
    NSButton *btn = [NSButton buttonWithImage:icon target:NSApp.delegate action:action];
    btn.bordered = NO;
    btn.symbolConfiguration = [NSImageSymbolConfiguration configurationWithPointSize:15
                                                                              weight:NSFontWeightSemibold];
    btn.contentTintColor = NSColor.whiteColor;
    btn.wantsLayer = YES;
    btn.layer.backgroundColor = [NSColor colorWithWhite:0.25 alpha:0.85].CGColor;
    btn.layer.cornerRadius = 18;
    btn.toolTip = tip;
    [in addSubview:btn];
    return btn;
}

- (void)updateFitButton
{
    NSRect b = self.bounds;
    double x, y, vw, vh;
    bool fits = tl_viewport_fit(b.size.width, b.size.height, g_layout_w, g_layout_h, &x, &y, &vw, &vh);
    bool same = b.size.height > 0 && g_layout_h > 0 &&
                fabs((b.size.width / b.size.height) / ((double)g_layout_w / g_layout_h) - 1.0) <= 0.02;
    bool full = (self.window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (same && !full && !self.inLiveResize) g_good_frame = self.window.frame;
    if (same || !fits) { g_fit_button.hidden = YES; g_keep_button.hidden = YES; return; }
    if (!g_fit_button) {
        g_fit_button = round_button(@"arrow.clockwise",
                                    @"Restart the game in the window's shape (the game saves first)",
                                    @selector(restartToFit:), self);
        g_keep_button = round_button(@"xmark",
                                     @"Put the window back in the game's shape",
                                     @selector(keepGameShape:), self);
    }
    const double r = 18, pad = 10, gap = 8;
    double cx, cy, kx, ky;
    if (vw < b.size.width - 1) {
        /* Bars at the sides: in the right one, near the top, one above the other. */
        double bar = b.size.width - (x + vw);
        cx = bar >= 2 * (r + pad) ? x + vw + bar / 2 : b.size.width - r - pad;
        cy = r + pad + 30;
        kx = cx; ky = cy + 2 * r + gap;
    } else {
        /* Bars above and below: in the bottom one, at the right, side by side. */
        double bar = b.size.height - (y + vh);
        cy = bar >= 2 * (r + pad) ? y + vh + bar / 2 : b.size.height - r - pad;
        cx = b.size.width - r - pad - 30;
        kx = cx - 2 * r - gap; ky = cy;
    }
    g_fit_button.frame = NSMakeRect(cx - r, cy - r, 2 * r, 2 * r);
    g_keep_button.frame = NSMakeRect(kx - r, ky - r, 2 * r, 2 * r);
    g_fit_button.hidden = NO;
    /* A window in full screen cannot change its shape. */
    g_keep_button.hidden = full;
}

- (void)layout
{
    [super layout];
    [self applyPresentation];
}
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }
- (void)touch:(int)phase event:(NSEvent *)event
{
    if (!atomic_load(&ready)) return;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    [self windowTap:phase at:p];
}

/* A tap in the window's own points, mapped into the game's pixels: through the
 * viewport in the letterbox mode, one to one in the adapt mode. */
- (void)windowTap:(int)phase at:(NSPoint)p
{
    if (!atomic_load(&ready)) return;
    /* The finger the game has down, and where it last was. A press outside the
     * game's picture (on a letterbox bar) is no touch at all; but a move or a let-go
     * out there still belongs to the finger that went down inside, and is sent where
     * it last was in the picture -- a let-go that was dropped left the game a finger
     * down for good, and a finger that is down makes every later tap do nothing. */
    static bool down;
    static double last_x, last_y;
    double gx, gy;
    {
        /* The surface's pixels: the picture is the surface, scaled into the window. */
        double x, y, w, h;
        int sw = atomic_load(&g_surface_w), sh = atomic_load(&g_surface_h);
        if (!tl_viewport_fit(self.bounds.size.width, self.bounds.size.height,
                             sw, sh, &x, &y, &w, &h) ||
            !tl_viewport_map(p.x, p.y, x, y, w, h, sw, sh, &gx, &gy)) {
            if (phase == 0 || !down) return;
            gx = last_x; gy = last_y;
        }
    }
    if (phase == 0 && down) {
        /* A let-go was lost (the button came up somewhere AppKit kept to itself):
         * the old finger goes up first. */
        tl_log_line("macos: a touch was still down; it is let go first");
        tl_pvz2_touch(3, 1, last_x, last_y);
    }
    if (phase == 0) down = true;
    else if (phase == 3 || phase == 4) down = false;
    last_x = gx; last_y = gy;
    if (atomic_load(&g_packet_in_hand)) {
        /* A packet is in hand: the mouse carries it where the player points and
         * the click lets it go. That is the same thing as dragging the packet
         * out of the bank with the mouse -- the game plants by a drag. */
        if (phase == 3) packet_carry_to(gx, gy);
        else tl_pvz2_touch(1, 1, gx, gy);
        return;
    }
    /* A click ends the previous edit, as dismissing Android's keyboard does.
     * The guest can start another edit if this click lands in a name field. */
    if (phase == 0) tl_pvz2_keyboard_dismissed();
    tl_pvz2_touch(phase, 1, gx, gy);
}
- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    tl_log_line("macos: the mouse pressed at (%.0f,%.0f) in the window", p.x, p.y);
    [self touch:0 event:event];
}
- (void)mouseDragged:(NSEvent *)event { [self touch:1 event:event]; }
- (void)mouseUp:(NSEvent *)event { [self touch:3 event:event]; }
- (void)rightMouseDown:(NSEvent *)event { [self touch:0 event:event]; }
- (void)rightMouseDragged:(NSEvent *)event { [self touch:1 event:event]; }
- (void)rightMouseUp:(NSEvent *)event { [self touch:3 event:event]; }

/* The guest asks for a keyboard when the player taps the name field
 * (Device_ShowKeyboard). On a Mac the keyboard is already there, so all that is
 * needed is for this view to be the one that gets the keys -- and for the guest to
 * be told a keyboard is up, which it cannot work out for itself. */
static void keyboard_shown(int show)
{
    tl_log_line("macos: the guest asked for a keyboard (showing=%d)", show);
    dispatch_async(dispatch_get_main_queue(), ^{
        /* The *game's* window, not the key window: the preferences may have
         * the key, and the game is what needs the keys. And the game's view is
         * the window's content view itself -- looking for it among the
         * subviews never found it, so the guest's request for a keyboard was
         * dropped. */
        NSWindow *w = g_window;
        if (!w) return;
        NSView *v = w.contentView;
        if ([v isKindOfClass:PVZ2View.class]) { [w makeFirstResponder:v]; return; }
        for (NSView *s in v.subviews)
            if ([s isKindOfClass:PVZ2View.class]) { [w makeFirstResponder:s]; return; }
    });
}

/* The packet in hand.
 *
 * The game plants by *dragging* a seed packet out of the bank. Measured: a
 * tap on a packet changes nothing (the card does not even light up) -- the
 * packet has to be held and then let go over the lawn. So the shortcut takes
 * the packet in hand (the press) and the player's next click is where it
 * goes: the click carries the packet there and lets go, which is what
 * dragging it with the mouse does. The shovel is a mode button, and a tap
 * on it is enough (measured: the shovel's ring lights up).
 */

/* Take a packet in hand, or put it back into the bank. */
static void packet_take_in_hand(double x, double y)
{
    atomic_store(&g_packet_in_hand, 1);
    atomic_fetch_add(&g_packet_hand_generation, 1);
    uint32_t generation = atomic_load(&g_packet_hand_generation);
    g_packet_hand_x = x;
    g_packet_hand_y = y;
    tl_pvz2_touch_injected(0, 1, (float)x, (float)y);
    /* If the player does not click, the packet goes back into the bank: it
     * must not stay in hand for ever. (Twenty-five seconds: long enough for
     * the player to point at the spot.) */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(25 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        /* Only this packet's own timer may put it back: a later packet may
         * already be in hand. */
        if (!atomic_load(&g_packet_in_hand)) return;
        if (atomic_load(&g_packet_hand_generation) != generation) return;
        atomic_store(&g_packet_in_hand, 0);
        tl_pvz2_touch_injected(3, 1, (float)x, (float)y);
        tl_log_line("macos: the packet was put back into the bank (%.0f,%.0f)", x, y);
    });
}

/* Carry the packet in hand to a place in the game's own pixels and let it go
 * there. This is what the player's click does, and what the test's own tap
 * does. */
static void packet_carry_to(double gx, double gy)
{
    if (!atomic_load(&g_packet_in_hand)) return;
    tl_pvz2_touch(1, 1, (float)gx, (float)gy);
    usleep(120000);
    tl_pvz2_touch(3, 1, (float)gx, (float)gy);
    atomic_store(&g_packet_in_hand, 0);
    tl_log_line("macos: the packet was let go at (%.0f,%.0f)", gx, gy);
}

/* Plant the packet in hand where the pointer is: the same drag, with the
 * pointer's place as the end of it. The packet is let go a moment later, so
 * the game sees the drag it wants (a press, a carry, a release). */
static void packet_plant_at(double gx, double gy)
{
    if (!atomic_load(&g_packet_in_hand)) return;
    /* A drag, not a jump: the finger goes from the packet to the spot, in a
     * few steps, and is let go there. The steps are short, so the run loop is
     * not held up for long, and the release is sent here and not later: a
     * release that goes out after another press would release the wrong
     * finger and leave the game's finger down for ever. */
    double fx = g_packet_hand_x, fy = g_packet_hand_y;
    for (int i = 1; i <= 5; i++) {
        double t = (double)i / 5.0;
        tl_pvz2_touch(1, 1, (float)(fx + (gx - fx) * t), (float)(fy + (gy - fy) * t));
        usleep(20000);
    }
    atomic_store(&g_packet_in_hand, 0);
    tl_pvz2_touch(3, 1, (float)gx, (float)gy);
    tl_log_line("macos: the packet was planted at (%.0f,%.0f)", gx, gy);
}

/* Where the pointer is.
 *
 * The person's mouse is never moved: the app reads the cursor where it is,
 * and a test can say where the cursor is (the tap script's "cursor:" token),
 * so the "plant at the cursor" shape can be proved without touching the
 * person at the keyboard. Both are in the game's own 1748x804 space, which is
 * the space the packets' positions are in as well. */
static atomic_llong g_cursor_set;          /* a test has said where it is */
static atomic_llong g_cursor_xy;           /* the game's own space, packed */

/* Where a test says the pointer is, in the game's own space. The two numbers
 * are packed into one word: the tap script's thread writes them and the main
 * thread reads them, and two plain doubles could be read half-updated. */
static void cursor_set_game(double gx, double gy)
{
    int32_t x = (int32_t)gx, y = (int32_t)gy;
    atomic_store(&g_cursor_xy, ((long long)(uint32_t)x << 32) | (uint32_t)y);
    atomic_store(&g_cursor_set, 1);
}

/* The pointer's place in the game's own space, or false when it is not over
 * the game at all (then there is nothing to plant on). */
static bool pointer_in_game(double *gx, double *gy)
{
    if (!g_view) return false;
    if (atomic_load(&g_cursor_set)) {
        long long packed = atomic_load(&g_cursor_xy);
        *gx = (double)(int32_t)(uint32_t)((unsigned long long)packed >> 32);
        *gy = (double)(int32_t)(uint32_t)((unsigned long long)packed & 0xffffffffu);
        return true;
    }
    NSWindow *w = g_window;
    if (!w) return false;
    /* The real cursor: the screen's place, then the window's, then the
     * view's. Reading it does not move it and needs no permission. */
    NSPoint p = [g_view convertPoint:[w convertPointFromScreen:[NSEvent mouseLocation]]
                              fromView:nil];
    if (!NSPointInRect(p, g_view.bounds)) return false;
    /* The picture is the surface, scaled into the window; the surface's pixels
     * are then the game's own space (one to one in the letterbox mode). */
    double x, y, vw, vh, sx, sy;
    int sw = atomic_load(&g_surface_w), sh = atomic_load(&g_surface_h);
    if (!tl_viewport_fit(g_view.bounds.size.width, g_view.bounds.size.height,
                         sw, sh, &x, &y, &vw, &vh) ||
        !tl_viewport_map(p.x, p.y, x, y, vw, vh, sw, sh, &sx, &sy))
        return false;
    surface_to_game(sx, sy, gx, gy);
    return true;
}

/* The keys. Escape pauses a level and is Android's Back elsewhere (keycode 4), Return is the IME's done key
 * (keycode 66), Backspace is one character deleted, and everything else is text the
 * way Android's EditInputConnection sends it: the characters that were committed
 * (action 0), and one event per character deleted (action 3). */
- (void)keyDown:(NSEvent *)event
{
    if (!atomic_load(&ready) || (event.modifierFlags & NSEventModifierFlagCommand)) return;
    /* A shortcut: a key that takes a seed packet in hand. The game's own UI is
     * not changed: the packet is pressed exactly as the mouse presses it. */
    if (!tl_pvz2_keyboard_showing() && !event.isARepeat &&
        !(event.modifierFlags & (NSEventModifierFlagControl | NSEventModifierFlagOption))) {
        /* Space (or the key the settings give it): every sun and coin on the
         * lawn flies to its counter, the game's own collection. */
        NSString *name = key_name(event);
        NSString *collectKey = setting_key(@"collectSunKey") ?: @"Space";
        if (name && collectKey.length && [collectKey caseInsensitiveCompare:name] == NSOrderedSame) {
            tl_pvz2_request_collect_sun();
            return;
        }
        int slot = shortcut_slot(name);
        if (slot >= 0) {
            double x, y, sx, sy;
            if (slot < PVZ2_SLOT_COUNT) {
                packet_position(slot, &x, &y);
                game_to_surface(x, y, &sx, &sy);
                tl_log_line("macos: the shortcut for a seed packet takes it in hand at (%.0f,%.0f) -- on the surface (%.0f,%.0f)",
                            x, y, sx, sy);
                if (atomic_load(&g_packet_in_hand)) {
                    /* The packet already in hand goes back into the bank first.
                     * Its place is in the surface's pixels already. */
                    tl_pvz2_touch(3, 1, (float)g_packet_hand_x, (float)g_packet_hand_y);
                    atomic_store(&g_packet_in_hand, 0);
                }
                if (native_seed_pick_mode()) {
                    /* The game's own press: the card's own pointer-down
                     * handler, on the game's own thread. No touch, no
                     * coordinates -- and the game itself refuses a card that
                     * cannot be paid for, or that is on its cooldown. */
                    tl_pvz2_request_seed_pick(slot);
                    tl_log_line("macos: the shortcut for a seed packet asks the guest's own press (slot %d)", slot);
                    return;
                }
                if (!seed_shortcut_enabled()) {
                    tl_log_line("macos: the seed shortcut is off: the synthetic touch is broken at any other surface size, and the game's own pick is not in yet");
                    return;
                }
                packet_take_in_hand(sx, sy);
                if (plant_at_cursor()) {
                    /* The other shape: the packet is dropped on the cell under
                     * the pointer, with the drag the game wants. */
                    double px, py;
                    if (pointer_in_game(&px, &py)) {
                        double cx, cy;
                        game_to_surface(px, py, &cx, &cy);
                        tl_log_line("macos: the shortcut plants the packet at the pointer (%.0f,%.0f)",
                                    px, py);
                        packet_plant_at(cx, cy);
                    } else {
                        tl_log_line("macos: the pointer is not over the game -- the packet stays in hand");
                    }
                }
            } else if (native_shovel_mode()) {
                /* The guest's own shovel, on the game's own thread: no touch at
                 * all -- the game is asked to press its own shovel button. */
                tl_pvz2_request_native_shovel();
                tl_log_line("macos: the shortcut for the shovel asks the guest's own shovel");
                return;
            } else {
                /* The shovel, the old way: a tap where the button is. */
                x = setting_double(kShovelX, PVZ2_SHOVEL_X);
                y = setting_double(kShovelY, PVZ2_SHOVEL_Y);
                game_to_surface(x, y, &sx, &sy);
                tl_log_line("macos: the shortcut for the shovel taps (%.0f,%.0f)", x, y);
                tl_pvz2_touch(0, 1, (float)sx, (float)sy);
                /* The up comes 120 ms later, without blocking the run loop: the
                 * window must not freeze while a shortcut is pressed. */
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(120 * NSEC_PER_MSEC)),
                               dispatch_get_main_queue(), ^{
                    tl_pvz2_touch(3, 1, (float)sx, (float)sy);
                });
            }
            return;
        }
    }
    switch (event.keyCode) {
    case 53:                                            /* Escape: Android's Back */
        if (atomic_load(&g_packet_in_hand)) {
            /* A packet in hand goes back into the bank. */
            atomic_store(&g_packet_in_hand, 0);
            tl_pvz2_touch(3, 1, (float)g_packet_hand_x, (float)g_packet_hand_y);
            tl_log_line("macos: the packet was put back into the bank (%.0f,%.0f)",
                        g_packet_hand_x, g_packet_hand_y);
            return;
        }
        tl_pvz2_request_escape();
        return;
    case 36:                                            /* Return: the IME's done */
        tl_pvz2_key(66, 1); tl_pvz2_key(66, 0);
        return;
    case 51:                                            /* Backspace: one character */
        tl_pvz2_text_action(3, "");
        return;
    default: break;
    }
    NSString *chars = event.characters;
    if (!chars.length) return;
    unichar c = [chars characterAtIndex:0];
    if (c < 0x20 || c == 0x7f) return;                  /* a control character */
    tl_pvz2_text_action(0, chars.UTF8String);
}
- (void)keyUp:(NSEvent *)event { (void)event; }
@end

/* The keys the tests press, without needing the app to be the frontmost one (a
 * key event posted by another process needs that process to be allowed to send
 * input, and the game is not always the app the player is looking at). The
 * event goes through the same keyDown the real key goes through. */
static void *key_stroker(void *arg)
{
    PVZ2View *view = (__bridge PVZ2View *)arg;
    const char *keys = getenv("TL_PVZ2_KEYSTROKE");
    if (!keys || !*keys) return NULL;
    /* When to press them: the shortcuts are off while the guest is asking for a
     * keyboard (the name field), so the test presses them once the game is in a
     * level. */
    int wait_s = getenv("TL_PVZ2_KEYSTROKE_DELAY") ? atoi(getenv("TL_PVZ2_KEYSTROKE_DELAY")) : 40;
    for (int i = 0; i < wait_s * 2; i++) {
        usleep(500000);
        if (!view) break;
    }
    for (const char *k = keys; *k; k++) {
        /* a '.' is a five-second pause, so a test can press, let the game
         * answer, and press again */
        if (*k == '.') { sleep(5); continue; }
        NSString *s = [NSString stringWithFormat:@"%c", *k];
        tl_log_line("macos: the test presses \"%s\"", s.UTF8String);
        /* The key goes through the app's own key handler, and that is the
         * main thread's business: AppKit's view state is main-thread-only,
         * and this thread is not it. (The wait has a timeout, so a main
         * thread that is busy cannot hang the test.) */
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_main_queue(), ^{
            NSEvent *e = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                                     modifierFlags:0 timestamp:0 windowNumber:0 context:nil
                                     characters:s charactersIgnoringModifiers:s isARepeat:NO
                                     keyCode:test_key_code(s)];
            [view keyDown:e];
            dispatch_semaphore_signal(done);
        });
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
        usleep(200000);
    }
    return NULL;
}

/* ---------------------------------------------------------------- updates (Sparkle)
 *
 * The app updates itself from the release feed (Info.plist's SUFeedURL: the
 * repository's sparkle/appcast.xml), and installs only an update signed with the key
 * whose public half is SUPublicEDKey. Every update is the slim app: a full app's OBB
 * is in the data folder by then (adopt_bundled_obb). Sparkle asks before it installs,
 * and its "Install and Relaunch" quits the way Cmd-Q does, so the game saves. A build
 * without the two keys has no updater, and a test launch does not start it (the
 * checks keep their state in the app's defaults) unless TL_PVZ2_UPDATES=1. */
static SPUStandardUpdaterController *g_updater;
static bool updates_configured(void)
{
    NSDictionary *plist = NSBundle.mainBundle.infoDictionary;
    return [plist[@"SUFeedURL"] length] && [plist[@"SUPublicEDKey"] length];
}
static void start_updater(void)
{
    if (g_updater || !updates_configured()) return;
    const char *force = getenv("TL_PVZ2_UPDATES");
    if (background_launch() && !(force && !strcmp(force, "1"))) return;
    g_updater = [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES
                                                              updaterDelegate:nil userDriverDelegate:nil];
    tl_log_line("macos: updates from %s, automatic checks %s",
                [NSBundle.mainBundle.infoDictionary[@"SUFeedURL"] UTF8String],
                g_updater.updater.automaticallyChecksForUpdates ? "on" : "off");
}

@interface PVZ2Delegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(strong) NSWindow *window;
@property(strong) NSTimer *statsTimer;
@end@implementation PVZ2Delegate
/* The cancel button: the window goes back to the game's shape -- where it was
 * the last time it had it, or else the same width in that shape. In the fixed
 * mode the window keeps the game's shape until the game restarts (the shape
 * chosen in the settings is kept for then). */
- (void)keepGameShape:(id)sender
{
    (void)sender;
    NSWindow *w = g_window;
    if (!w || (w.styleMask & NSWindowStyleMaskFullScreen) || g_layout_h <= 0) return;
    double a = (double)g_layout_w / g_layout_h;
    tl_log_line("macos: the window goes back to the game's shape (%.3f:1)", a);
    if (!adapt_mode()) {
        g_fixed_lock = a;
        w.contentAspectRatio = NSMakeSize(round(a * 1000), 1000);
    }
    NSSize gs = [w contentRectForFrameRect:g_good_frame].size;
    if (!NSIsEmptyRect(g_good_frame) && gs.height > 0 && fabs(gs.width / gs.height / a - 1.0) <= 0.02)
        [w setFrame:g_good_frame display:YES animate:YES];
    else
        window_to_aspect(w, a);
}

/* The fit button: the game restarts in the window's shape. The game saves on
 * the way out (applicationWillTerminate), the new copy waits for the old one
 * to let go of the data directory, and it opens where this window is. */
- (void)restartToFit:(id)sender
{
    (void)sender;
    if (!g_window) return;
    NSRect f = g_window.frame;
    tl_log_line("macos: restarting the game in the window's shape (%.0fx%.0f points)",
                [g_window contentRectForFrameRect:f].size.width,
                [g_window contentRectForFrameRect:f].size.height);
    NSWorkspaceOpenConfiguration *cfg = [NSWorkspaceOpenConfiguration configuration];
    cfg.createsNewApplicationInstance = YES;
    cfg.activates = !background_launch();
    cfg.arguments = @[ @"--relaunch",
                       [NSString stringWithFormat:@"--frame=%.0f,%.0f,%.0f,%.0f",
                        f.origin.x, f.origin.y, f.size.width, f.size.height] ];
    /* A test's own settings carry over; its script ran here and does not run again. */
    NSMutableDictionary *env = [NSProcessInfo.processInfo.environment mutableCopy];
    if (env[@"TL_PVZ2_TAP"]) env[@"TL_PVZ2_TAP"] = @"";
    cfg.environment = env;
    /* The copies already running (another data directory is another game, and
     * may be the player's own): the new copy is the one that is not among them. */
    NSString *bundleID = NSBundle.mainBundle.bundleIdentifier;
    NSMutableSet<NSNumber *> *before = [NSMutableSet set];
    for (NSRunningApplication *a in [NSRunningApplication runningApplicationsWithBundleIdentifier:bundleID])
        [before addObject:@(a.processIdentifier)];
    [[NSWorkspace sharedWorkspace] openApplicationAtURL:NSBundle.mainBundle.bundleURL
                                          configuration:cfg
                                      completionHandler:^(NSRunningApplication *app, NSError *err) {
        if (!app) tl_log_line("macos: the restart did not start: %s", err.localizedDescription.UTF8String);
    }];
    /* The system reports back only once the new copy has finished starting, and
     * the new copy cannot start until this one has let go of the data directory
     * -- so this one leaves as soon as the new process is there, not when the
     * report comes. If no new process shows up, the game stays as it is. */
    __block int tries = 0;
    __block void (^check)(void);
    __weak __block void (^weak_check)(void);
    check = ^{
        for (NSRunningApplication *a in [NSRunningApplication runningApplicationsWithBundleIdentifier:bundleID]) {
            if (![before containsObject:@(a.processIdentifier)] && !a.terminated) {
                tl_log_line("macos: the new copy is pid %d; this one saves and leaves", a.processIdentifier);
                [NSApp terminate:nil];
                return;
            }
        }
        if (++tries > 100) {
            tl_log_line("macos: the restart's new copy never appeared; the game carries on");
            return;
        }
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), weak_check);
    };
    weak_check = check;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), check);
}
/* A person resizing the game's window in the adapt mode: kept to the shapes the
 * game can take (adapt_clamp). The letterbox mode takes any shape. */
- (void)windowWillStartLiveResize:(NSNotification *)n
{
    if (n.object != g_window) return;
    g_drag_from = [g_window contentRectForFrameRect:g_window.frame].size;
    g_drag_axes = 0;
}
- (void)windowDidEndLiveResize:(NSNotification *)n
{
    if (n.object != g_window) return;
    g_drag_from = NSZeroSize;
    g_drag_axes = 0;
}
- (NSSize)windowWillResize:(NSWindow *)w toSize:(NSSize)frameSize
{
    if (w != g_window || !adapt_mode()) return frameSize;
    NSSize c = [w contentRectForFrameRect:(NSRect){w.frame.origin, frameSize}].size;
    NSSize was = [w contentRectForFrameRect:w.frame].size;
    NSSize n = adapt_clamp(c, was);
    if (NSEqualSizes(n, c)) return frameSize;
    return [w frameRectForContentRect:(NSRect){NSZeroPoint, n}].size;
}
- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    start_updater();
    /* A slim app's first start: the game's data first, then the game. */
    if (!g_obb_download && obb_download_needed()) {
        tl_log_line("macos: the game's data (OBB) is not here yet: it is downloaded first");
        g_obb_download = [[PVZ2ObbDownload alloc] initWithDone:^{
            [self applicationDidFinishLaunching:notification];
        }];
        [g_obb_download start];
        return;
    }
    /* The settings window writes the app's defaults and nothing else: the host
     * watches them, and a change of the resize mode acts at once. (The keys are
     * read when a key is pressed, so they need nothing.) */
    {
        static bool was_adapt;
        static NSString *was_aspect;
        was_adapt = adapt_mode();
        was_aspect = [[NSUserDefaults standardUserDefaults] stringForKey:kFixedAspect];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSUserDefaultsDidChangeNotification
                                                          object:nil queue:NSOperationQueue.mainQueue
                                                      usingBlock:^(NSNotification *note) {
            (void)note;
            [g_view followDisplayScale];
            /* the settings window's "Check for updates automatically" */
            id checks = [[NSUserDefaults standardUserDefaults] objectForKey:@"SUEnableAutomaticChecks"];
            if (g_updater && checks && [checks boolValue] != g_updater.updater.automaticallyChecksForUpdates)
                g_updater.updater.automaticallyChecksForUpdates = [checks boolValue];
            bool now = adapt_mode();
            NSString *aspect = [[NSUserDefaults standardUserDefaults] stringForKey:kFixedAspect];
            if (now == was_adapt && (aspect == was_aspect || [aspect isEqualToString:was_aspect])) return;
            was_adapt = now;
            was_aspect = aspect;
            resize_mode_changed();
        }];
    }
    (void)notification;

    /* The mode, and where it came from. The mode decides how the game is drawn,
     * so it must be in the log: an app with no preference at all is in the
     * letterbox mode (the safe one), and a test can see that here. */
    {
        NSString *stored = [[NSUserDefaults standardUserDefaults] stringForKey:kResizeMode];
        tl_log_line("macos: the resize mode is %s (%s)",
                    adapt_mode() ? "adapt" : "fixed",
                    stored.length ? stored.UTF8String : "the default: no preference is set");
    }

    /* The window is the game's own 1748x804, at the display's scale where that
     * fits, and as large as the screen allows where it does not: the drawable is
     * always 1748x804 (that is what the driver renders into) and the window is
     * scaled, so the picture is always the game's own resolution. */
    NSScreen *screen = [NSScreen mainScreen];
    CGFloat scale = screen.backingScaleFactor;
    NSRect visible = screen.visibleFrame;
    CGFloat w = surfaceWidth / scale, h = surfaceHeight / scale;
    CGFloat maxW = visible.size.width - 32, maxH = visible.size.height - 64;
    if (w > maxW) { w = maxW; h = w * (CGFloat)surfaceHeight / surfaceWidth; }
    if (h > maxH) { h = maxH; w = h * (CGFloat)surfaceWidth / surfaceHeight; }
    /* The first start: most of the display's width (the shape comes after, from
     * the mode). Later starts put the window back where it was. */
    if (w < visible.size.width * 0.7) { w = round(visible.size.width * 0.7); h = round(w / 1.6); }
    NSRect frame = NSMakeRect(0, 0, w, h);
    if (background_launch()) {
        /* Out of the way: the top left corner of the visible area, and small when
         * the game can be small -- in the letterbox mode the picture is scaled
         * into the window, so the window's size is free. In the adapt mode the
         * window's size *is* the game's size, so it stays what it is.
         *
         * The top and not the bottom, because a window at the bottom cannot grow
         * upwards: macOS keeps its corner and clips the height, and a test that
         * resizes the window would get a window it did not ask for. */
        CGFloat bw = 874, bh = 402;
        frame = NSMakeRect(visible.origin.x + 8,
                            visible.origin.y + visible.size.height - bh - 8, bw, bh);
    }

    self.window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"PvZ2 Reflourished";
    self.window.delegate = self;
    self.window.releasedWhenClosed = NO;
    /* The smallest window. In the letterbox mode the game keeps its own shape,
     * so a window much smaller than it would only be black bars; in the adapt
     * mode the game lays its UI out for the window it is in, so the window can
     * be small, and of any shape. */
    /* Where the window was. A restart to fit the window opens exactly where the
     * old window was; otherwise the window comes back where the player left it.
     * (A test's window is never remembered: it would move the player's.) */
    bool placed = false;
    if (g_relaunch && !NSIsEmptyRect(g_relaunch_frame)) {
        [self.window setFrame:g_relaunch_frame display:NO];
        placed = true;
    } else if (!background_launch()) {
        placed = [self.window setFrameUsingName:@"PVZ2GameWindow"];
    }
    if (!background_launch()) [self.window setFrameAutosaveName:@"PVZ2GameWindow"];
    /* A test can start the window on a given display (TL_PVZ2_TEST_SCREEN=n). */
    if (background_launch() && getenv("TL_PVZ2_TEST_SCREEN")) {
        int n = atoi(getenv("TL_PVZ2_TEST_SCREEN"));
        if (n >= 0 && n < (int)NSScreen.screens.count) {
            NSRect v = NSScreen.screens[n].visibleFrame;
            [self.window setFrameTopLeftPoint:NSMakePoint(v.origin.x + 40, NSMaxY(v) - 40)];
        }
    }
    g_window = self.window;
    apply_window_rules();
    /* The display the window ended up on decides the scale. */
    scale = render_scale(self.window);
    {
        NSSize content = [self.window contentRectForFrameRect:self.window.frame].size;
        w = content.width; h = content.height;
        frame = NSMakeRect(0, 0, w, h);
    }
    /* The game picks its art once, when it starts, from the size it starts in:
     * the HD set (1536 lines) at 750 or 804 pixels tall, the standard one
     * (768) at 551 -- a small window on a display that is not Retina. So it
     * starts at least the game's own 804 pixels tall, and goes to the
     * display's own scale once it is past its start (g_starting); the art it
     * picked stays for the whole session. */
    bool launch_boost = h > 0 && h * scale < surfaceHeight;
    if (launch_boost) {
        scale = surfaceHeight / h;
        tl_log_line("macos: the game starts at %.2fx so that it picks its HD art", scale);
    }
    PVZ2View *view = [[PVZ2View alloc] initWithFrame:frame];
    g_view = view;
    view.wantsLayer = YES;
    CAMetalLayer *layer = [CAMetalLayer layer];
    /* RGBA, not BGRA: ANGLE's Metal back end renders into the layer's texture and
     * reads it back the way it is stored, so a BGRA layer hands the reader a frame
     * with its red and blue swapped -- and the EGL layer's sun finder (which looks
     * for yellow) never finds the sun on a window surface. */
    layer.pixelFormat = MTLPixelFormatRGBA8Unorm;
    layer.framebufferOnly = YES;
    layer.contentsScale = scale;
    layer.drawableSize = CGSizeMake(surfaceWidth, surfaceHeight);
    view.layer.backgroundColor = NSColor.blackColor.CGColor;
    [view.layer addSublayer:layer];
    view.metal = layer;
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    /* The surface: the window's own pixels, in the shape the window has now --
     * the game lays itself out for it. */
    int surfW = (int)lround(w * scale), surfH = (int)lround(h * scale);
    /* The shape the game will lay itself out for. */
    g_layout_w = surfW; g_layout_h = surfH;
    atomic_store(&g_surface_w, surfW);
    atomic_store(&g_surface_h, surfH);
    tl_log_line("macos: the game is laid out for %dx%d (%s)", surfW, surfH,
                g_relaunch ? "restarted to fit the window" : "at launch");
    [view applyPresentation];
    {
        NSDate *started_at = [NSDate date];
        [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:YES block:^(NSTimer *timer) {
            bool done = tl_pvz2_started();
            if (!done && -started_at.timeIntervalSinceNow < 20) return;
            [timer invalidate];
            g_starting = false;
            tl_log_line("macos: the game is past its start (%s, %.1f s); the window's size is the game's again",
                        done ? "the driver says so" : "the 20 s limit", -started_at.timeIntervalSinceNow);
            [g_view followDisplayScale];
            [g_view applyPresentation];
        }];
    }
    g_window = self.window;
    self.window.contentView = view;
    if (!background_launch() && !placed) [self.window center];
    if (background_launch()) {
        /* Shown, small, in the corner, and without taking the focus: the person
         * using the Mac keeps their keyboard and their window (the window is
         * ordered in front of theirs, which is the price of a visible window --
         * it is 640 points wide, in the top left corner, and it can be closed
         * with a click). */
        [self.window orderFrontRegardless];
    } else {
        [self.window makeKeyAndOrderFront:nil];
        [self.window makeFirstResponder:view];
        [NSApp activateIgnoringOtherApps:YES];
    }

    /* The guest's keyboard. On iOS the host has to invent one; here the hardware is
     * the keyboard, and this is what tells the driver that the guest asked for it. */
    tl_pvz2_set_keyboard_hook(keyboard_shown);

    /* The display's own maximum: the game's "Target 60 FPS" switch asks the
     * host for a rate, and the host answers with what the screen can show --
     * 120 on a ProMotion screen, 60 on an ordinary one. */
    /* The display the window is on (not the main one: with two displays they
     * differ, and a 175 Hz external one is not the built-in 120 Hz one), and
     * again whenever the window is moved to another display. */
    NSScreen *on = self.window.screen ?: NSScreen.mainScreen;
    int top = (int)on.maximumFramesPerSecond;
    if (top > 0) tl_pvz2_set_max_fps(top);
    tl_log_line("macos: the display can do %d fps, and that is what the game's setting asks for", top);
    [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidChangeScreenNotification
                                                      object:self.window queue:nil
                                                  usingBlock:^(NSNotification *note) {
        NSWindow *w = note.object;
        int fps = (int)w.screen.maximumFramesPerSecond;
        tl_log_line("macos: the window is on another display (%d fps)", fps);
        if (fps > 0) tl_pvz2_set_max_fps(fps);
    }];

    config = (tl_ga_config){ .apk_path = g_apk, .data_dir = g_data,
        .package_name = "com.ea.game.pvz2_rfl", .width = surfW, .height = surfH,
        .metal_layer = (__bridge void *)layer, .angle_egl = g_egl,
        .angle_gles = g_gles, .frame_dir = NULL, .frame_every = 0 };
    /* A test can ask the app to write the frames it presents to a directory --
     * the picture of the game, from inside the app, without the screen being
     * involved at all. That is the way to see the game when its window is
     * behind other windows. */
    const char *frame_dir = getenv("TL_PVZ2_FRAMES");
    if (frame_dir && *frame_dir) {
        /* The directory has to exist: the runtime writes the frames into it
         * and does not create it. */
        NSString *dir = [NSString stringWithUTF8String:frame_dir];
        if (dir) mkdirs(dir);
        config.frame_dir = frame_dir;
        /* Negative means "the latest frame only": the runtime writes latest.bmp
         * and replaces it, so a test always finds the newest picture. */
        int every = getenv("TL_PVZ2_FRAME_EVERY") ? atoi(getenv("TL_PVZ2_FRAME_EVERY")) : 30;
        if (every < 1) every = 1;      /* zero would turn the "latest only" mode off */
        config.frame_every = -every;
        tl_log_line("macos: the frames go to %s, every %d", frame_dir, every);
    }
    /* The native shovel (experimental, off by default). */
    tl_pvz2_set_native_shovel(native_shovel_mode());
    /* The native seed pick: the game's own press. */
    tl_pvz2_set_seed_pick(native_seed_pick_mode());

    /* The guest's own memory warning. The game's Java has no memory callback
     * at all, so the host is the activity: the host is told about the memory
     * pressure and the guest's own function is called on the game's thread.
     * Off by default (the guest's function is experimental); the environment
     * turns it on. */
    const char *lowmem = getenv("TL_PVZ2_LOW_MEMORY");
    tl_pvz2_set_low_memory_warning(lowmem && lowmem[0] == '1');
    dispatch_source_t mem = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0, 0, dispatch_get_main_queue());
    if (mem) {
        dispatch_source_set_event_handler(mem, ^{
            tl_log_line("macos: the memory pressure was reported");
            tl_pvz2_request_low_memory();
        });
        dispatch_resume(mem);
    }

    /* The test switch: the game keeps running when the app is not the
     * frontmost one. There is no pause here -- the app never stops the game
     * for being in the background -- and this says so in the log, so a test can
     * see that the game kept presenting while another application had the
     * focus. */
    const char *keep_running = getenv("TL_KEEP_RUNNING_IN_BACKGROUND");
    if (keep_running && keep_running[0] == '1')
        tl_log_line("macos: the game keeps running in the background");

    /* A test can ask the app to walk its own window through a list of shapes --
     * "TL_PVZ2_RESIZE=800x600,600x800,400x300". The app resizes itself, so
     * nothing has to reach into it from outside, and nothing else on the
     * machine is touched. Each shape is held for a few seconds, and the log
     * says which one it is. */
    const char *shapes = getenv("TL_PVZ2_RESIZE");
    if (shapes && *shapes) {
        NSMutableArray *sizes = [NSMutableArray array];
        for (NSString *piece in [@(shapes) componentsSeparatedByString:@","]) {
            NSArray *parts = [piece componentsSeparatedByString:@"x"];
            if (parts.count == 2) {
                CGFloat sw = [parts[0] doubleValue], sh = [parts[1] doubleValue];
                if (sw > 0 && sh > 0) [sizes addObject:NSStringFromSize(CGSizeMake(sw, sh))];
            }
        }
        __block NSUInteger index = 0;
        [NSTimer scheduledTimerWithTimeInterval:8 repeats:YES block:^(NSTimer *timer) {
            (void)timer;
            if (index >= sizes.count) return;
            CGSize s = NSSizeFromString(sizes[index]);
            index++;
            [self.window setContentSize:s];
            /* The window lays its content view out itself; the view's layout
             * is where the picture is fitted into the new shape. */
            tl_log_line("macos: the window is now %.0fx%.0f points", s.width, s.height);
        }];
    }
    tl_log_line("macos: window %gx%g points, surface %dx%d, data %s", w, h,
                surfW, surfH, config.data_dir);
    tl_log_line("macos: apk %s", config.apk_path);
    tl_log_line("macos: egl [%s] gles [%s]", config.angle_egl ? config.angle_egl : "(null)",
                config.angle_gles ? config.angle_gles : "(null)");

    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t thread;
    int error = pthread_create(&thread, &attr, launch_game, NULL);
    pthread_attr_destroy(&attr);
    if (error) { tl_log_line("macos: cannot create launch thread (%d)", error); [NSApp terminate:nil]; return; }
    pthread_detach(thread);

    if (getenv("TL_PVZ2_TAP")) {
        static pthread_t tt; pthread_create(&tt, NULL, tapper, NULL);
    }
    if (getenv("TL_PVZ2_KEYSTROKE")) {
        static pthread_t kt; pthread_create(&kt, NULL, key_stroker, (__bridge void *)view);
    }
    /* The preferences window, for a test that needs to see it. */
    if (getenv("TL_PVZ2_PREFS")) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{ [PVZ2Prefs show]; });
    }

    /* The display's sleep. The system posts these when the screen goes to
     * sleep and when it wakes; the app pauses the game and goes on with it.
     * The workspace posts them to the default centre, which is where the
     * app listens. */
    NSNotificationCenter *centre = [NSNotificationCenter defaultCenter];
    [centre addObserver:self selector:@selector(willSleep:)
                   name:NSWorkspaceWillSleepNotification object:nil];
    [centre addObserver:self selector:@selector(didWake:)
                   name:NSWorkspaceDidWakeNotification object:nil];

    __block unsigned long previous;
    __block double previous_time = 0;
    double start_time = [NSDate timeIntervalSinceReferenceDate];
    self.statsTimer = [NSTimer scheduledTimerWithTimeInterval:5 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        unsigned long frames = tl_pvz2_frames();
        /* The first line has no earlier count to compare with: the average
         * since the start is the honest thing to say. (Dividing by the count
         * that is not there yet produced a joke of a number.) */
        double now = [NSDate timeIntervalSinceReferenceDate];
        if (previous_time <= 0) {
            double since = now - start_time;
            previous_time = now;
            previous = frames;
            tl_log_line("macos: frames=%lu, last 5s=%.1f fps", frames, since > 0 ? frames / since : 0.0);
            return;
        }
        tl_log_line("macos: frames=%lu, last 5s=%.1f fps", frames, (frames - previous) / 5.0);
        previous_time = now;
        previous = frames;
    }];
}
/* The app is not on the screen any more: it is hidden (Cmd-H), its window is
 * minimized, or the display has gone to sleep. The game pauses, the way it
 * does on Android when the activity is stopped, and it goes on when the app
 * is back. */
- (void)applicationDidHide:(NSNotification *)notification
{
    (void)notification;
    if (atomic_exchange(&g_away_hidden, 1)) return;   /* already hidden */
    if (away_count() == 1) app_suspended("the app was hidden");  /* the first reason */
}
- (void)applicationDidUnhide:(NSNotification *)notification
{
    (void)notification;
    if (!atomic_exchange(&g_away_hidden, 0)) return;
    app_resumed("the app is back");
}
- (void)windowDidMiniaturize:(NSNotification *)notification
{
    if (notification.object != g_window) return;
    if (atomic_exchange(&g_away_minimized, 1)) return;
    if (away_count() == 1) app_suspended("the window was minimized");
}
- (void)windowDidDeminiaturize:(NSNotification *)notification
{
    if (notification.object != g_window) return;
    if (!atomic_exchange(&g_away_minimized, 0)) return;
    app_resumed("the window was restored");
}
- (void)willSleep:(NSNotification *)notification
{
    (void)notification;
    if (atomic_exchange(&g_away_asleep, 1)) return;
    if (away_count() == 1) app_suspended("the display is going to sleep");
}
- (void)didWake:(NSNotification *)notification
{
    (void)notification;
    if (!atomic_exchange(&g_away_asleep, 0)) return;
    app_resumed("the display is awake again");
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { (void)app; return !close_keeps_game(); }
/* The Dock icon clicked (or the app opened again) while the game's window is
 * closed and the game kept: the window comes back and the game goes on. */
- (BOOL)applicationShouldHandleReopen:(NSApplication *)app hasVisibleWindows:(BOOL)visible
{
    (void)app; (void)visible;
    [self showGameWindow:nil];
    return NO;
}
/* Window > PvZ2 Reflourished (Cmd-0): the game's window, closed or not. */
- (void)showGameWindow:(id)sender
{
    (void)sender;
    if (!g_window) return;
    [g_window makeKeyAndOrderFront:nil];
    if (atomic_exchange(&g_away_closed, 0)) app_resumed("the window is open again");
}
/* The game does not stop when the app is not the frontmost one: there is no
 * pause here, and the log says when the focus moved, so a test can see that
 * the game kept presenting while another application had the keyboard. */
- (void)applicationDidResignActive:(NSNotification *)notification
{
    (void)notification;
    tl_log_line("macos: the app is not frontmost; the game keeps running");
}
- (void)applicationDidBecomeActive:(NSNotification *)notification
{
    (void)notification;
    tl_log_line("macos: the app is frontmost");
}
/* The app is hidden (Cmd-H) and shown again. That is the app going away and
 * coming back, the way the iPad's app switcher is, so the guest is told both
 * times: it saves on the way out, and it lays its window out again on the way
 * back. The game stops drawing while it is hidden, because the guest's surface
 * is gone for that time -- the same thing the iPad does. */
/* The hide and the unhide themselves are handled above, with the other ways
 * the app can go away (a minimized window, a display that sleeps): the guest
 * is told once, when the first reason comes, and it is told it is back when
 * the last reason is gone. */
/* Cmd-, : the preferences. */
/* App menu > Check for Updates…, and the settings window's Check Now */
- (void)checkForUpdates:(id)sender { [g_updater checkForUpdates:sender]; }
- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if (item.action == @selector(checkForUpdates:)) return g_updater.updater.canCheckForUpdates;
    return YES;
}
- (void)showPreferences:(id)sender
{
    (void)sender;
    /* The settings window is SwiftUI (tools/pvz2-macos-settings.swift); the old
     * AppKit panel is the fallback for a build without it. */
    Class settings = NSClassFromString(@"PVZ2Settings");
    if (settings) [settings performSelector:@selector(show)];
    else [PVZ2Prefs show];
}
- (void)applicationWillTerminate:(NSNotification *)notification
{
    (void)notification;
    /* The guest is an Android game: it expects to be told that the app is going
     * away, and it saves on that event. The call waits for the guest, so the
     * save is on the disk before the process leaves.
     *
     * The drawing is stopped *first*: the guest is told that its surface is
     * gone, and a frame drawn after that is a frame drawn into a surface the
     * guest has torn down. That is what crashed the app on the way out, and
     * the iPad host does the same in the same order. */
    tl_pvz2_set_paused(true);
    tl_pvz2_lifecycle_background();
    tl_log_line("macos: closed by the user, %lu frames presented", tl_pvz2_frames());
}
/* Closing the game window (the red button, Cmd-W) quits the app: the game is
 * not a document, and the macOS way for a one-window app is to quit with it.
 * The setting "When the window is closed: keep the game paused" keeps the game
 * instead: the window goes away, the game pauses the way it does when the app
 * is hidden, and the Dock icon (or Window > PvZ2 Reflourished) brings it back
 * where it was. */
- (void)windowWillEnterFullScreen:(NSNotification *)n { (void)n; g_fs_moving = true; }
- (void)windowWillExitFullScreen:(NSNotification *)n { (void)n; g_fs_moving = true; }
- (void)windowDidEnterFullScreen:(NSNotification *)n { (void)n; g_fs_moving = false; [g_view applyPresentation]; }
- (void)windowDidExitFullScreen:(NSNotification *)n { (void)n; g_fs_moving = false; [g_view applyPresentation]; }
- (void)windowDidFailToEnterFullScreen:(NSWindow *)w { (void)w; g_fs_moving = false; [g_view applyPresentation]; }
- (void)windowDidFailToExitFullScreen:(NSWindow *)w { (void)w; g_fs_moving = false; [g_view applyPresentation]; }
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    if (sender == g_window && close_keeps_game()) {
        [sender orderOut:nil];
        if (!atomic_exchange(&g_away_closed, 1) && away_count() == 1)
            app_suspended("the window was closed (the game is kept)");
        return NO;
    }
    [self askToQuit:nil];
    return NO;
}
/* Quit Reflourished (Cmd-Q) and the window's close button when it quits: the
 * player is asked first, so a slip of the hand does not end a level. The
 * setting "Ask before quitting" turns it off, and so does the alert's own
 * "Don't ask again". A quit from outside the app -- the Dock's menu, a
 * logout, a restart -- is not asked about. */
- (void)askToQuit:(id)sender
{
    (void)sender;
    static bool asking;
    if (!confirm_quit() || background_launch()) { [NSApp terminate:nil]; return; }
    if (asking) return;
    asking = true;
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"Quit PvZ2 Reflourished?";
    alert.informativeText = @"The game saves before it quits.";
    [alert addButtonWithTitle:@"Quit"];
    [alert addButtonWithTitle:@"Cancel"];
    alert.showsSuppressionButton = YES;
    alert.suppressionButton.title = @"Don't ask again";
    void (^answered)(NSModalResponse) = ^(NSModalResponse response) {
        asking = false;
        if (response != NSAlertFirstButtonReturn) return;
        if (alert.suppressionButton.state == NSControlStateValueOn)
            [[NSUserDefaults standardUserDefaults] setBool:NO forKey:kConfirmQuit];
        [NSApp terminate:nil];
    };
    if (g_window.visible && !g_window.miniaturized)
        [alert beginSheetModalForWindow:g_window completionHandler:answered];
    else
        answered([alert runModal]);
}
@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        for (int i = 1; i < argc; i++) {
            double x, y, fw, fh;
            if (!strcmp(argv[i], "--relaunch")) g_relaunch = true;
            else if (sscanf(argv[i], "--frame=%lf,%lf,%lf,%lf", &x, &y, &fw, &fh) == 4)
                g_relaunch_frame = NSMakeRect(x, y, fw, fh);
        }
        /* The log goes next to the data, so it survives the app and can be read
         * after a crash. TL_PVZ2_LOG overrides it (the harness's own name for it). */
        NSString *data = [NSString stringWithUTF8String:
            (getenv("TL_DATA") && *getenv("TL_DATA")) ? getenv("TL_DATA")
            : support_dir().UTF8String];
        dataPath = [data stringByStandardizingPath];
        mkdirs(dataPath);
        snprintf(g_data, sizeof g_data, "%s", dataPath.UTF8String);

        /* One instance per data directory: the game does not lock its saves. */
        /* A restart to fit the window: the copy that started this one is saving
         * and leaving, and it lets go of the data directory when it is gone. */
        bool claimed = claim_instance(dataPath);
        for (int i = 0; g_relaunch && !claimed && i < 300; i++) {
            usleep(100000);
            claimed = claim_instance(dataPath);
        }
        if (!claimed) {
            fprintf(stderr, "another copy is already running with this data directory; "
                            "bringing it to the front\n");
            activate_other_instance();
            return 0;
        }
        const char *log = getenv("TL_PVZ2_LOG");
        NSString *logPath = log ? [NSString stringWithUTF8String:log]
                                : [dataPath stringByAppendingPathComponent:@"pvz2.log"];
        /* Each launch starts its own log. The one before it stays as pvz2.previous.log
         * (after a crash, that is the run that crashed), so the log never grows past
         * two runs. A restart to fit the window is the same session and keeps writing
         * to it, and a test's TL_PVZ2_LOG is the test's to manage. */
        if (!log && !g_relaunch)
            rename(logPath.UTF8String,
                   [dataPath stringByAppendingPathComponent:@"pvz2.previous.log"].UTF8String);
        freopen(logPath.UTF8String, "a", stderr);
        setvbuf(stderr, NULL, _IOLBF, 0);

        import_old_settings();
        adopt_bundled_obb();

        /* The APK: in the bundle, or wherever the environment says. */
        NSString *apk = nil;
        if (getenv("TL_PVZ2_APK") && *getenv("TL_PVZ2_APK"))
            apk = [NSString stringWithUTF8String:getenv("TL_PVZ2_APK")];
        else apk = bundle_dir(@"Game/pvz2.apk");
        if (![NSFileManager.defaultManager fileExistsAtPath:apk]) {
            fprintf(stderr, "No game APK at %s\n", apk.UTF8String);
            return 2;
        }
        apkPath = [apk stringByStandardizingPath];
        snprintf(g_apk, sizeof g_apk, "%s", apkPath.UTF8String);

        /* ANGLE: the EGL the driver needs. In the bundle, or wherever the
         * environment says. */
        NSString *egl = getenv("TL_ANGLE_EGL") && *getenv("TL_ANGLE_EGL")
            ? [NSString stringWithUTF8String:getenv("TL_ANGLE_EGL")]
            : bundle_dir(@"Frameworks/libEGL.dylib");
        NSString *gles = getenv("TL_ANGLE_GLES") && *getenv("TL_ANGLE_GLES")
            ? [NSString stringWithUTF8String:getenv("TL_ANGLE_GLES")]
            : bundle_dir(@"Frameworks/libGLESv2.dylib");
        if (![NSFileManager.defaultManager fileExistsAtPath:egl]) {
            fprintf(stderr, "No ANGLE at %s\n", egl.UTF8String);
            return 2;
        }
        eglPath = egl; glesPath = gles;
        snprintf(g_egl, sizeof g_egl, "%s", eglPath.UTF8String);
        snprintf(g_gles, sizeof g_gles, "%s", glesPath.UTF8String);

        /* The route: the converted libraries in the bundle, if they are there. */
        NSString *aot = stage_aot_dylibs();
        if (aot) tl_log_line("macos: JIT-free: converted guest libraries in %s", aot.UTF8String);
        else tl_log_line("macos: no converted guest libraries in the bundle; the guest's "
                         "ELF will be translated, which needs executable memory");
        NSString *cdn = stage_cdn_tar();
        if (cdn) tl_log_line("macos: the CDN fallback archive is %s", cdn.UTF8String);

        tl_ld_set_verbosity(getenv("TL_VERBOSE") ? atoi(getenv("TL_VERBOSE")) : 1);
        tl_jni_set_trace(getenv("TL_JNI_TRACE") ? atoi(getenv("TL_JNI_TRACE")) : 1);
        static unsigned char stack[65536];
        stack_t signalStack = { .ss_sp = stack, .ss_size = sizeof(stack) }; sigaltstack(&signalStack, NULL);
        struct sigaction action = { .sa_sigaction = on_crash, .sa_flags = SA_SIGINFO | SA_ONSTACK };
        sigemptyset(&action.sa_mask);
        int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT };
        for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) sigaction(signals[i], &action, NULL);

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        /* The standard menus, so the standard keys work in both windows:
         * Cmd-H hides the app (the game pauses), Cmd-W closes the window that
         * has the focus (the game's quits the app, see windowShouldClose), Cmd-M
         * minimizes (the game pauses; the settings window cannot be minimized,
         * as macOS's settings windows cannot). */
        NSMenu *menu = [NSMenu new];
        NSMenuItem *item = [NSMenuItem new]; [menu addItem:item];
        NSMenu *appMenu = [NSMenu new];
        if (updates_configured()) {
            [appMenu addItemWithTitle:@"Check for Updates…" action:@selector(checkForUpdates:) keyEquivalent:@""];
            [appMenu addItem:[NSMenuItem separatorItem]];
        }
        [appMenu addItemWithTitle:@"Settings…" action:@selector(showPreferences:) keyEquivalent:@","];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:@"Hide Reflourished" action:@selector(hide:) keyEquivalent:@"h"];
        [[appMenu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"]
            setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
        [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:@"Quit Reflourished" action:@selector(askToQuit:) keyEquivalent:@"q"];
        item.submenu = appMenu;
        NSMenuItem *fileItem = [NSMenuItem new]; [menu addItem:fileItem];
        NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
        [fileMenu addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
        fileItem.submenu = fileMenu;
        NSMenuItem *windowItem = [NSMenuItem new]; [menu addItem:windowItem];
        NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
        [windowMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
        [windowMenu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
        [windowMenu addItem:[NSMenuItem separatorItem]];
        [windowMenu addItemWithTitle:@"PvZ2 Reflourished" action:@selector(showGameWindow:) keyEquivalent:@"0"];
        [windowMenu addItem:[NSMenuItem separatorItem]];
        [windowMenu addItemWithTitle:@"Bring All to Front" action:@selector(arrangeInFront:) keyEquivalent:@""];
        windowItem.submenu = windowMenu;
        app.mainMenu = menu;
        app.windowsMenu = windowMenu;   /* macOS adds the window list and Enter Full Screen */
        PVZ2Delegate *delegate = [PVZ2Delegate new]; app.delegate = delegate;
        [app run];
    }
    return 0;
}
