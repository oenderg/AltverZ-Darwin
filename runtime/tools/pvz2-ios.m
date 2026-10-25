/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Minimal iOS host for the PvZ2: Reflourished driver.
 *
 * This is the device counterpart of tools/pvz2-window.m: the same driver, the
 * same off-screen-free path through a real CAMetalLayer, but on UIKit and with
 * the iOS executable-memory route. It exists instead of the full Husk app so the
 * game needs only ANGLE -- no QEMU, virglrenderer, MoltenVK or the guest image.
 *
 * Files it reads from the app's Documents folder (visible in Files when
 * UIFileSharingEnabled is set, which this app's Info.plist does):
 *
 *   Documents/pvz2.apk                              the APK
 *   Documents/obb/main.675.com.ea.game.pvz2_rfl.obb the OBB (found beside the APK)
 *
 * Writable state goes to Documents/data. Logs go to Documents/pvz2.log.
 *
 * Executable memory: none. The game's libraries come converted ahead of time into
 * signed Mach-O libraries (Frameworks/libPVZ2.dylib and the rest), so nothing here
 * makes memory executable or writes code; a build without them cannot run, and the
 * log says so.
 *
 * Touches: one finger per touch, phases matching the driver (0 down, 1 move,
 * 3 up, 4 cancel). Landscape only, like the game.
 */
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AVFAudio/AVFAudio.h>

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "husk-tl-audio.h"
#include "husk-tl-pvz2.h"
#include "husk-tl-cdn.h"
#include "husk-tl-viewport.h"
#include "pvz2-keys.h"
#include "pvz2-open-url.h"

static bool open_game_url(const char *url)
{
    return pvz2_open_url(url, ^BOOL(NSURL *target) {
        UIApplication *app = UIApplication.sharedApplication;
        if (![app canOpenURL:target]) return NO;
        /* UIKit completes asynchronously; the JNI result reports accepted submission
         * so the guest can save when opening a link backgrounds the app. */
        [app openURL:target options:@{} completionHandler:^(BOOL success) {
            if (!success) tl_log_line("pvz2: open URL %s failed (iOS completion)", target.absoluteString.UTF8String);
        }];
        return YES;
    });
}

/* ===================== BEGIN AOT-DYLIB-LOADER TEST ============================
 * Delete everything from here to the END marker to revert. Nothing below changes
 * how the game is launched; the test runs before anything else and does not come
 * back.
 *
 * The ahead-of-time route needs one thing answered on the physical device: does a
 * converted Android .so -- an opaque blob of guest arm64 code wrapped in a Mach-O
 * dylib -- dlopen() and *execute* with no debugger attached? This app is signed
 * with an ad-hoc distribution profile (get-task-allow = false), so nothing can
 * JIT here; if the guest code still runs, it ran because the dylib loaded and the
 * linker fixed it up.
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/mman.h>
#include <CommonCrypto/CommonDigest.h>

/* Link-time addresses of the blobs, from libNimble.dylib.facts next to the
 * converted library (code_link / data_link / tcb_link / stubs_link). Every runtime
 * address is compared against these plus one slide. */
#define AOT_CODE_LINK  0x4000ULL
#define AOT_DATA_LINK  0x18000ULL
#define AOT_TCB_LINK   0x19000ULL
#define AOT_STUBS_LINK 0x4d40ULL
#define AOT_STUB_PLT   0x670ULL   /* the guest PLT stub that reaches pthread_key_create */

static uint64_t aot_rd64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t aot_rd32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* "<image>+0x<off>" when v lands inside anything dyld has mapped -- the app, a
 * framework, or a dylib out of the shared cache -- so a bound slot can be shown to
 * hold a real host address rather than the link-time placeholder. dladdr() answers
 * that directly; hand-walking load commands gets the header size wrong (a
 * mach_header is 28 bytes, mach_header_64 is 32, and every image here is 64-bit). */
static int aot_locate(uintptr_t v, char *buf, size_t n)
{
    Dl_info di;
    if (dladdr((const void *)v, &di) && di.dli_fbase) {
        snprintf(buf, n, "%s+0x%lx", di.dli_fname ? di.dli_fname : "(unnamed)",
                 (unsigned long)(v - (uintptr_t)di.dli_fbase));
        return 1;
    }
    return 0;
}

/* The file as it ships, before dyld rebases or binds anything: read-only mmap plus
 * the segment table, so a link-time vaddr can be read straight out of the bytes. */
static const uint8_t *aot_shipped(const char *path, uint64_t va)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(struct mach_header_64)) { close(fd); return NULL; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return NULL;
    const struct mach_header_64 *h = m;
    if (h->magic != MH_MAGIC_64) return NULL;
    const struct load_command *lc = (const void *)(h + 1);
    for (uint32_t k = 0; k < h->ncmds; k++) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)lc;
            if (va >= s->vmaddr && va < s->vmaddr + s->filesize)
                return (const uint8_t *)m + s->fileoff + (va - s->vmaddr);
        }
        lc = (const void *)((const uint8_t *)lc + lc->cmdsize);
    }
    return NULL;
}

static void aot_dylib_test(void)
{
    static const struct { uint64_t off; const char *sym; } kBinds[] = {
        { 0x208, "pthread_key_create" }, { 0x210, "__cxa_finalize" },
        { 0x218, "__stack_chk_fail" },   { 0x220, "pthread_setspecific" },
        { 0x228, "__cxa_atexit" },
    };
    static const struct { uint64_t off; uint64_t host_off; int in_code; const char *want; } kReb[] = {
        { 0x0,   0x6d0, 1, "guest_code+0x6d0" },
        { 0x8,   0x6c0, 1, "guest_code+0x6c0" },
        { 0x238, 0x238, 0, "guest_data+0x238" },
    };
    char reason[256] = "";
    char where[320];
#define AOT_BAD(...) do { if (!reason[0]) snprintf(reason, sizeof reason, __VA_ARGS__); \
                          fprintf(stderr, "AOT: FAIL  " __VA_ARGS__); fputc('\n', stderr); } while (0)

    fprintf(stderr, "AOT: === converted-dylib load test, no debugger attached ===\n");

    /* A process cannot really prove "no debugger" about itself, but its provisioning
     * profile fixes it: get-task-allow = false means no debugger can attach at all,
     * so nothing in this run had a chance to use one -- and no JIT was available
     * either. Read the entitlement out of the profile embedded in the bundle. */
    const char *gta = "no embedded profile";
    NSData *pd = [NSData dataWithContentsOfFile:
                  [NSBundle.mainBundle pathForResource:@"embedded" ofType:@"mobileprovision"]];
    const char *needle = "get-task-allow";
    if (pd.length > 32) {
        const uint8_t *p = pd.bytes, *hit = memmem(p, pd.length, needle, strlen(needle));
        if (hit) {
            size_t rest = pd.length - (size_t)(hit - p) - strlen(needle);
            const uint8_t *tail = (const uint8_t *)hit + strlen(needle);
            rest = rest < 256 ? rest : 256;      /* the value follows the key at once */
            const uint8_t *t = memmem(tail, rest, "<true/>", 7);
            const uint8_t *f = memmem(tail, rest, "<false/>", 8);
            gta = (!t && !f) ? "unreadable" : (t && (!f || t < f) ? "true" : "false");
        } else {
            gta = "not listed";
        }
    }
    fprintf(stderr, "AOT: profile         pid=%d main=%s\n", getpid(),
            [NSBundle.mainBundle executablePath].UTF8String);
    fprintf(stderr, "AOT: profile         embedded.mobileprovision get-task-allow=%s"
                    " (false => no debugger can attach, so no JIT)\n", gta);
    if (strcmp(gta, "false") != 0)
        AOT_BAD("the profile's get-task-allow is %s, not false", gta);

    NSString *dir = NSBundle.mainBundle.privateFrameworksPath;
    if (!dir) dir = [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"Frameworks"];
    NSString *path = [dir stringByAppendingPathComponent:@"libNimble.dylib"];
    fprintf(stderr, "AOT: dylib path      %s (present=%d)\n", path.UTF8String,
            (int)[[NSFileManager defaultManager] fileExistsAtPath:path]);

    /* What the bind slots hold in the file, before dyld gets to them. */
    const uint8_t *shipped = aot_shipped(path.UTF8String, AOT_DATA_LINK);
    for (unsigned i = 0; i < sizeof kBinds / sizeof *kBinds; i++)
        fprintf(stderr, "AOT: shipped data+0x%03llx %-21s 0x%016llx (chained-fixup placeholder)\n",
                kBinds[i].off, kBinds[i].sym, shipped ? aot_rd64(shipped + kBinds[i].off) : 0);

    void *h = dlopen(path.UTF8String, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *e = dlerror();
        fprintf(stderr, "AOT: dlopen FAILED: %s\n", e ? e : "(no dlerror)");
        AOT_BAD("dlopen failed: %s", e ? e : "(no dlerror)");
        goto done;
    }
    fprintf(stderr, "AOT: dlopen          OK (handle=%p)\n", h);

    /* dlsym takes the C name; the Mach-O symbols carry the underscore. */
    uint8_t *code  = (uint8_t *)dlsym(h, "guest_code");
    uint8_t *data  = (uint8_t *)dlsym(h, "guest_data");
    uint8_t *tcb   = (uint8_t *)dlsym(h, "guest_tcb");
    uint8_t *stubs = (uint8_t *)dlsym(h, "guest_stubs");
    if (!code || !data || !tcb) {
        AOT_BAD("the image labels did not resolve (code=%p data=%p tcb=%p)", (void *)code, (void *)data, (void *)tcb);
        goto done;
    }
    fprintf(stderr, "AOT: labels          guest_code=%p guest_data=%p guest_tcb=%p guest_stubs=%p\n",
            (void *)code, (void *)data, (void *)tcb, (void *)stubs);

    intptr_t slide = (intptr_t)((char *)code - (char *)(uintptr_t)AOT_CODE_LINK);
    int same = (char *)data - (char *)(uintptr_t)AOT_DATA_LINK == slide &&
               (char *)tcb  - (char *)(uintptr_t)AOT_TCB_LINK  == slide &&
               (char *)stubs- (char *)(uintptr_t)AOT_STUBS_LINK== slide;
    fprintf(stderr, "AOT: slide           0x%lx  (all four labels agree: %s)\n",
            (long)slide, same ? "yes" : "NO");
    if (!same) AOT_BAD("the four labels did not land at one slide (0x%lx)", (long)slide);

    /* One baked patch. `mrs x0, tpidr_el0` became `adrp x0, <the fake thread block>`,
     * which is page-relative and therefore survives the slide. Decode it the way the
     * hardware does: immlo = bits 30:29, immhi = bits 23:5, imm = immhi:immlo
     * sign-extended from 21 bits, target page = (pc & ~0xFFF) + (imm << 12). */
    {
        uint32_t w = aot_rd32(code + 0x70c);
        int64_t imm = (int64_t)(((((uint32_t)w >> 5) & 0x7FFFF) << 2) | (((uint32_t)w >> 29) & 3));
        if (imm & 0x100000) imm -= 0x200000;
        uintptr_t site = (uintptr_t)(code + 0x70c);
        uintptr_t page = (site & ~(uintptr_t)0xFFF) + (uintptr_t)(imm << 12);
        int hit = (page == ((uintptr_t)tcb & ~(uintptr_t)0xFFF));
        fprintf(stderr, "AOT: adrp patch      guest_code+0x70c word=0x%08x -> page %p  guest_tcb is %p  %s\n",
                w, (void *)page, (void *)tcb, hit ? "MATCH" : "NO MATCH");
        if (!hit) AOT_BAD("the baked adrp at guest_code+0x70c names %p, not guest_tcb %p",
                          (void *)page, (void *)tcb);
    }

    /* Three rebases: dyld's own chain fixups, emitted by ld64 from the `.quad
     * _guest_code + 0x6d0` expressions, applied to the writable data segment. */
    for (unsigned i = 0; i < sizeof kReb / sizeof *kReb; i++) {
        uint64_t got = aot_rd64(data + kReb[i].off);
        uint8_t *base = kReb[i].in_code ? code : data;
        uint64_t want = (uintptr_t)base + kReb[i].host_off;
        int ok = got == want;
        fprintf(stderr, "AOT: rebase          data+0x%03llx = 0x%016llx  want %s = 0x%016llx  %s\n",
                kReb[i].off, (unsigned long long)got, kReb[i].want, (unsigned long long)want,
                ok ? "MATCH" : "NO MATCH");
        if (!ok) AOT_BAD("rebase data+0x%03llx is 0x%016llx, not %s", kReb[i].off,
                         (unsigned long long)got, kReb[i].want);
    }

    /* Five binds: dyld must have replaced the chained-fixup placeholders with the
     * host's own addresses for these symbols. RTLD_DEFAULT lookup is the app's
     * binding for the same symbol, so equality is the real test. */
    for (unsigned i = 0; i < sizeof kBinds / sizeof *kBinds; i++) {
        uint64_t v = aot_rd64(data + kBinds[i].off);
        void *host = dlsym(RTLD_DEFAULT, kBinds[i].sym);
        int mapped = aot_locate((uintptr_t)v, where, sizeof where);
        int ok = mapped && v == (uintptr_t)host;
        fprintf(stderr, "AOT: bind            data+0x%03llx %-21s = 0x%016llx  app &%s = 0x%016lx  %s  %s\n",
                kBinds[i].off, kBinds[i].sym, (unsigned long long)v, kBinds[i].sym,
                (unsigned long)(uintptr_t)host, ok ? "MATCH" : "NO MATCH",
                mapped ? where : "(not in any loaded image)");
        if (!ok) AOT_BAD("bind data+0x%03llx (%s) = 0x%016llx, not the host's 0x%016lx",
                         kBinds[i].off, kBinds[i].sym, (unsigned long long)v,
                         (unsigned long)(uintptr_t)host);
    }

    /* The real test: run guest instructions. guest_code+0x670 is libNimble's own
     * PLT stub -- `adrp x16, <data>; ldr x17, [x16, #0x208]; br x17` -- so calling it
     * executes guest code through a rewritten adrp pair into a slot dyld bound to the
     * host's pthread_key_create. Nothing here is JIT: the code is in the dylib's
     * signed __TEXT and the data it touches is in __DATA. */
    memset(tcb, 0, 0x1000);
    {
        int (*stub)(unsigned *, void *) = (int (*)(unsigned *, void *))(uintptr_t)(code + AOT_STUB_PLT);
        unsigned key = 0;
        fprintf(stderr, "AOT: guest stub      calling guest_code+0x670 (adrp/ldr/br -> pthread_key_create)\n");
        fflush(NULL);
        int rc = stub(&key, NULL);
        int ok = rc == 0 && key != 0;
        fprintf(stderr, "AOT: guest stub      returned rc=%d key=%u  %s\n", rc, key,
                ok ? "GUEST CODE RAN" : "NO");
        if (!ok) AOT_BAD("the guest PLT stub returned rc=%d key=%u", rc, key);
    }

done:
    fprintf(stderr, "AOT-DYLIB-LOAD: %s\n", reason[0] ? reason : "OK");
    fflush(NULL);
    _exit(reason[0] ? 3 : 0);
#undef AOT_BAD
}
/* ====================== END AOT-DYLIB-LOADER TEST ============================= */

static tl_ga_config g_cfg;
static atomic_bool g_ready;
static int g_w, g_h;
static UIWindowScene *g_scene;
static int g_orientation = 4;          /* the guest's vocabulary: 4 landscape, 3 landscape flipped */
static UIDeviceOrientation _last_device_orientation = UIDeviceOrientationUnknown;

/* The guest asks the device which way up it is, in its own vocabulary
 * (from its Java): 4 landscape, 3 landscape turned over, 1 portrait, 2
 * portrait turned over. The guest re-lays-out when the answer changes, so
 * the host has to answer with the truth -- and to tell the guest when the
 * device is turned the other way. */
static int guest_orientation_for(UIInterfaceOrientation io)
{
    switch (io) {
        case UIInterfaceOrientationLandscapeLeft:  return 4;
        case UIInterfaceOrientationLandscapeRight: return 3;
        case UIInterfaceOrientationPortrait:       return 1;
        case UIInterfaceOrientationPortraitUpsideDown: return 2;
        default: return 4;
    }
}
static char g_apk[1024], g_data[1024], g_egl[1024];

/* The translation layer asks Husk's own loader (husk-tl-load.c) for the executable
 * region a debugger granted. The converted libraries need none, so there is none. */
typedef struct { uint8_t *rw_addr; uint8_t *rx_addr; size_t size; } host_dual_mapping;
host_dual_mapping *tl_find_stikdebug_prewarmed(void);
host_dual_mapping *tl_find_stikdebug_prewarmed(void) { return NULL; }

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
    fprintf(stderr, "\nCRASH: signal=%d fault=%p pc=%#llx lr=%#llx\n",
            sig, info->si_addr, (unsigned long long)uc->uc_mcontext->__ss.__pc,
            (unsigned long long)uc->uc_mcontext->__ss.__lr);
    fflush(stderr);
    _exit(128 + sig);
}

/* The driver's job, off the main thread so UIKit keeps the run loop. */
/* The launch thread reports failures on the main thread, and the alert belongs to the
 * controller the scene put on screen; show_message is defined further down. */
static UIViewController *g_root_vc;

/* While the app is in the background, the layer must not hand out drawables
 * that nobody presents. The frame loop stops asking for them (the layer's
 * pause in the translation layer skips the draw and the swap), and the layer
 * itself is told not to make anyone wait for one: in the background a
 * drawable can take a second to arrive, and the app must not sit on it. */
static void show_message(UIViewController *, NSString *, NSString *);

static void audio_session_setup(void);
static void display_size_pixels(UIWindowScene *windowScene, int *out_w, int *out_h);

/* Scripted input, for the simulator (which has no input a script can send):
 * TL_PVZ2_TAP="x,y;wait:3;drag:x1,y1,x2,y2;..." in the game's own pixels, the Mac
 * test app's language. It starts TL_PVZ2_TAP_DELAY seconds (default 3) after the game
 * has started. Launch with SIMCTL_CHILD_TL_PVZ2_TAP=... to set it in the simulator. */
static void test_text(const char *text);
static void test_keyboard_hide(void);
static void test_return(void);
static void keyboard_touch_began(void);

static void send_game_touch(int phase, int id, float x, float y)
{
    if (phase == 0) {
        if (NSThread.isMainThread) keyboard_touch_began();
        else dispatch_sync(dispatch_get_main_queue(), ^{ keyboard_touch_began(); });
    }
    tl_pvz2_touch(phase, id, x, y);
}

static void *tapper(void *arg)
{
    (void)arg;
    pthread_setname_np("PVZ2 taps");
    for (int i = 0; i < 600 && !tl_pvz2_started(); i++) usleep(100000);
    int delay = getenv("TL_PVZ2_TAP_DELAY") ? atoi(getenv("TL_PVZ2_TAP_DELAY")) : 3;
    sleep((unsigned)delay);
    char *copy = strdup(getenv("TL_PVZ2_TAP")), *save = NULL;
    for (char *tok = strtok_r(copy, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        int x, y, x2, y2, wait;
        if (sscanf(tok, "wait:%d", &wait) == 1) {
            if (wait >= 0 && wait <= 600) sleep((unsigned)wait);
        } else if (!strncmp(tok, "press:", 6)) {
            char *text = strdup(tok + 6);
            dispatch_sync(dispatch_get_main_queue(), ^{ test_text(text); });
            free(text);
        } else if (!strcmp(tok, "keyboard:hide")) {
            dispatch_sync(dispatch_get_main_queue(), ^{ test_keyboard_hide(); });
        } else if (!strcmp(tok, "return")) {
            dispatch_sync(dispatch_get_main_queue(), ^{ test_return(); });
        } else if (sscanf(tok, "drag:%d,%d,%d,%d", &x, &y, &x2, &y2) == 4) {
            tl_log_line("ios: the script drags (%d,%d) -> (%d,%d)", x, y, x2, y2);
            send_game_touch(0, 1, (float)x, (float)y);
            for (int i = 1; i <= 10; i++) {
                usleep(20000);
                tl_pvz2_touch(1, 1, (float)(x + (x2 - x) * i / 10), (float)(y + (y2 - y) * i / 10));
            }
            usleep(20000);
            tl_pvz2_touch(3, 1, (float)x2, (float)y2);
            sleep(2);
        } else if (sscanf(tok, "%d,%d", &x, &y) == 2) {
            tl_log_line("ios: the script taps (%d,%d)", x, y);
            send_game_touch(0, 1, (float)x, (float)y);
            usleep(120000);
            tl_pvz2_touch(3, 1, (float)x, (float)y);
            sleep(2);
        }
    }
    free(copy);
    tl_log_line("ios: the tap script is done");
    return NULL;
}

static void *launch_game(void *unused)
{
    (void)unused;
    @autoreleasepool {
        pthread_setname_np("PVZ2 launch");
        tl_audio_install();
        /* The session, and the interruption: the game is silent without them
         * after the first trip to the home screen. */
        audio_session_setup();
        bool ok = tl_pvz2_start(&g_cfg) && tl_pvz2_run();
        atomic_store(&g_ready, ok);
        tl_log_line("ios: driver startup %s", ok ? "complete" : "FAILED");
        if (ok && getenv("TL_PVZ2_TAP")) {
            pthread_t t;
            if (!pthread_create(&t, NULL, tapper, NULL)) pthread_detach(t);
        }
        if (!ok) {
            /* A black screen is not a diagnosis. The usual reason is the one thing this
             * device cannot do without help: iPadOS 27 enforces TXM, so executable memory
             * has to come from a debugger, and StikDebug has to be attached *at launch*
             * rather than afterwards. Say so where the person can read it. */
            dispatch_async(dispatch_get_main_queue(), ^{
                if (g_root_vc) show_message(g_root_vc, @"The game could not start",
                    @"The runtime needs executable memory, and this iPadOS only grants it "
                    "to a debugger. Attach StikDebug and relaunch the app (it must be "
                    "attached at launch, before the game starts).\n\n"
                    @"The log says what happened: Documents/pvz2.log, visible in Files.");
            });
        }
    }
    return NULL;
}

#pragma mark - view

/* The packet in hand: the keyboard took it out of the seed bank and the
 * player's next touch is where it goes.
 *
 * The game plants by *dragging* a seed packet out of the bank. Measured on the
 * Mac: a tap on a packet changes nothing (the card does not even light up) --
 * the packet has to be held and then let go over the lawn. So the shortcut
 * takes the packet in hand (the press, with no release) and the player's next
 * touch carries it there and lets it go, which is what dragging it with a
 * finger does. The shovel is a mode button, and a tap on it is enough. */
static atomic_int g_packet_in_hand;
static atomic_uint g_packet_hand_generation;

/* Take a packet in hand, or put it back into the bank. */
static void packet_take_in_hand(double x, double y)
{
    atomic_store(&g_packet_in_hand, 1);
    atomic_fetch_add(&g_packet_hand_generation, 1);
    uint32_t generation = atomic_load(&g_packet_hand_generation);
    tl_pvz2_touch_injected(0, 1, (float)x, (float)y);
    /* If the player does not touch the screen, the packet goes back into the
     * bank: it must not stay in hand for ever. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(25 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        /* Only this packet's own timer may put it back: a later packet may
         * already be in hand. */
        if (!atomic_load(&g_packet_in_hand)) return;
        if (atomic_load(&g_packet_hand_generation) != generation) return;
        atomic_store(&g_packet_in_hand, 0);
        tl_pvz2_touch_injected(3, 1, (float)x, (float)y);
        tl_log_line("ios: the packet was put back into the bank (%.0f,%.0f)", x, y);
    });
}

@interface PVZ2View : UIView
- (void)updateViewport:(const char *)reason;
- (void)updateSafeInsets;
@property(nonatomic, strong) CAMetalLayer *metal;
@end

@class PVZ2View;      /* the Metal layer lives in the view, as a sublayer */

static void metal_paused(bool paused)
{
    if (!g_root_vc) return;
    CAMetalLayer *layer = [(PVZ2View *)g_root_vc.view metal];
    if (!layer) return;
    layer.allowsNextDrawableTimeout = YES;
    tl_log_line("ios: the layer %s", paused ? "released its drawable" : "asks for drawables again");
}

static CGRect _lastViewport;
@implementation PVZ2View
/* The view's own layer is a plain one, and the Metal layer lives inside it.
 * UIKit owns the view's layer -- it sets its frame to the view's bounds on
 * every layout pass -- so the Metal layer, whose frame is the game's
 * rectangle, is a *sublayer* that nobody else touches. (Fighting UIKit over
 * the view's own layer is what left the game cropped in a corner.) */
+ (Class)layerClass { return [CALayer class]; }
- (BOOL)canBecomeFirstResponder { return YES; }

- (instancetype)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if (self) {
        CAMetalLayer *metal = [CAMetalLayer layer];
        metal.pixelFormat = MTLPixelFormatBGRA8Unorm;
        metal.framebufferOnly = YES;
        metal.contentsScale = [UIScreen mainScreen].scale;
        /* The drawable is the game's own size and never changes, so the
         * guest's layout never changes with the window's shape. */
        metal.drawableSize = CGSizeMake(g_w, g_h);
        /* The layer is the game's own size, in points: a CAMetalLayer draws a
         * drawable one to one and *crops* the part that does not fit, so the
         * drawable must not be bigger than the layer. The layer is then
         * scaled to fit the window (see updateViewport). */
        metal.bounds = CGRectMake(0, 0, g_w / metal.contentsScale, g_h / metal.contentsScale);
        [self.layer addSublayer:metal];
        _metal = metal;
    }
    return self;
}

/* The game keeps its aspect ratio: whatever shape the window is, the game is
 * drawn in the largest rectangle of that shape that fits, and the rest of the
 * window is black. The window can be made narrow, but the game is never
 * cropped and never stretched. (The arithmetic is in husk-tl-viewport.h, and
 * it is tested: scripts/viewport-test.c.) */
- (CGRect)gameRect
{
    double x, y, w, h;
    if (!tl_viewport_fit(self.bounds.size.width, self.bounds.size.height, g_w, g_h, &x, &y, &w, &h))
        return CGRectMake(0, 0, 0, 0);
    return CGRectMake(x, y, w, h);
}

/* The viewport: the game's own rectangle inside the window. The game keeps
 * its aspect ratio, so the rectangle is the largest one of that shape that
 * fits the window, and the rest of the window is black -- in a portrait
 * window the game is a landscape strip with bars above and below.
 *
 * It is recomputed from the window's *current* shape, and checked on every
 * frame: the window can change shape while the app is in the background (the
 * system resizes it then), and a viewport left over from that shape shows the
 * game cropped into a corner. */
- (void)updateViewport:(const char *)reason
{
    CGRect r = [self gameRect];
    if (r.size.width <= 0) return;
    /* The Metal layer is the game's own size, and it is *scaled* to the
     * game's rectangle in the window: the game keeps its aspect, and the
     * rest of the window is black. (Setting the layer's frame instead would
     * crop the drawable -- a CAMetalLayer draws a drawable one to one.) */
    CAMetalLayer *m = self.metal;
    tl_viewport_present(m, self.bounds.size.width, self.bounds.size.height,
                        g_w, g_h, m.contentsScale);
    CGFloat scale = m.affineTransform.a;
    if (CGRectEqualToRect(r, _lastViewport)) return;
    _lastViewport = r;
    tl_log_line("ios: the viewport is %.0fx%.0f at (%.0f,%.0f) in a %.0fx%.0f window, "
                "the layer scaled to %.4f (%s)",
                r.size.width, r.size.height, r.origin.x, r.origin.y,
                self.bounds.size.width, self.bounds.size.height, scale, reason);
}

/* The screen's cutout, for the game: the part of the game's rectangle that the
 * Dynamic Island and the rounded corners cover, in the game's pixels. The game
 * keeps its buttons clear of it, as it does on an Android phone with a camera
 * hole. Black bars around the game already keep it clear, so only the part of
 * the unsafe area that overlaps the game counts. Left and right only: in
 * landscape Android's cutout has no top or bottom either, and the home
 * indicator is not a cutout (it hides itself). */
static int _lastSafe[2] = { -1, -1 };
- (void)updateSafeInsets
{
    CGRect r = [self gameRect];
    if (r.size.width <= 0) return;
    UIEdgeInsets sa = self.safeAreaInsets;
    double px = (double)g_w / r.size.width;
    double left = fmax(0, sa.left - r.origin.x);
    double right = fmax(0, sa.right - (self.bounds.size.width - CGRectGetMaxX(r)));
    int l = (int)lround(left * px), rt = (int)lround(right * px);
    if (l == _lastSafe[0] && rt == _lastSafe[1]) return;
    _lastSafe[0] = l; _lastSafe[1] = rt;
    tl_pvz2_set_safe_insets(l, rt, 0, 0);
    tl_log_line("ios: the safe area is %.0f,%.0f,%.0f,%.0f points (left, right, top, bottom); "
                "the game keeps %d px clear on the left and %d on the right",
                sa.left, sa.right, sa.top, sa.bottom, l, rt);
}

- (void)safeAreaInsetsDidChange
{
    [super safeAreaInsetsDidChange];
    [self updateSafeInsets];
}

- (void)layoutSubviews
{
    [super layoutSubviews];
    self.backgroundColor = [UIColor blackColor];
    /* UIKit owns the root view's geometry. Never force its frame from inside
     * layoutSubviews: doing so freezes the test size and fights every resize. */
    self.layer.backgroundColor = [UIColor blackColor].CGColor;
    [self updateViewport:"the window changed shape"];
    [self updateSafeInsets];
    /* Everything about the geometry, so a mismatch can be seen in the log
     * instead of guessed at from a screenshot. */
    CAMetalLayer *m = self.metal;
    tl_log_line("ios: the view is %.0fx%.0f, its layer %.0fx%.0f at (%.0f,%.0f), "
                "the Metal layer %.0fx%.0f at (%.0f,%.0f) scaled %.3f, with a %.0fx%.0f drawable at %.1fx, "
                "the game is %dx%d",
                self.bounds.size.width, self.bounds.size.height,
                self.layer.bounds.size.width, self.layer.bounds.size.height,
                self.layer.frame.origin.x, self.layer.frame.origin.y,
                m.bounds.size.width, m.bounds.size.height, m.frame.origin.x, m.frame.origin.y,
                m.transform.m11, m.drawableSize.width, m.drawableSize.height, m.contentsScale,
                g_w, g_h);
}

- (void)send:(NSSet<UITouch *> *)touches phase:(int)phase
{
    if (!atomic_load(&g_ready)) return;
    CGRect r = [self gameRect];
    if (r.size.width <= 0 || r.size.height <= 0) return;
    int id = 1;
    for (UITouch *t in touches) {
        CGPoint p = [t locationInView:self];
        /* The game's own pixels: the same rectangle the game is drawn in, so a
         * tap lands where the player sees it even with the window resized. */
        double gx, gy;
        if (!tl_viewport_map(p.x, p.y, r.origin.x, r.origin.y, r.size.width, r.size.height,
                             g_w, g_h, &gx, &gy))
            continue;   /* a tap in the black bars is not a tap on the game */
        if (phase == 0) keyboard_touch_began();
        if (atomic_load(&g_packet_in_hand)) {
            /* A packet is in hand (the keyboard took it out of the bank): the
             * finger carries it where it points and lets it go, which is how a
             * player plants one -- by dragging it out of the bank. */
            if (phase == 4) {
                /* The gesture was cancelled (a call, the app switcher): the
                 * guest is told the touch ended, and the packet goes back into
                 * the bank. Without this the packet stays in hand and the
                 * player's next tap is taken by it. */
                tl_pvz2_touch(4, 1, (float)gx, (float)gy);
                atomic_store(&g_packet_in_hand, 0);
                tl_log_line("ios: the touch was cancelled; the packet is back in the bank");
            } else if (phase == 3) {
                tl_pvz2_touch(1, 1, (float)gx, (float)gy);
                usleep(120000);
                tl_pvz2_touch(3, 1, (float)gx, (float)gy);
                atomic_store(&g_packet_in_hand, 0);
                tl_log_line("ios: the packet was let go at (%.0f,%.0f)", gx, gy);
            } else {
                tl_pvz2_touch(1, 1, (float)gx, (float)gy);
            }
            continue;
        }
        tl_pvz2_touch(phase, id++, (float)gx, (float)gy);
    }
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self send:touches phase:0]; }
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self send:touches phase:1]; }
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self send:touches phase:3]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self send:touches phase:4]; }
@end

static void keyboard_install(UIView *view);

/* The orientations the window may take. On the iPad the window follows the
 * device, like other iPad games: in portrait the game (which has no portrait
 * layout, and is always told it is in landscape) is a landscape strip with bars
 * above and below. A phone stays in landscape, as phone games do: the strip
 * would be a third of its screen. */
static UIInterfaceOrientationMask allowed_orientations(void)
{
    return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone
        ? UIInterfaceOrientationMaskLandscape : UIInterfaceOrientationMaskAll;
}

@interface PVZ2ViewController : UIViewController
@end
@implementation PVZ2ViewController
- (void)loadView { self.view = [[PVZ2View alloc] initWithFrame:[UIScreen mainScreen].bounds]; }
- (void)viewDidLoad { [super viewDidLoad]; keyboard_install(self.view); }
- (void)viewWillTransitionToSize:(CGSize)size
       withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator
{
    [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
    /* The window is about to change shape; the viewport has to follow it. */
    [(PVZ2View *)self.view updateViewport:"the window is turning"];
    [coordinator animateAlongsideTransition:^(id<UIViewControllerTransitionCoordinatorContext> ctx) {
        [(PVZ2View *)self.view updateViewport:"the window is turning"];
    } completion:nil];
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }

/* The audio session.
 *
 * The system deactivates the session when the app is interrupted (a call, the
 * app switcher), and the guest's sound comes back only when the session is
 * active again AND the output is started again. Without this the game is
 * silent after the first trip to the home screen, which is what the player
 * heard. */
static void audio_session_setup(void)
{
    AVAudioSession *session = [AVAudioSession sharedInstance];
    NSError *err = nil;
    if (![session setCategory:AVAudioSessionCategoryAmbient withOptions:0 error:&err]) {
        tl_log_line("ios: the audio session's category was refused: %s",
                    err.localizedDescription.UTF8String);
    } else {
        [session setActive:YES error:&err];
        if (err)
            tl_log_line("ios: the audio session could not be activated: %s",
                        err.localizedDescription.UTF8String);
        else
            tl_log_line("ios: the audio session is active");
    }
    [[NSNotificationCenter defaultCenter]
        addObserverForName:AVAudioSessionInterruptionNotification
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *note) {
        NSNumber *type = note.userInfo[AVAudioSessionInterruptionTypeKey];
        if (!type) return;
        if (type.unsignedIntegerValue == AVAudioSessionInterruptionTypeBegan) {
            tl_log_line("ios: the audio was interrupted");
            return;
        }
        AVAudioSession *ss = [AVAudioSession sharedInstance];
        NSError *e2 = nil;
        [ss setActive:YES error:&e2];
        if (e2)
            tl_log_line("ios: the audio session could not be activated again: %s",
                        e2.localizedDescription.UTF8String);
        tl_audio_restart();
        tl_log_line("ios: the audio is back");
    }];
}

/* The display's own size, in pixels, in landscape order (the long side
 * first) -- the size the guest's surface is made of.
 *
 * The sources are Apple's physical-screen APIs: the display's own pixels
 * (rotation independent), the scene's screen's pixels, and the scene's own
 * points. The scene's size limits are only looked at, and only if they are
 * plausible: iPadOS hands out CGFLOAT_MAX there when the scene may be any
 * size, and that is not a size -- taking it for one gave the guest a surface
 * of -1x-1, and a black screen.
 *
 * The largest plausible source wins (the display's own pixels are the
 * biggest thing there is, and a window is never bigger than the display),
 * and when nothing is plausible the display's own pixels are used and the log
 * says so. */
static void display_size_pixels(UIWindowScene *windowScene, int *out_w, int *out_h)
{
    CGFloat display_scale = UIScreen.mainScreen.scale;
    CGSize main_native = UIScreen.mainScreen.nativeBounds.size;
    CGSize scene_native = windowScene.screen.nativeBounds.size;
    CGSize scene_points = windowScene.coordinateSpace.bounds.size;
    CGSize system_maximum = CGSizeZero;
    if (windowScene.sizeRestrictions) system_maximum = windowScene.sizeRestrictions.maximumSize;

    double cand_w[4], cand_h[4];
    const char *cand_name[4] = { "mainScreen.nativeBounds", "scene.nativeBounds",
                                 "the scene's own points", "the scene's size limit" };
    cand_w[0] = main_native.width;  cand_h[0] = main_native.height;
    cand_w[1] = scene_native.width;  cand_h[1] = scene_native.height;
    cand_w[2] = scene_points.width * display_scale;
    cand_h[2] = scene_points.height * display_scale;
    cand_w[3] = system_maximum.width * display_scale;
    cand_h[3] = system_maximum.height * display_scale;
    for (int i = 0; i < 4; i++)
        tl_log_line("ios: the display's size from %s: %.0fx%.0f pixels%s",
                    cand_name[i], cand_w[i], cand_h[i],
                    pvz2_display_size_plausible(cand_w[i], cand_h[i]) ? "" : " (not plausible)");

    int which = -1, w = 0, h = 0;
    pvz2_display_size(cand_w, cand_h, 4, &w, &h, &which);
    if (which < 0) {
        /* Nothing plausible: the old path -- the display's own pixels -- and
         * a loud line, because a surface of nothing is a black screen. */
        tl_log_line("ios: display size not trusted: using nativeBounds %.0fx%.0f",
                    main_native.width, main_native.height);
        w = (int)(main_native.width > main_native.height ? main_native.width : main_native.height);
        h = (int)(main_native.width > main_native.height ? main_native.height : main_native.width);
    } else if (which != 0) {
        tl_log_line("ios: display size not trusted: using nativeBounds %.0fx%.0f",
                    main_native.width, main_native.height);
    }
    *out_w = w;
    *out_h = h;
}

/* The native shovel: the guest's own "the shovel button was pressed"
 * (ShovelUI::OnMouseDown, guest 0x13ab0a0). This is the DEFAULT now: it is
 * the game's own button, so it needs no coordinates and it cannot be wrong
 * whatever size the surface is. 1 is the touch at the button the guest
 * reports (the old way, kept for the tests), 2 is the guest's own function,
 * 0 turns it off. */
static int native_shovel_mode(void)
{
    const char *env = getenv("TL_PVZ2_NATIVE_SHOVEL");
    return (env && *env) ? atoi(env) : 2;
}

/* The seed shortcut (the number keys): OFF by default. The synthetic touch
 * was measured for a 1748x804 surface and the iPad's surface is 2752x2064, so
 * the touch lands where there is no card -- and it leaves a finger down in the
 * guest, and a finger that is down makes every later tap do nothing. The
 * game's own "the card was pressed" is the way this is done now; until that
 * is in, the shortcut says so and does nothing. The environment turns the old
 * way on, for the tests that need it. */
static bool seed_shortcut_enabled(void)
{
    const char *env = getenv("TL_PVZ2_SEED_SHORTCUT");
    return env && env[0] == '1';
}

/* The native seed pick: the game's own "this card was pressed". On by
 * default; the environment turns it off. */
static int native_seed_pick_mode(void)
{
    const char *env = getenv("TL_PVZ2_SEED_PICK");
    if (env && *env) return atoi(env);
    /* ON in this test build: the pick is proved on the Mac (the game's own
     * press, the cursor made pending, the player's own click plants it), and
     * this build is for the player to try it with a keyboard on the iPad.
     * TL_PVZ2_SEED_PICK=0 turns it off (the old synthetic touch is not used
     * at all -- it was wrong at every surface size but the one it was
     * measured on, and it left a finger down in the game). */
    return 1;
}

/* The hardware keyboard's shortcuts: the same mapping as the Mac's (the shared
 * table in pvz2-keys.h). The system's own keyboard-shortcuts menu lists them,
 * so the player can see what the keys do. */
- (NSArray<UIKeyCommand *> *)keyCommands
{
    NSMutableArray<UIKeyCommand *> *cmds = [NSMutableArray array];
    for (int i = 0; i < PVZ2_SLOT_COUNT; i++) {
        NSString *input = @(pvz2_default_slot_keys[i]);
        UIKeyCommand *cmd = [UIKeyCommand keyCommandWithInput:input
                                                 modifierFlags:0
                                                        action:@selector(pvz2_shortcut:)];
        cmd.discoverabilityTitle = [NSString stringWithFormat:@"Seed packet %d", i + 1];
        [cmds addObject:cmd];
    }
    /* The shovel's key is the letter, not the shifted letter: the system's own
     * matching is on the key, and the mapping itself ignores the case. */
    char lower = (char)tolower((unsigned char)PVZ2_DEFAULT_SHOVEL_KEY[0]);
    NSString *shovel = [NSString stringWithFormat:@"%c", lower];
    UIKeyCommand *sc = [UIKeyCommand keyCommandWithInput:shovel
                                            modifierFlags:0
                                                   action:@selector(pvz2_shortcut:)];
    sc.discoverabilityTitle = @"Shovel";
    [cmds addObject:sc];
    UIKeyCommand *collect = [UIKeyCommand keyCommandWithInput:@" " modifierFlags:0 action:@selector(pvz2_shortcut:)];
    collect.discoverabilityTitle = @"Collect suns and coins";
    [cmds addObject:collect];
    /* Escape, as on the Mac: the pause button in a level, Back anywhere else. */
    UIKeyCommand *back = [UIKeyCommand keyCommandWithInput:UIKeyInputEscape modifierFlags:0 action:@selector(pvz2_shortcut:)];
    back.discoverabilityTitle = @"Pause / Back";
    [cmds addObject:back];
    return cmds;
}

- (void)pvz2_shortcut:(UIKeyCommand *)cmd
{
    if (!atomic_load(&g_ready)) return;
    const char *chars = cmd.input.UTF8String;
    if ([cmd.input isEqualToString:UIKeyInputEscape]) { tl_pvz2_request_escape(); return; }
    /* While the guest is typing a name, the keys are the guest's. The event
     * has already been matched as a key command, so the characters are handed
     * to the guest as text: otherwise a digit or an "s" would be swallowed by
     * the shortcut and never reach the name field. */
    if (tl_pvz2_keyboard_showing()) {
        if (chars && *chars) tl_pvz2_text_action(0, chars);
        return;
    }
    if ([cmd.input isEqualToString:@" "]) { tl_pvz2_request_collect_sun(); return; }
    int slot = pvz2_slot_for_key(chars, pvz2_default_slot_keys, PVZ2_DEFAULT_SHOVEL_KEY);
    if (slot < 0) return;
    double x, y;
    if (slot < PVZ2_SLOT_COUNT) {
        if (native_seed_pick_mode()) {
            /* The game's own press: the card's own pointer-down handler, on
             * the game's own thread. No touch, no coordinates. */
            tl_pvz2_request_seed_pick(slot);
            tl_log_line("ios: the shortcut for a seed packet asks the guest's own press (slot %d)", slot);
            return;
        }
        if (!seed_shortcut_enabled()) {
            tl_log_line("ios: the seed shortcut is off: the synthetic touch is broken at this surface size, and the game's own pick is not in yet");
            return;
        }
        pvz2_packet_position(slot, &x, &y);
        tl_log_line("ios: the shortcut for a seed packet takes it in hand at (%.0f,%.0f)", x, y);
        packet_take_in_hand(x, y);
    } else if (native_shovel_mode()) {
        /* The guest's own shovel, on the game's own thread (experimental; the
         * ordinary tap is what the game expects, and it is the default). */
        tl_pvz2_request_native_shovel();
        tl_log_line("ios: the shortcut for the shovel asks the guest's own shovel");
        return;
    } else {
        /* The shovel. The game's own button when the game has told us where it
         * is and its answer agrees with the measurement; the measured place
         * otherwise. (On the iPad the game cannot be asked yet -- the app object
         * cannot be found there, so the scale from the game's own space is not
         * known -- so the measured numbers are what it uses.) */
        int geo[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        int from_guest = 0;
        if (tl_pvz2_ui_geometry(&geo[0], &geo[4]))
            pvz2_shovel_position(&geo[4], &x, &y, &from_guest);
        else {
            x = PVZ2_SHOVEL_X;
            y = PVZ2_SHOVEL_Y;
        }
        tl_log_line("ios: the shortcut for the shovel taps (%.0f,%.0f)%s", x, y,
                    from_guest ? " (the game's own button)" : "");
        tl_pvz2_touch(0, 1, (float)x, (float)y);
        /* The up comes 120 ms later, without blocking the run loop. */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(120 * NSEC_PER_MSEC)),
                       dispatch_get_main_queue(), ^{
            tl_pvz2_touch(3, 1, (float)x, (float)y);
        });
    }
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return allowed_orientations(); }
@end

#pragma mark - the keyboard

/* The guest asks the activity for a keyboard (Device_ShowKeyboard) when the
 * player taps the name field, and the characters come back as text events.
 * iOS has no EditText, so the host keeps a UITextField of its own -- out of
 * sight, but first responder when the guest asks -- and sends what is typed
 * to the guest the way Android's EditInputConnection does: the characters
 * that were committed (action 0), and one event per character deleted
 * (action 3). The guest cannot tell the two apart. */
static UITextField *g_field;

static void keyboard_resign(const char *reason)
{
    if (!g_field.isFirstResponder) return;
    tl_log_line("ios: resignFirstResponder (%s), keyboard flag=%d", reason, tl_pvz2_keyboard_showing());
    [g_field resignFirstResponder];
}

static void keyboard_hide_if_still_dismissed(void)
{
    /* A touch on the name field makes the guest ask for the keyboard again.
     * Keep UIKit editing throughout that round trip so the keyboard never flickers. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.18 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        if (tl_pvz2_keyboard_showing()) {
            tl_log_line("ios: keeping the keyboard after a new guest request: flag=1, first responder=%d",
                        g_field.isFirstResponder);
            return;
        }
        keyboard_resign("edit stayed dismissed");
    });
}

static void keyboard_touch_began(void)
{
    if (!tl_pvz2_keyboard_showing()) return;
    tl_log_line("ios: a game touch ends the current edit: keyboard flag=1 -> 0");
    tl_pvz2_keyboard_dismissed();
    keyboard_hide_if_still_dismissed();
}

static void keyboard_shown(int show)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        if (show) {
            /* At start-up the guest asks for the keyboard and drops it again at
             * once: by the time this runs it is already hidden again, and raising
             * it now would only flash it on the screen. */
            if (!tl_pvz2_keyboard_showing()) {
                tl_log_line("ios: the guest's keyboard request was already withdrawn: not shown");
                return;
            }
            /* Each entry starts empty: the guest keeps its own text, and the
             * field is only a way to see the keyboard and to hear the keys. */
            g_field.text = @"";
            BOOL ok = [g_field becomeFirstResponder];
            /* With a hardware keyboard -- a Magic Keyboard, or a Mac's keyboard
             * through Universal Control -- iPadOS shows no on-screen keyboard:
             * the keys still arrive here. */
            tl_log_line("ios: the guest asked for a keyboard: flag=%d, first responder=%d, in a window=%d",
                        tl_pvz2_keyboard_showing(), ok, g_field.window != nil);
        } else {
            keyboard_hide_if_still_dismissed();
        }
    });
}

@interface PVZ2FieldDelegate : NSObject <UITextFieldDelegate>
@end
@implementation PVZ2FieldDelegate
- (void)textFieldDidEndEditing:(UITextField *)field
{
    tl_pvz2_keyboard_dismissed();
}
- (void)keyboardWillHide:(NSNotification *)notification
{
    /* The iPad keyboard's hide key can hide it without ending first responder.
     * Android clears its keyboard flag here; the guest then releases the caret. */
    tl_pvz2_keyboard_dismissed();
    keyboard_resign("keyboard will hide");
}
- (BOOL)textField:(UITextField *)field
    shouldChangeCharactersInRange:(NSRange)range
                replacementString:(NSString *)text
{
    if (text.length == 0) {
        /* A deletion. The engine has no other way to delete: its text record
         * only appends, and its only delete is the BACKSPACE key (Android
         * keycode 67) -- one press per character. */
        for (NSUInteger i = 0; i < range.length; i++) {
            tl_pvz2_key(67, 1);
            tl_pvz2_key(67, 0);
        }
        return YES;
    }
    tl_pvz2_text_action(0, text.UTF8String);
    return YES;
}
- (BOOL)textFieldShouldReturn:(UITextField *)field
{
    /* The IME's done key. The guest's own key events carry Android's key
     * codes, and ENTER is 66. */
    keyboard_resign("Return");
    tl_pvz2_key(66, 1);
    tl_pvz2_key(66, 0);
    return NO;
}
@end

static void keyboard_install(UIView *view)
{
    static PVZ2FieldDelegate *delegate;
    if (!delegate) delegate = [PVZ2FieldDelegate new];
    g_field = [[UITextField alloc] initWithFrame:CGRectMake(0, 0, 1, 1)];
    g_field.delegate = delegate;
    g_field.alpha = 0.01;                 /* present enough to take the keyboard */
    g_field.autocorrectionType = UITextAutocorrectionTypeNo;
    g_field.autocapitalizationType = UITextAutocapitalizationTypeNone;
    g_field.keyboardType = UIKeyboardTypeDefault;
    /* An empty input view lets the simulator exercise hardware-like input
     * while keeping first responder, without changing its global keyboard setting. */
    if (getenv("TL_PVZ2_TEST_HARDWARE_KEYBOARD")) {
        g_field.inputView = [[UIView alloc] initWithFrame:CGRectZero];
        g_field.inputAssistantItem.leadingBarButtonGroups = @[];
        g_field.inputAssistantItem.trailingBarButtonGroups = @[];
        tl_log_line("ios: test hardware keyboard: using an empty input view");
    }
    [view addSubview:g_field];
    [[NSNotificationCenter defaultCenter] addObserver:delegate selector:@selector(keyboardWillHide:)
                                                 name:UIKeyboardWillHideNotification object:nil];
    tl_pvz2_set_keyboard_hook(keyboard_shown);
    tl_pvz2_set_open_url_hook(open_game_url);
}

/* The simulator script exercises the same delegate as keyboard input. UIKit's
 * programmatic insertText does not call shouldChangeCharactersInRange. */
static void test_text(const char *text)
{
    tl_log_line("ios: the script types text: keyboard flag=%d, first responder=%d",
                tl_pvz2_keyboard_showing(), g_field.isFirstResponder);
    NSString *value = [NSString stringWithUTF8String:text];
    NSRange range = NSMakeRange(g_field.text.length, 0);
    if ([g_field.delegate textField:g_field shouldChangeCharactersInRange:range replacementString:value])
        [g_field insertText:value];
}
static void test_keyboard_hide(void)
{
    tl_log_line("ios: the script dismisses the keyboard");
    keyboard_resign("script hide");
}
static void test_return(void)
{
    tl_log_line("ios: the script presses Return");
    [g_field.delegate textFieldShouldReturn:g_field];
}

#pragma mark - paths
static void mkdirs(const char *path)
{
    char t[1024]; snprintf(t, sizeof(t), "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0755); *p = '/'; }
    mkdir(t, 0755);
}

/* A new game version's OBB has a new name, and the old one is 1.3 GB that nothing
 * reads any more: once the one this app wants is in place, the others go. */
static void drop_other_obbs(NSString *dir, NSString *keep)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *name in [fm contentsOfDirectoryAtPath:dir error:nil]) {
        if ([name isEqualToString:keep] || ![name.pathExtension.lowercaseString isEqualToString:@"obb"]) continue;
        if ([fm removeItemAtPath:[dir stringByAppendingPathComponent:name] error:nil])
            tl_log_line("ios: removed another version's OBB: %s", name.UTF8String);
    }
}

/* A full IPA's OBB is copied into Documents/obb, where a slim app keeps its own. The
 * copy is a clone (APFS, and NSFileManager clones): instant, no space of its own, and
 * it outlives the bundle, so an update to the slim app (the AltStore source's) keeps
 * the game's data. */
static void stage_bundled_obb(NSString *from_dir, NSString *docs)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *dir = [docs stringByAppendingPathComponent:@"obb"];
    for (NSString *name in [fm contentsOfDirectoryAtPath:from_dir error:nil]) {
        if (![name.pathExtension.lowercaseString isEqualToString:@"obb"]) continue;
        NSString *from = [from_dir stringByAppendingPathComponent:name];
        NSString *to = [dir stringByAppendingPathComponent:name];
        long long size = [[fm attributesOfItemAtPath:from error:nil][NSFileSize] longLongValue];
        NSDictionary *have = [fm attributesOfItemAtPath:to error:nil];
        if (!have || [have[NSFileSize] longLongValue] != size) {
            NSString *part = [to stringByAppendingString:@".part"];
            NSError *err = nil;
            [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
            [fm removeItemAtPath:part error:nil];
            [fm removeItemAtPath:to error:nil];
            if (![fm copyItemAtPath:from toPath:part error:&err] || ![fm moveItemAtPath:part toPath:to error:&err]) {
                tl_log_line("ios: could not stage the bundled OBB: %s", err.localizedDescription.UTF8String);
                [fm removeItemAtPath:part error:nil];
                return;
            }
            [[NSURL fileURLWithPath:dir] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
            tl_log_line("ios: staged the bundled OBB into Documents/obb (%s)", name.UTF8String);
        }
        drop_other_obbs(dir, name);
        return;
    }
}

/* The APK and OBB that came with the app, if any: an IPA that carries the game needs no
 * file transfer to start, and Documents stays the place a user can replace them. An
 * update that brings another game version replaces the APK staged by the one before:
 * the game version the staged copies came with is noted beside them. */
static void stage_bundled_game(NSString *docs)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *src = [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"Game"];
    if (![fm fileExistsAtPath:src]) return;
    NSString *mark = [docs stringByAppendingPathComponent:@".bundled-game-version"];
    NSString *version = NSBundle.mainBundle.infoDictionary[@"PVZ2GameVersion"] ?: @"";
    NSString *staged = [NSString stringWithContentsOfFile:mark encoding:NSUTF8StringEncoding error:nil];
    bool new_game = version.length && ![staged isEqualToString:version];

    for (NSString *name in [fm contentsOfDirectoryAtPath:src error:nil]) {
        NSString *from = [src stringByAppendingPathComponent:name];
        NSString *to = [docs stringByAppendingPathComponent:name];
        if ([name isEqualToString:@"obb"]) { stage_bundled_obb(from, docs); continue; }
        bool replace = new_game && [name.pathExtension.lowercaseString isEqualToString:@"apk"];
        if ([fm fileExistsAtPath:to]) {
            if (!replace) continue;                          // the user's copy wins
            [fm removeItemAtPath:to error:nil];
        }
        NSError *err = nil;
        if (![fm copyItemAtPath:from toPath:to error:&err])
            tl_log_line("ios: could not stage %s: %s", name.UTF8String,
                        err.localizedDescription.UTF8String);
        else
            tl_log_line("ios: staged %s from the app bundle", name.UTF8String);
    }
    if (new_game) [version writeToFile:mark atomically:YES encoding:NSUTF8StringEncoding error:nil];
    /* an OBB a user put in Documents itself belongs in Documents/obb */
    NSString *obb = [docs stringByAppendingPathComponent:@"obb"];
    for (NSString *name in [fm contentsOfDirectoryAtPath:docs error:nil]) {
        if (![name.pathExtension.lowercaseString isEqualToString:@"obb"]) continue;
        NSString *from = [docs stringByAppendingPathComponent:name];
        if ([fm fileExistsAtPath:obb]) return;
        if (![fm createDirectoryAtPath:obb withIntermediateDirectories:YES
            attributes:nil error:nil]) return;
        [fm moveItemAtPath:from toPath:[obb stringByAppendingPathComponent:name] error:nil];
        tl_log_line("ios: moved the OBB in Documents into Documents/obb");
        return;
    }
}

/* The shared staging policy lets an updated bundle replace only an older cache. */
static void stage_bundled_cdn(NSString *docs)
{
    if (getenv("TL_PVZ2_MAIN_TAR")) return;
    NSString *src = [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"Game/1.4.2.main.tar"];
    NSString *dst = [docs stringByAppendingPathComponent:@"data/No_Backup/1.4.2.main.tar"];
    tl_cdn_stage(src.UTF8String, dst.UTF8String);
}

/* The APK is the only .apk in Documents; the OBB lives in Documents/obb and the
 * translation layer finds it beside the APK by itself. */
static bool find_inputs(NSString *docs)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSArray<NSString *> *items = [fm contentsOfDirectoryAtPath:docs error:nil];
    NSString *apk = nil;
    for (NSString *name in items)
        if ([name.pathExtension.lowercaseString isEqualToString:@"apk"]) { apk = name; break; }
    if (!apk) return false;
    snprintf(g_apk, sizeof(g_apk), "%s", [docs stringByAppendingPathComponent:apk].UTF8String);

    NSString *obbDir = [docs stringByAppendingPathComponent:@"obb"];
    if (![fm fileExistsAtPath:obbDir]) return false;
    NSArray<NSString *> *obbs = [fm contentsOfDirectoryAtPath:obbDir error:nil];
    for (NSString *name in obbs)
        if ([name.pathExtension.lowercaseString isEqualToString:@"obb"]) return true;
    return false;
}

static void show_message(UIViewController *vc, NSString *title, NSString *body)
{
    UIAlertController *a = [UIAlertController alertControllerWithTitle:title message:body
                                                        preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [vc presentViewController:a animated:YES completion:nil];
}

#pragma mark - the first launch: the expansion file

/* The slim app carries no OBB (1.3 GB of the game's data): it downloads it on the
 * first launch, the way the mod's own Android helper (OBBHelper) does, from the
 * links in the app's Info.plist (PVZ2ObbURLs, written by the build), and checks the
 * size and the sha256 the build wrote next to them before the game ever sees it.
 *
 * An OBB already in Documents whose size is not the one the app names is another
 * version of the game's data (the 1.4.0 one, say): it is downloaded again and
 * replaced. Only the size is looked at on a normal launch; the hash is checked once,
 * after the download. */
@interface PVZ2ObbDownload : NSObject <NSURLSessionDownloadDelegate>
- (instancetype)initWithDocs:(NSString *)docs controller:(UIViewController *)vc done:(void (^)(void))done;
+ (BOOL)neededIn:(NSString *)docs;
- (BOOL)start;
@end

static PVZ2ObbDownload *g_obb_download;

static NSDictionary *obb_info(void)
{
    NSDictionary *plist = NSBundle.mainBundle.infoDictionary;
    NSArray *urls = plist[@"PVZ2ObbURLs"];
    NSString *sha = plist[@"PVZ2ObbSHA256"];
    NSNumber *size = plist[@"PVZ2ObbSize"];
    NSString *name = plist[@"PVZ2ObbName"] ?: @"main.675.com.ea.game.pvz2_rfl.obb";
    if (![urls isKindOfClass:NSArray.class] || !urls.count || !sha.length || size.longLongValue <= 0) return nil;
    return @{ @"urls": urls, @"sha": sha.lowercaseString, @"size": size, @"name": name };
}

@implementation PVZ2ObbDownload {
    NSString *_docs, *_obbDir, *_name, *_sha;
    long long _size;
    NSArray<NSString *> *_urls;
    NSUInteger _urlIndex;
    UIViewController *_vc;
    void (^_done)(void);
    NSURLSession *_session;
    NSData *_resume;
    UIView *_overlay;
    UILabel *_title, *_detail;
    UIProgressView *_bar;
    UIButton *_retry;
    UIBackgroundTaskIdentifier _bg;
}

+ (BOOL)neededIn:(NSString *)docs
{
    NSDictionary *info = obb_info();
    if (!info) return NO;
    NSString *path = [[docs stringByAppendingPathComponent:@"obb"] stringByAppendingPathComponent:info[@"name"]];
    NSDictionary *attrs = [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
    if (!attrs) return YES;
    long long have = [attrs[NSFileSize] longLongValue], want = [info[@"size"] longLongValue];
    if (have == want) return NO;
    tl_log_line("ios: the OBB in Documents is %lld bytes and this app wants %lld: it is downloaded again",
                have, want);
    return YES;
}

- (instancetype)initWithDocs:(NSString *)docs controller:(UIViewController *)vc done:(void (^)(void))done
{
    if (!(self = [super init])) return nil;
    _docs = docs; _vc = vc; _done = [done copy];
    _obbDir = [docs stringByAppendingPathComponent:@"obb"];
    _bg = UIBackgroundTaskInvalid;
    return self;
}

- (BOOL)start
{
    NSDictionary *info = obb_info();
    if (!info) return NO;
    _urls = info[@"urls"]; _sha = info[@"sha"]; _size = [info[@"size"] longLongValue]; _name = info[@"name"];
    [self buildOverlay];
    [self begin];
    return YES;
}

- (void)buildOverlay
{
    UIView *root = _vc.view;
    _overlay = [[UIView alloc] initWithFrame:root.bounds];
    _overlay.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _overlay.backgroundColor = UIColor.blackColor;
    _title = [UILabel new];
    _title.text = @"Downloading the game's data";
    _title.font = [UIFont boldSystemFontOfSize:22];
    _title.textColor = UIColor.whiteColor;
    _title.textAlignment = NSTextAlignmentCenter;
    _title.numberOfLines = 0;
    _bar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    _detail = [UILabel new];
    _detail.font = [UIFont monospacedDigitSystemFontOfSize:15 weight:UIFontWeightRegular];
    _detail.textColor = UIColor.lightGrayColor;
    _detail.textAlignment = NSTextAlignmentCenter;
    _detail.numberOfLines = 0;
    _retry = [UIButton buttonWithType:UIButtonTypeSystem];
    [_retry setTitle:@"Try again" forState:UIControlStateNormal];
    _retry.titleLabel.font = [UIFont boldSystemFontOfSize:18];
    _retry.hidden = YES;
    [_retry addTarget:self action:@selector(retryTapped) forControlEvents:UIControlEventTouchUpInside];
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[ _title, _bar, _detail, _retry ]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 16;
    stack.alignment = UIStackViewAlignmentFill;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [_overlay addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:_overlay.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:_overlay.centerYAnchor],
        [stack.widthAnchor constraintEqualToAnchor:_overlay.widthAnchor multiplier:0.6],
    ]];
    [root addSubview:_overlay];
}

- (void)say:(NSString *)detail
{
    _detail.text = detail;
}

- (BOOL)enoughSpace
{
    NSURL *url = [NSURL fileURLWithPath:_docs];
    NSNumber *free = nil;
    [url getResourceValue:&free forKey:NSURLVolumeAvailableCapacityForImportantUsageKey error:nil];
    long long need = _size + (256ll << 20);
    if (!free || free.longLongValue >= need) return YES;
    /* An OBB of another version is on its way out anyway: it goes first when
     * both do not fit. */
    NSString *old = [_obbDir stringByAppendingPathComponent:_name];
    if ([NSFileManager.defaultManager fileExistsAtPath:old]) {
        [NSFileManager.defaultManager removeItemAtPath:old error:nil];
        tl_log_line("ios: the old OBB was removed to make room for the new one");
        [url removeAllCachedResourceValues];
        [url getResourceValue:&free forKey:NSURLVolumeAvailableCapacityForImportantUsageKey error:nil];
        if (!free || free.longLongValue >= need) return YES;
    }
    _title.text = @"Not enough free space";
    [self say:[NSString stringWithFormat:@"The game's data needs %.1f GB free on this device (%.1f GB is free now). "
                                          "Free some space, then try again.",
                                         need / 1e9, free.longLongValue / 1e9]];
    _retry.hidden = NO;
    tl_log_line("ios: not enough space for the OBB: %lld bytes free, %lld needed", free.longLongValue, need);
    return NO;
}

- (void)begin
{
    _retry.hidden = YES;
    _title.text = @"Downloading the game's data";
    if (![self enoughSpace]) return;
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    if (_bg == UIBackgroundTaskInvalid)
        _bg = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"obb" expirationHandler:^{
            [UIApplication.sharedApplication endBackgroundTask:self->_bg];
            self->_bg = UIBackgroundTaskInvalid;
        }];
    if (!_session)
        _session = [NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.defaultSessionConfiguration
                                                 delegate:self delegateQueue:NSOperationQueue.mainQueue];
    NSURLSessionDownloadTask *task;
    if (_resume) {
        task = [_session downloadTaskWithResumeData:_resume];
        tl_log_line("ios: the OBB download resumes");
    } else {
        NSURL *url = [NSURL URLWithString:_urls[_urlIndex]];
        task = [_session downloadTaskWithURL:url];
        tl_log_line("ios: the OBB is downloaded from %s", url.absoluteString.UTF8String);
    }
    _resume = nil;
    [self say:@"Starting…"];
    [task resume];
}

- (void)retryTapped
{
    [self begin];
}

- (void)URLSession:(NSURLSession *)session downloadTask:(NSURLSessionDownloadTask *)task
      didWriteData:(int64_t)bytes totalBytesWritten:(int64_t)written totalBytesExpectedToWrite:(int64_t)expected
{
    (void)session; (void)task; (void)bytes; (void)expected;
    _bar.progress = (float)((double)written / (double)_size);
    [self say:[NSString stringWithFormat:@"%.0f of %.0f MB", written / 1e6, _size / 1e6]];
}

- (void)URLSession:(NSURLSession *)session downloadTask:(NSURLSessionDownloadTask *)task
    didFinishDownloadingToURL:(NSURL *)location
{
    (void)session;
    NSInteger status = [task.response isKindOfClass:NSHTTPURLResponse.class]
                     ? ((NSHTTPURLResponse *)task.response).statusCode : 200;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *part = [_obbDir stringByAppendingPathComponent:[_name stringByAppendingString:@".part"]];
    [fm createDirectoryAtPath:_obbDir withIntermediateDirectories:YES attributes:nil error:nil];
    [fm removeItemAtPath:part error:nil];
    /* The system deletes the file when this returns: it is moved now. */
    NSError *err = nil;
    if (status < 200 || status >= 300 || ![fm moveItemAtURL:location toURL:[NSURL fileURLWithPath:part] error:&err]) {
        tl_log_line("ios: the OBB download failed (HTTP %ld%s%s)", (long)status, err ? ", " : "",
                    err ? err.localizedDescription.UTF8String : "");
        [self failed:[NSString stringWithFormat:@"The server answered %ld.", (long)status] nextLink:YES];
        return;
    }
    _title.text = @"Checking the game's data";
    [self say:@"This takes a few seconds."];
    NSString *want = _sha, *name = _name, *dir = _obbDir;
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
            if (have != size || ![got isEqualToString:want]) {
                tl_log_line("ios: the downloaded OBB is wrong (%lld bytes, sha256 %s); it is deleted",
                            have, got.UTF8String ?: "?");
                [NSFileManager.defaultManager removeItemAtPath:part error:nil];
                [self failed:@"The download was damaged." nextLink:YES];
                return;
            }
            NSString *final = [dir stringByAppendingPathComponent:name];
            [NSFileManager.defaultManager removeItemAtPath:final error:nil];
            NSError *e = nil;
            if (![NSFileManager.defaultManager moveItemAtPath:part toPath:final error:&e]) {
                tl_log_line("ios: the OBB could not be put in place: %s", e.localizedDescription.UTF8String);
                [self failed:@"The game's data could not be saved." nextLink:NO];
                return;
            }
            /* 1.3 GB that can be downloaded again does not belong in a backup. */
            [[NSURL fileURLWithPath:dir] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
            tl_log_line("ios: the OBB is downloaded and checked (%lld bytes)", have);
            drop_other_obbs(dir, name);
            [self finish];
        });
    });
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error
{
    (void)session; (void)task;
    if (!error) return;
    _resume = error.userInfo[NSURLSessionDownloadTaskResumeData];
    tl_log_line("ios: the OBB download stopped: %s%s", error.localizedDescription.UTF8String,
                _resume ? " (it can resume)" : "");
    [self failed:error.localizedDescription nextLink:(_resume == nil)];
}

- (void)failed:(NSString *)why nextLink:(BOOL)next
{
    if (next && _urls.count > 1) _urlIndex = (_urlIndex + 1) % _urls.count;
    UIApplication.sharedApplication.idleTimerDisabled = NO;
    _title.text = @"The download stopped";
    [self say:[NSString stringWithFormat:@"%@ Check the connection, then try again.", why]];
    _retry.hidden = NO;
}

- (void)finish
{
    UIApplication.sharedApplication.idleTimerDisabled = NO;
    [_session finishTasksAndInvalidate];
    _session = nil;
    [_overlay removeFromSuperview];
    if (_bg != UIBackgroundTaskInvalid) {
        [UIApplication.sharedApplication endBackgroundTask:_bg];
        _bg = UIBackgroundTaskInvalid;
    }
    if (_done) _done();
}
@end

#pragma mark - app

/* iOS 26+ refuses to run an app that has not adopted the scene lifecycle (it is
 * killed with SIGTRAP from UIApplicationEvaluateRuntimeIssueForNoSceneLifecycle-
 * Adoption), so the window and the game live in a scene delegate. StikDebug's JIT
 * region is still claimed in didFinishLaunching, which runs first. */
@interface PVZ2Delegate : UIResponder <UIApplicationDelegate>
@end

@implementation PVZ2Delegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options
{
    (void)app; (void)options;

    /* The log has to be readable off the device: keep it in Documents. A console
     * run (devicectl process launch --console) sets TL_IOS_CONSOLE to keep stderr
     * on the console instead, where the launch tool can stream it. */
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    if (!getenv("TL_IOS_CONSOLE")) {
        /* Each launch starts its own log. The one before it stays as pvz2.previous.log
         * (after a crash, that is the run that crashed), so the log never grows past
         * two runs. */
        NSString *logPath = [docs stringByAppendingPathComponent:@"pvz2.log"];
        rename(logPath.UTF8String,
               [docs stringByAppendingPathComponent:@"pvz2.previous.log"].UTF8String);
        freopen(logPath.UTF8String, "a", stderr);
        setvbuf(stderr, NULL, _IOLBF, 0);
    }

#ifdef AOT_DYLIB_TEST
    /* AOT-DYLIB-LOADER TEST: before anything else, and it never comes back. Built in
     * only with -DAOT_DYLIB_TEST, because the shipping app has a game to start. */
    @autoreleasepool { aot_dylib_test(); }
#endif

    /* JIT-free mode. If the bundle carries converted guest libraries (Frameworks/
     * libPVZ2.dylib and its .facts file), point the loader at them: it hands those back
     * as ordinary dylibs instead of mapping the guest's ELF, so nothing in this process
     * ever has to make memory executable or write code. That is the whole difference
     * between needing StikDebug attached at launch and not needing it. */
    NSString *fw = NSBundle.mainBundle.privateFrameworksPath;
    if (!fw) fw = [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"Frameworks"];
    bool jit_free = [[NSFileManager defaultManager]
        fileExistsAtPath:[fw stringByAppendingPathComponent:@"libPVZ2.dylib"]];
    if (jit_free) {
        setenv("TL_AOT_DIR", fw.UTF8String, 1);
        tl_log_line("ios: JIT-free: converted guest libraries in %s", fw.UTF8String);
    } else {
        unsetenv("TL_AOT_DIR");
        tl_log_line("ios: no converted guest libraries in %s: this build cannot run the game", fw.UTF8String);
    }

    static unsigned char sigstack[65536];
    stack_t ss = { .ss_sp = sigstack, .ss_size = sizeof(sigstack) };
    sigaltstack(&ss, NULL);
    struct sigaction sa = { .sa_sigaction = on_crash, .sa_flags = SA_SIGINFO | SA_ONSTACK };
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT };
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaction(sigs[i], &sa, NULL);

    return YES;
}
@end

#pragma mark - scene

@interface PVZ2SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
- (void)watchOrientation;
@end

@implementation PVZ2SceneDelegate

/* The device is turned: the interface orientation changes, and the guest has
 * to be told, or it keeps drawing the old way up. There is no notification
 * for the interface orientation itself, so a slow display link watches it.
 * (The system stops the link when the app is in the background.) */
- (void)watchOrientation
{
    CADisplayLink *link = [CADisplayLink displayLinkWithTarget:self selector:@selector(orientationTick:)];
    /* The preferred rate has to lie between the minimum and the maximum, or
     * CoreAnimation throws. The link runs at the display's own rate: it is
     * where the viewport is checked against the window's current shape. */
    if (@available(iOS 15.0, *)) {
        double top = [UIScreen mainScreen].maximumFramesPerSecond;
        if (top < 30) top = 30;
        link.preferredFrameRateRange = CAFrameRateRangeMake(1, top, top);
    }
    [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}
- (void)orientationTick:(CADisplayLink *)link
{
    (void)link;
    /* Every frame: the window's shape can change without a layout pass (the
     * system resizes the window while the app is in the background), and the
     * viewport has to follow it or the game shows cropped. */
    [(PVZ2View *)g_root_vc.view updateViewport:"every frame"];
    if (!g_scene) return;
    int o = guest_orientation_for(g_scene.interfaceOrientation);
    if (o != g_orientation) {
        g_orientation = o;
        tl_log_line("ios: the interface turned (%d), the guest stays in landscape", o);
    }
    /* The interface and the device can differ (the test asks for a portrait
     * interface on a landscape display), and then the system turns the
     * window's content to match the display. The game itself is never
     * turned: it is always drawn upright, in its own rectangle. */
    UIDeviceOrientation dev = [UIDevice currentDevice].orientation;
    if (dev != _last_device_orientation) {
        _last_device_orientation = dev;
        tl_log_line("ios: the device is %ld, the interface is %ld", (long)dev,
                    (long)g_scene.interfaceOrientation);
    }
}

/* The measurement the window-aspect experiment needs.
 *
 * iPadOS can either let the app's window take any shape (the app is told, and
 * draws in it) or put the app in the compatibility mode, where the scene's
 * size is fixed and the system scales the whole thing uniformly -- the way
 * Clash of Clans behaves. The two are told apart by this line: in the
 * compatibility mode the effective geometry NEVER changes and only the scale
 * does; in the free mode it changes with every pixel of the drag.
 *
 * It is written every second, so the log shows what the geometry is even when
 * nothing is being dragged, and the delegate's callback (below) reports the
 * changes as they happen.
 */
- (void)logEffectiveGeometry
{
    if (@available(iOS 26.0, *)) {
        UIWindowSceneGeometry *g = g_scene.effectiveGeometry;
        CGSize s = g.coordinateSpace.bounds.size;
        tl_log_line("ios: geometry %.0fx%.0f  interactive=%d  locked=%d",
                    s.width, s.height, (int)g.isInteractivelyResizing,
                    (int)g.isInterfaceOrientationLocked);
    }
}

- (void)windowScene:(UIWindowScene *)windowScene
    didUpdateEffectiveGeometry:(UIWindowSceneGeometry *)previousGeometry
{
    (void)previousGeometry;
    [self logEffectiveGeometry];
    /* If the display's size was not known when the scene connected, it is
     * asked again here -- the scene's geometry is the first thing that is
     * really true about the display, and the guest's surface waits for it. */
    if (g_w < 100 || g_h < 100) {
        int dw = 0, dh = 0;
        display_size_pixels(windowScene, &dw, &dh);
        if (dw >= 100 && dh >= 100) {
            g_w = dw;
            g_h = dh;
            tl_log_line("ios: the guest's surface is created at %dx%d from the display", g_w, g_h);
            tl_pvz2_set_surface_size(g_w, g_h);
        }
    }
}

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options
{
    (void)session; (void)options;
    UIWindowScene *windowScene = (UIWindowScene *)scene;

    /* iPadOS 26 and later give every app a window it can resize, and a scene that does not
     * ask for anything else gets a small one: 688x1032 points on a 13-inch iPad, which is
     * the letterboxed phone-sized window this app used to come up in. Ask for the screen's
     * own geometry, then insist on it in the window's frame, because the scene's answer is
     * applied asynchronously. */
    /* The display, not the window: on iPadOS 26+ mainScreen.bounds is the
     * window's own -- and in willConnectToSession it is not ready at all, so
     * the scene's own coordinate space is the size to go by. */
    /* The window's shape is the scene's own space: the system sizes it from
     * the device, and turns it when the device turns (the app allows every
     * orientation). Pinning it to the display's landscape size is what drew
     * the game sideways on a portrait screen. */
    CGRect full = CGRectMake(0, 0, windowScene.coordinateSpace.bounds.size.width,
                              windowScene.coordinateSpace.bounds.size.height);
    /* The display, for the window's limits: the window may grow to the whole
     * display, and shrink to about 40% of it.
     *
     * And the display's size is not always in the list of screens: a scene
     * that is a *window* sees a screen of the window's own size (that is what
     * iPadOS gives a windowed app), and the display is then only known from
     * the size restrictions the system itself set -- which is the display's
     * size, and which this host is about to replace with its own. So it is
     * read first. */
    /* The DISPLAY's size, in pixels, from Apple's own physical-screen APIs.
     *
     * The scene's size limits are NOT a source: iPadOS hands out
     * CGFLOAT_MAX there when the scene may be any size, and taking that for
     * the display's size gave the guest a surface of -1x-1 and a black
     * screen. They are only looked at, and only if they are plausible. */
    {
        int dw = 0, dh = 0;
        display_size_pixels(windowScene, &dw, &dh);
        g_w = dw; g_h = dh;
        if (g_w >= 100 && g_h >= 100)
            tl_log_line("ios: the guest's surface is created at %dx%d from the display", g_w, g_h);
        else
            tl_log_line("ios: the display's size is not known yet (%dx%d); the guest waits for it",
                        g_w, g_h);
    }
    double window_w = CGRectGetWidth(full), window_h = CGRectGetHeight(full);
    tl_log_line("ios: the window is %.0fx%.0f points", window_w, window_h);
    (void)0;
    {
    CGFloat display_scale = UIScreen.mainScreen.scale;
    CGSize main_native = UIScreen.mainScreen.nativeBounds.size;
    CGSize scene_native = windowScene.screen.nativeBounds.size;
    CGSize scene_points = windowScene.coordinateSpace.bounds.size;
    CGSize system_maximum = CGSizeZero;
    if (windowScene.sizeRestrictions) system_maximum = windowScene.sizeRestrictions.maximumSize;

    double cand_w[4], cand_h[4];
    const char *cand_name[4] = { "mainScreen.nativeBounds", "scene.nativeBounds",
                                 "the scene's own points", "the scene's size limit" };
    cand_w[0] = main_native.width;  cand_h[0] = main_native.height;
    cand_w[1] = scene_native.width;  cand_h[1] = scene_native.height;
    cand_w[2] = scene_points.width * display_scale;
    cand_h[2] = scene_points.height * display_scale;
    cand_w[3] = system_maximum.width * display_scale;
    cand_h[3] = system_maximum.height * display_scale;
    /* What each source says, so a wrong one is visible in the log. */
    for (int i = 0; i < 4; i++)
        tl_log_line("ios: the display's size from %s: %.0fx%.0f pixels%s",
                    cand_name[i], cand_w[i], cand_h[i],
                    pvz2_display_size_plausible(cand_w[i], cand_h[i]) ? "" : " (not plausible)");
    int which = -1, disp_w_px = 0, disp_h_px = 0;
    pvz2_display_size(cand_w, cand_h, 4, &disp_w_px, &disp_h_px, &which);
    if (which < 0) {
        /* Nothing plausible: the old path -- the display's own pixels -- and
         * a loud line, because a surface of nothing is a black screen. */
        tl_log_line("ios: display size not trusted: using nativeBounds %.0fx%.0f",
                    main_native.width, main_native.height);
        disp_w_px = (int)(main_native.width > main_native.height ? main_native.width : main_native.height);
        disp_h_px = (int)(main_native.width > main_native.height ? main_native.height : main_native.width);
        which = 0;
    } else if (which != 0) {
        tl_log_line("ios: display size not trusted: using nativeBounds %.0fx%.0f",
                    main_native.width, main_native.height);
    }
    }

    if (@available(iOS 16.0, *)) {
        UIWindowSceneGeometryPreferencesIOS *pref = [[UIWindowSceneGeometryPreferencesIOS alloc] init];
        pref.interfaceOrientations = allowed_orientations();
        /* The window can be made smaller -- the game is scaled into whatever
         * shape the window has -- but not to nothing: the smallest sensible
         * window is about 40% of the game's size, with the game's own ratio.
         * (iPad games usually allow even less shrinking.) */
        double disp_scale = UIScreen.mainScreen.scale;
        double display_w = (double)g_w / disp_scale, display_h = (double)g_h / disp_scale;
        double long_side = MAX(display_w, display_h);
        double short_side = MIN(display_w, display_h);
        UISceneSizeRestrictions *limits = windowScene.sizeRestrictions;
        if (limits) {
            limits.minimumSize = CGSizeMake(long_side * 0.4, short_side * 0.4);
            limits.maximumSize = CGSizeMake(long_side, long_side);
            tl_log_line("ios: the window may be %.0fx%.0f to %.0fx%.0f points",
                        long_side * 0.4, short_side * 0.4, long_side, long_side);
        }
        [windowScene requestGeometryUpdateWithPreferences:pref errorHandler:^(NSError *e) {
            tl_log_line("ios: full-screen geometry refused: %s", e.localizedDescription.UTF8String);
        }];
    }

    /* A test can ask for a rotation without touching the device: launch with
     * TL_FORCE_PORTRAIT=1 and the scene is asked for a portrait window, so
     * the game has to letterbox itself into it -- which is the same code path
     * the player's own rotation takes. */
    /* A test can ask for a rotation without touching the device: launch with
     * TL_FORCE_PORTRAIT=1 and the scene is asked for a portrait window, which
     * is the same code path the player's own rotation takes. */
    const char *force_portrait = getenv("TL_FORCE_PORTRAIT");
    if (@available(iOS 16.0, *)) {
      if (force_portrait && force_portrait[0] == '1') {
        UIWindowSceneGeometryPreferencesIOS *pref_portrait = [[UIWindowSceneGeometryPreferencesIOS alloc] init];
        pref_portrait.interfaceOrientations = UIInterfaceOrientationMaskPortrait;
        [windowScene requestGeometryUpdateWithPreferences:pref_portrait errorHandler:^(NSError *e) {
            tl_log_line("ios: the portrait window was refused: %s", e.localizedDescription.UTF8String);
        }];
      }
      /* And TL_FORCE_LANDSCAPE=1 the other way, for a phone (or the simulator) that starts
       * upright: the game is then seen as the player holds it. */
      const char *force_landscape = getenv("TL_FORCE_LANDSCAPE");
      if (force_landscape && force_landscape[0] == '1') {
        UIWindowSceneGeometryPreferencesIOS *pref_land = [[UIWindowSceneGeometryPreferencesIOS alloc] init];
        pref_land.interfaceOrientations = UIInterfaceOrientationMaskLandscapeRight;
        [windowScene requestGeometryUpdateWithPreferences:pref_land errorHandler:^(NSError *e) {
            tl_log_line("ios: the landscape window was refused: %s", e.localizedDescription.UTF8String);
        }];
      }
    }

    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;

    CGSize screen = windowScene.coordinateSpace.bounds.size;
    for (UIScreen *s in UIScreen.screens)
        tl_log_line("ios: screen %.0fx%.0f scale %.1f",
                    CGRectGetWidth(s.bounds), CGRectGetHeight(s.bounds), s.scale);
    /* The guest's surface is created from the DISPLAY's shape, never from the
     * window the scene happens to start in.
     *
     * The guest reads its size once, at start-up, and keeps it: its aspect and
     * its touch mapping are frozen there. So a scene that starts in a small
     * window (the window the player left it in, which iPadOS restores) would
     * give the game a phone-like surface for ever -- the game was drawn in a
     * 1.9:1 strip and scaled up, which is what the player saw. The surface is
     * therefore the display's own landscape size, in the display's pixels
     * (2752x2064 on a 13-inch iPad Pro), and whatever shape the window has,
     * that surface is drawn into it (letterboxed, never stretched). */
    if (g_w < 100 || g_h < 100) {
        /* The display's size was not known yet: the layer waits for it (and
         * says so in the log), and this is where it is asked again. */
        tl_log_line("ios: the display's size is not known yet (%dx%d); the guest waits for it", g_w, g_h);
    }
    g_scene = windowScene;
    /* The guest is always told it is in landscape: the game has no portrait
     * layout, and the *window* rotates instead (the game is then a landscape
     * strip in a portrait window, with bars above and below). */
    g_orientation = 4;
    tl_pvz2_set_orientation(g_orientation);
    tl_log_line("ios: the interface is %ld, the guest is in landscape (%d)",
                (long)windowScene.interfaceOrientation, g_orientation);

    self.window = [[UIWindow alloc] initWithWindowScene:windowScene];
    self.window.frame = full;
    self.window.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    /* The geometry, every second: the signal the window-aspect experiment
     * reads (in the compatibility mode the size never changes; in the free
     * mode it changes with the drag). */
    [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        [self logEffectiveGeometry];
    }];
    fprintf(stderr, "ios: scene=%.0fx%.0f screen=%.0fx%.0f window=%.0fx%.0f\n",
            screen.width, screen.height, full.size.width, full.size.height,
            self.window.bounds.size.width, self.window.bounds.size.height);
    PVZ2ViewController *vc = [PVZ2ViewController new];
    g_root_vc = vc;
    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
    [self watchOrientation];

    NSString *data = [docs stringByAppendingPathComponent:@"data"];
    /* A fresh first run, for a test: the game's own saves are disposable while
     * the work is going on, so the data directory can be wiped at launch.
     * Launch with TL_WIPE_DATA=1 (or the argument --wipe-data). */
    if (getenv("TL_WIPE_DATA") || [[NSProcessInfo processInfo].arguments containsObject:@"--wipe-data"]) {
        NSError *err = nil;
        if ([[NSFileManager defaultManager] removeItemAtPath:data error:&err])
            tl_log_line("ios: the data directory was wiped, for a fresh first run");
        else if (err)
            tl_log_line("ios: the data directory could not be wiped: %s",
                        err.localizedDescription.UTF8String);
    }
    mkdirs(data.UTF8String);
    snprintf(g_data, sizeof(g_data), "%s", data.UTF8String);

    stage_bundled_game(docs);
    stage_bundled_cdn(docs);
    bool have_inputs = find_inputs(docs);
    /* The APK is there and the OBB is not (the slim app's first launch), or it is
     * another version's: the app downloads it, then starts the game. */
    if (g_apk[0] && [PVZ2ObbDownload neededIn:docs]) {
        __weak typeof(self) weakSelf = self;
        g_obb_download = [[PVZ2ObbDownload alloc] initWithDocs:docs controller:vc done:^{
            g_obb_download = nil;
            if (find_inputs(docs)) [weakSelf startGameIn:vc];
        }];
        if ([g_obb_download start]) return;
        g_obb_download = nil;
    }
    if (!have_inputs) {
        tl_log_line("ios: no APK/OBB in Documents; see the app's README");
        show_message(vc, @"Game files needed",
                     @"Put your Reflourished .apk in this app's Documents folder and the OBB at "
                      "Documents/obb/main.675.com.ea.game.pvz2_rfl.obb, then relaunch.");
        return;
    }
    [self startGameIn:vc];
}

/* The game itself, once the APK and the OBB are in Documents. */
- (void)startGameIn:(PVZ2ViewController *)vc
{
    NSString *egl = [[NSBundle mainBundle].privateFrameworksPath stringByAppendingPathComponent:@"libANGLE-shared.dylib"];
    snprintf(g_egl, sizeof(g_egl), "%s", egl.UTF8String);
    tl_log_line("ios: apk=%s data=%s egl=%s surface=%dx%d", g_apk, g_data, g_egl, g_w, g_h);

    PVZ2View *view = (PVZ2View *)vc.view;
    /* The Metal layer is the view's own sublayer now (see PVZ2View). */
    CAMetalLayer *layer = view.metal;
    /* The display's own maximum: the game's "Target 60 FPS" setting means
     * "the highest rate this device can do" here, and the game is
     * delta-driven, so a faster presentation makes it smoother and not
     * faster. */
    int top = (int)[UIScreen mainScreen].maximumFramesPerSecond;
    if (top < 30) top = 30;
    tl_pvz2_set_max_fps(top);
    /* The native shovel (experimental; off by default). */
    tl_pvz2_set_native_shovel(native_shovel_mode());
    /* The native seed pick: the game's own press. */
    tl_pvz2_set_seed_pick(native_seed_pick_mode());
    /* The guest's own memory warning (experimental; off by default). */
    const char *lowmem = getenv("TL_PVZ2_LOW_MEMORY");
    tl_pvz2_set_low_memory_warning(lowmem && lowmem[0] == '1');
    tl_log_line("ios: the display can do %d fps, and that is what the game's setting asks for", top);
    [view becomeFirstResponder];
    /* The game asks for the cutout once, while it starts: it has to be known now. */
    [view layoutIfNeeded];
    [view updateSafeInsets];

    g_cfg = (tl_ga_config){ .apk_path = g_apk, .data_dir = g_data, .package_name = "com.ea.game.pvz2_rfl",
                            .width = g_w, .height = g_h, .metal_layer = (__bridge void *)layer,
                            .angle_egl = g_egl, .angle_gles = NULL, .frame_dir = NULL, .frame_every = 0 };

    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t thread;
    int err = pthread_create(&thread, &attr, launch_game, NULL);
    pthread_attr_destroy(&attr);
    if (err) { tl_log_line("ios: cannot create the launch thread (%d)", err); return; }
    pthread_detach(thread);
}

/* Sound and drawing stop while the app is not in front, and the guest is told
 * that it is going away -- it saves on that event, and the event is what the
 * user triggers when they swipe the app out of the app switcher. */
/* The memory warning: the game's Java has no memory callback at all, so the
 * host is the activity. The guest's own function (LawnApp::LowMemoryWarning,
 * guest 0x93fcb0) is called on the game's own thread. Off by default (the
 * guest's function is experimental); the environment turns it on. */
- (void)applicationDidReceiveMemoryWarning:(UIApplication *)application
{
    (void)application;
    tl_log_line("ios: the app was told about the memory pressure");
    tl_pvz2_request_low_memory();
}
- (void)applicationWillTerminate:(UIApplication *)application
{
    (void)application;
    /* The app is going away for good: the guest is told, so it saves. iOS
     * rarely calls this -- a suspended app is killed without a word -- but
     * when it does, the save must be on disk. */
    tl_log_line("ios: terminating");
    tl_pvz2_set_paused(true);
    tl_pvz2_lifecycle_background();
}
- (void)sceneWillResignActive:(UIScene *)scene
{
    (void)scene;
    tl_log_line("ios: resigning active");
    /* The order matters, and it is the order the crash taught:
     *
     *   1. the drawing stops (the loop keeps running: it still runs the
     *      guest's jobs, and the request below is done there -- on the game's
     *      own thread, where the guest's engine is);
     *   2. the guest is told it is going away: its own handler writes the
     *      save, and the pump runs the job that writes it. This waits for
     *      the guest, so the save is on the disk before the system takes the
     *      app away;
     *   3. and only then the drawable is released -- the guest's teardown
     *      jobs touch the renderer, and a renderer whose drawable is gone is
     *      what crashed the app on the way out.
     */
    tl_audio_set_paused(true);
    tl_pvz2_set_paused(true);
    tl_pvz2_lifecycle_background();     /* the guest saves on this */
    metal_paused(true);                /* the layer stops handing out drawables */
}
- (void)sceneDidEnterBackground:(UIScene *)scene
{
    (void)scene;
    tl_log_line("ios: entered background");
    /* The app is about to be suspended, so the guest is *not* told again
     * here: the thread that would tell it is suspended too, and the call
     * would block the user interface for a second for nothing. It was told
     * in sceneWillResignActive, which is the event the game saves on. */
    metal_paused(true);
}
- (void)sceneDidBecomeActive:(UIScene *)scene
{
    (void)scene;
    metal_paused(false);
    tl_audio_set_paused(false);
    /* The system stops the audio when the app is interrupted, and the game is
     * silent until the session is active again and the output is started. */
    {
        AVAudioSession *session = [AVAudioSession sharedInstance];
        NSError *err = nil;
        if (![session setActive:YES error:&err] && err)
            tl_log_line("ios: the audio session could not be activated: %s",
                        err.localizedDescription.UTF8String);
        tl_audio_restart();
    }
    tl_pvz2_set_paused(false);
    tl_pvz2_lifecycle_foreground();
    /* The window may have a new shape (the system resizes it while the app
     * is in the background), so the viewport is checked right away. */
    [(PVZ2View *)g_root_vc.view updateViewport:"the app became active"];
}
- (void)sceneWillEnterForeground:(UIScene *)scene
{
    (void)scene;
    tl_log_line("ios: will enter foreground");
}
@end


int main(int argc, char **argv)
{
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([PVZ2Delegate class]));
    }
}
