/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Plants vs. Zombies 2: Reflourished -- the translation-layer driver.
 *
 * Why a driver at all: the game is not a NativeActivity and not an engine Husk
 * already knows.  Its Java shell (PvZ2GameActivity -> SexyAppFrameworkActivity)
 * builds a GLSurfaceView, an AndroidGameApp (PopCap's bridge to Android) and a
 * few service objects, calls Native_GameAppInitialize, and from then on the
 * engine runs on a GL thread: surface created/changed, then a frame per call.
 * Everything the native library asks Java for is a fixed, small set of methods
 * (see the hook table at the bottom); the PvZ 2 RE project documented them and
 * this file implements the same contract against Husk's Java world.
 *
 * The pieces that are specific to this game and are easy to get wrong:
 *
 *   - FrameworkInfo_SysGetMainExpansionFilePath must return the .obb path with
 *     a leading '/', because the engine treats anything else as a resource name
 *     to be looked up through the Java asset API (PvZ 2 RE, framework_activity).
 *   - The engine patches libPVZ2.so at start-up (libRFLib's A64HookFunction),
 *     so code-write emulation has to be on before libRFLib's constructors run.
 *   - The Reflourished library's constructors run after the game's JNI_OnLoad,
 *     not with the game's own initialisers (the Switch port does the same).
 */

#define _DARWIN_C_SOURCE
#include "husk-tl-pvz2.h"

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>
#if !TARGET_OS_IPHONE
/* The mach_vm calls are not available on iOS (mach_vm.h says so). The app
 * object is only looked for on the Mac, where the surface's size can change;
 * on the iPad the surface is the game's own size for ever. */
#include <mach/mach_vm.h>
#define PVZ2_HAVE_MACH_VM 1
#endif
#include <sys/sysctl.h>

#include "husk-tl-bionic.h"
#include "husk-tl-codewrite.h"
#include "husk-tl-dexindex.h"
#include "husk-tl-egl.h"
#include "husk-tl-framerate.h"
#include "husk-tl-jni.h"
#include "husk-tl-aot.h"
#include "husk-tl-ld.h"
#include "husk-tl-xmem.h"

void tl_jni_hle_install(void);
void tl_hle_configure(const char *pkg, const char *apk, const char *data, int w, int h);
void tl_hle_set_activity(jobj *a);
jobj *tl_hle_assets(void);
void tl_set_data_dir(const char *dir);
const char *tl_data_dir(void);
void tl_nwindow_configure(int w, int h, void *layer);
void *tl_nwindow_get(void);

#define PKG "com.ea.game.pvz2_rfl"
#define APP "com/popcap/SexyAppFramework/AndroidGameApp"
#define ACT "com/popcap/SexyAppFramework/SexyAppFrameworkActivity"
#define ACT2 "com/popcap/PvZ2/PvZ2GameActivity"
#define VIEW "com/popcap/SexyAppFramework/AndroidSurfaceView"
#define HTTP "com/popcap/SexyAppFramework/AndroidHttpProxy"
#define TXN "com/popcap/SexyAppFramework/AndroidHttpTransaction"
#define CLOUD "com/popcap/SexyAppFramework/cloud/Cloud"
#define PLAYC "com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect"
#define PLAYA "com/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements"
#define PLAYL "com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard"
#define NOTIF "com/popcap/SexyAppFramework/AndroidNotification"
#define UIEV "com/popcap/SexyAppFramework/AndroidUIEventManager"

#define OBB_NAME "main.675." PKG ".obb"

static struct {
    tl_ga_config cfg;
    char apk[1024], data[512], pkg[128], frame_dir[512];
    char obb_guest[600], obb_host[700], files_guest[600];
    int w, h;
    float scale;
    jobj *activity, *gameapp, *view, *surface, *holder, *http, *cloud, *playc, *playa, *playl, *notif, *uiev;
    pthread_t gl;
    atomic_bool running, paused, ready;
    atomic_bool loop_running;     /* the draw loop is going */
    atomic_bool in_background;    /* the guest was told the app is going away */
    bool started;
    atomic_int max_fps;           /* the display's own maximum */
    atomic_int target_fps;        /* the rate the host presents at */
    atomic_int asked_fps;         /* what the game last asked for (0: not yet) */
    int orientation;              /* the guest's vocabulary: 4 landscape, 3 landscape flipped */
    atomic_uint_least64_t want_wh;  /* the surface size the host asks for, packed (0 = none) */
    atomic_uint_least64_t start_wh; /* the size the guest was started with, packed */
} P;

static tl_lib *GAME_LIB;

static jvalue vl(void *p) { jvalue v; v.j = 0; v.l = p; return v; }
static jvalue vi(int i) { jvalue v; v.j = 0; v.i = i; return v; }
static jvalue vj(int64_t j) { jvalue v; v.j = j; return v; }
static jvalue vz(int z) { jvalue v; v.j = 0; v.z = z != 0; return v; }
static const char *S(const jobj *o) { const char *s = tl_jni_string(o); return s ? s : ""; }
static jobj *str(const char *s) { return tl_jni_new_string(s ? s : ""); }

/* ------------------------------------------------------------- settings store
 *
 * AndroidGameApp.Config_* is backed by the Activity's SharedPreferences on a
 * phone.  The engine expects writes to be visible to the next read and to
 * KeyExists, so an accept-and-forget stub is not enough: settings that do not
 * persist make the game think it is a fresh install every launch.
 */
typedef struct { char *key, *val; int is_int; int32_t num; } setting;
static struct { setting *v; int n, cap; pthread_mutex_t mu; } CFG = { NULL, 0, 0, PTHREAD_MUTEX_INITIALIZER };

static setting *cfg_find(const char *k)
{
    for (int i = 0; i < CFG.n; i++) if (!strcmp(CFG.v[i].key, k)) return &CFG.v[i];
    return NULL;
}
static void cfg_put(const char *k, setting s)
{
    pthread_mutex_lock(&CFG.mu);
    setting *e = cfg_find(k);
    if (!e) {
        if (CFG.n == CFG.cap) { CFG.cap = CFG.cap ? CFG.cap * 2 : 32; CFG.v = realloc(CFG.v, (size_t)CFG.cap * sizeof(*CFG.v)); }
        e = &CFG.v[CFG.n++];
        memset(e, 0, sizeof(*e));
        e->key = strdup(k);
    } else { free(e->val); e->val = NULL; }
    e->val = s.val; e->is_int = s.is_int; e->num = s.num;
    pthread_mutex_unlock(&CFG.mu);
}

/* The engine reads and writes these through Config_* only, so a small
 * tab-separated file in the app's data directory stands in for the phone's
 * SharedPreferences.  Without it every launch looks like a fresh install and
 * the option screen, tutorial flags and volume all reset. */
static const char *cfg_file_path(void)
{
    static char path[1200];
    if (!path[0]) {
        char dir[1100];
        snprintf(dir, sizeof(dir), "%s/config", tl_data_dir());
        mkdir(dir, 0755);
        snprintf(path, sizeof(path), "%s/pvz2-config", dir);
    }
    return path;
}
/* Values are paths, version strings and numbers: escape the three bytes that
 * would break one-record-per-line. */
static void cfg_esc(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (const char *p = in; *p && j + 2 < n; p++) {
        if (*p == '\\' || *p == '\n' || *p == '\t') { out[j++] = '\\'; out[j++] = *p == '\\' ? '\\' : *p == '\n' ? 'n' : 't'; }
        else out[j++] = *p;
    }
    out[j] = 0;
}
static void cfg_unesc(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (const char *p = in; *p && j + 1 < n; p++) {
        if (*p == '\\' && p[1]) { p++; out[j++] = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p; }
        else out[j++] = *p;
    }
    out[j] = 0;
}
static void cfg_load(void)
{
    FILE *f = fopen(cfg_file_path(), "r");
    if (!f) return;
    char line[4096];
    int loaded = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *t1 = strchr(line, '\t'); if (!t1) continue;
        *t1++ = 0;
        char *t2 = strchr(t1, '\t'); if (!t2) continue;
        *t2++ = 0;
        setting s; memset(&s, 0, sizeof(s));
        char key[256], val[3500];
        cfg_unesc(line, key, sizeof(key));
        cfg_unesc(t2, val, sizeof(val));
        s.is_int = atoi(t1);
        s.num = (int32_t)strtol(val, NULL, 10);
        if (!s.is_int) s.val = val[0] ? strdup(val) : NULL;
        cfg_put(key, s);
        loaded++;
    }
    fclose(f);
    tl_log_line("pvz2: config loaded, %d setting(s) from %s", loaded, cfg_file_path());
}
static void cfg_save(void)
{
    char tmp[1400];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cfg_file_path());
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    pthread_mutex_lock(&CFG.mu);
    for (int i = 0; i < CFG.n; i++) {
        char key[600], val[3500];
        cfg_esc(CFG.v[i].key, key, sizeof(key));
        if (CFG.v[i].is_int) snprintf(val, sizeof(val), "%d", CFG.v[i].num);
        else cfg_esc(CFG.v[i].val ? CFG.v[i].val : "", val, sizeof(val));
        fprintf(f, "%s\t%d\t%s\n", key, CFG.v[i].is_int, val);
    }
    pthread_mutex_unlock(&CFG.mu);
    fclose(f);
    rename(tmp, cfg_file_path());
}

/* ---------------------------------------------------------------- Java side */

static void J_ignore(tl_jcall *c) { (void)c; c->ret = vi(0); }
static void J_true(tl_jcall *c) { (void)c; c->ret = vz(1); }
static void J_false(tl_jcall *c) { (void)c; c->ret = vz(0); }
static void J_empty(tl_jcall *c) { (void)c; c->ret = vl(str("")); }
static void J_void(tl_jcall *c) { (void)c; }

/* The keyboard. The guest asks the activity for one (Device_ShowKeyboard) and
 * the characters come back as text events. A host that has a keyboard -- the
 * iPad's, or a Mac's -- raises it and sends the characters; a host that has
 * none does nothing, and the guest waits, which is what it did before. */
static void (*g_keyboard_hook)(int show);
static int g_keyboard_up;

void tl_pvz2_set_keyboard_hook(void (*fn)(int show)) { g_keyboard_hook = fn; }
int tl_pvz2_keyboard_showing(void) { return g_keyboard_up; }

static void J_show_keyboard(tl_jcall *c)
{
    g_keyboard_up = 1;
    if (g_keyboard_hook) g_keyboard_hook(1);
    c->ret = vz(1);
}
static void J_hide_keyboard(tl_jcall *c)
{
    g_keyboard_up = 0;
    if (g_keyboard_hook) g_keyboard_hook(0);
    c->ret = vz(1);
}
static void J_keyboard_showing(tl_jcall *c) { c->ret = vz(g_keyboard_up); }

/* AndroidHttpProxy.GetNetworkStatus: 0 none, 1 mobile, 2 Wi-Fi.  The title
 * polls this every frame and keeps waiting while it reads 0, so report a
 * connected device (TL_PVZ2_NET overrides it). */
static void J_http_network_status(tl_jcall *c)
{
    const char *e = getenv("TL_PVZ2_NET");
    c->ret = vi(e ? atoi(e) : 2);
}

/* Cloud.attemptSilentSync: Android always completes this leg, even with no
 * account signed in.  Staying silent parks PlayerIdentityService on the title
 * screen forever, so report the real fact (not authenticated) through the
 * game's own Native_AuthStatusChanged(false), as the Switch port does. */
static void J_cloud_silent_sync(tl_jcall *c)
{
    typedef void (*auth_fn)(void *, void *, unsigned char);
    auth_fn auth = (auth_fn)tl_jni_native(CLOUD, "Native_AuthStatusChanged", "(Z)V");
    tl_log_line("pvz2: Cloud.attemptSilentSync -> Native_AuthStatusChanged(false)%s", auth ? "" : " (not registered)");
    if (auth) auth(tl_jni_env(), c->self, 0);
}

static void J_obb_path(tl_jcall *c) { (void)c; tl_log_line("pvz2: OBB path asked for: %s", P.obb_guest); c->ret = vl(str(P.obb_guest)); }
static void J_data_folder(tl_jcall *c) { (void)c; c->ret = vl(str(P.files_guest)); }

static void J_uuid(tl_jcall *c) { (void)c; c->ret = vl(str("6a8d1f2c-3b47-4e59-9c0a-pvz2husk01")); }
/* The guest's own orientation vocabulary (from its Java, Device_GetCurrentUIOrientation):
 *   4 = landscape, 3 = landscape turned over, 1 = portrait, 2 = portrait turned over.
 * The host tells the guest which one it is in, and the guest re-lays-out. */
static void J_orientation(tl_jcall *c) { (void)c; c->ret = vi(P.orientation); }

static void J_set_framerate(tl_jcall *c)
{
    int asked = c->args[0].i;
    int max = atomic_load(&P.max_fps);
    int target = tl_framerate_target(asked, max);
    tl_log_line("pvz2: the game asks for %d fps; the host will present at %d", asked, target);
    atomic_store(&P.asked_fps, asked);
    atomic_store(&P.target_fps, target);
}
static void J_product_version(tl_jcall *c) { (void)c; c->ret = vl(str("1")); }
static void J_package(tl_jcall *c) { (void)c; c->ret = vl(str(PKG)); }
static void J_activity_name(tl_jcall *c) { (void)c; c->ret = vl(str(ACT2)); }
static void J_locale(tl_jcall *c) { (void)c; c->ret = vl(str("en_US")); }
static void J_country(tl_jcall *c) { (void)c; c->ret = vl(str("US")); }
static void J_currency_code(tl_jcall *c) { (void)c; c->ret = vl(str("USD")); }
static void J_currency_symbol(tl_jcall *c) { (void)c; c->ret = vl(str("$")); }
static void J_device_name(tl_jcall *c) { (void)c; c->ret = vl(str("Husk")); }
static void J_device_id(tl_jcall *c) { (void)c; c->ret = vl(str("UNKNOWN/EMULERATOR")); }

/* The game's memory diagnostic. It used to answer zero, and the device calls
 * it every ~73 seconds -- a game that sizes anything by "total memory" was
 * deciding with a zero. The host's own figures: the resident size for "used"
 * and the machine's memory for "total". */
static void J_memory_used(tl_jcall *c)
{
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        c->ret = vj((int64_t)info.phys_footprint);
    else
        c->ret = vj((int64_t)0);
}
static void J_memory_total(tl_jcall *c)
{
    uint64_t total = 0;
    size_t size = sizeof(total);
    if (sysctlbyname("hw.memsize", &total, &size, NULL, 0) != 0) total = 0;
    c->ret = vj((int64_t)total);
}
static void J_os_version(tl_jcall *c) { (void)c; c->ret = vl(str("14")); }
static void J_hardware(tl_jcall *c) { (void)c; c->ret = vl(str("iPhone")); }

/* static void Graphics_GetScreenSizeInPixels(int[]) / ...InPoints(int[]) */
static void J_screen_size(tl_jcall *c)
{
    jobj *a = c->args[0].l;
    /* The array comes from the guest's own code, so it is worth saying whether it was
     * one this layer can write into: a pointer that is not an array here is a pointer
     * into the guest's memory, and the game would read back whatever was in it. */
    tl_log_line("pvz2: screen size into %p (kind %d)", (void *)a, a ? (int)a->kind : -1);
    if (a && a->kind == TL_K_PRIM_ARRAY && a->arr.len >= 2 && a->arr.etype == 'I') {
        ((int32_t *)a->arr.data)[0] = P.w;
        ((int32_t *)a->arr.data)[1] = P.h;
    }
    c->ret = vi(0);
}
/* The screen's cutout (the iPhone's Dynamic Island and rounded corners), as
 * the host measured it: left, right, top, bottom, in the surface's pixels. */
static _Atomic int32_t g_safe[4];

void tl_pvz2_set_safe_insets(int left, int right, int top, int bottom)
{
    atomic_store(&g_safe[0], left); atomic_store(&g_safe[1], right);
    atomic_store(&g_safe[2], top); atomic_store(&g_safe[3], bottom);
}

static void J_safe_rect(tl_jcall *c)
{
    jobj *a = c->args[0].l;
    if (a && a->kind == TL_K_PRIM_ARRAY && a->arr.len >= 4 && a->arr.etype == 'I') {
        /* Despite its name this is NOT an x/y/width/height rectangle.
         * AndroidSurfaceView returns left/right/top/bottom cutout insets,
         * all zero when there is no cutout. Returning screen dimensions
         * here pushes MainMenu's controls outside the drawable area.
         *
         * The game reads them in its own layout units, 600 to the surface's
         * height (measured: 156 moved the map's buttons 208 px in on an
         * 804 px tall surface), and Android hands it half the larger side
         * inset in both [0] and [1]. The host's numbers are the pixels to
         * keep clear, so they are converted here. A test can pretend to be
         * a phone with TL_PVZ2_SAFE_INSETS=left,right,top,bottom (pixels).
         * The game asks once, while it starts. */
        int32_t *o = (int32_t *)a->arr.data;
        int px[4] = { 0, 0, 0, 0 };
        const char *env = getenv("TL_PVZ2_SAFE_INSETS");
        if (!env || sscanf(env, "%d,%d,%d,%d", &px[0], &px[1], &px[2], &px[3]) != 4)
            for (int i = 0; i < 4; i++) px[i] = atomic_load(&g_safe[i]);
        for (int i = 0; i < 4; i++)
            o[i] = P.h > 0 ? (int32_t)((px[i] * 600LL + P.h / 2) / P.h) : 0;
        static atomic_int calls;
        int n = atomic_fetch_add(&calls, 1);
        if (n < 8 || (n & (n - 1)) == 0)
            tl_log_line("pvz2: safe insets asked (%d): left %d right %d top %d bottom %d px "
                        "of a %d px tall surface -> %d %d %d %d",
                        n + 1, px[0], px[1], px[2], px[3], P.h, o[0], o[1], o[2], o[3]);
    }
    c->ret = vi(0);
}

/* The per-frame poll.
 *
 * The game asks whether a config key exists once per frame, so the number of these
 * calls is the number of frames the game's *own* update loop got through -- a much
 * better measure of "the game is running" than the frames the harness presents, which
 * keep coming from whatever drew last.
 *
 * With TL_PVZ2_POLL_TRACE set, the address the call came from is printed too. In the
 * converting route the guest's code is native, so the return address is the guest's
 * own instruction: it says exactly which function does the per-frame work, and where
 * that work stops.
 */
static void poll_trace(const char *what, const char *k)
{
    static int on = -1;
    if (on < 0) on = getenv("TL_PVZ2_POLL_TRACE") ? 1 : 0;
    if (!on) return;
    /* The return address is in the JNI stub, not in the guest: the guest calls the
     * method through the JNIEnv table, so the stub is between. The guest's own caller
     * is one or two frames up, and the guest's code is native, so the frame chain
     * reaches it. */
    void *fp = __builtin_frame_address(0);
    uintptr_t addrs[6];
    int n = 0;
    for (int d = 0; d < 6 && fp && ((uintptr_t)fp & 7) == 0; d++) {
        uintptr_t *fr = (uintptr_t *)fp;
        addrs[n++] = fr[1];
        fp = (uintptr_t *)fr[0];
    }
    char buf[256];
    int p = snprintf(buf, sizeof(buf), "pvz2: poll %s(\"%s\")", what, k ? k : "?");
    for (int i = 0; i < n && p < (int)sizeof(buf) - 24; i++)
        p += snprintf(buf + p, sizeof(buf) - p, " %p", (void *)addrs[i]);
    tl_log_line("%s", buf);
}

/* The end of the game's start-up, for the host. The game reads
 * "prevent_load_game" in every start (about 6 s in), right before it loads the
 * save; the loading after it stalls the frames (25, then 44 fps), and the
 * title runs smoothly once it is done. So: that read, and then 2 s of frames
 * with no gap over 150 ms. A new screen size written into the game before that
 * crashes it (the host holds the size until then: tools/pvz2-macos.m). */
static atomic_bool g_save_gate_read, g_started;
int tl_pvz2_started(void) { return atomic_load(&g_started); }

static void J_config_exists(tl_jcall *c)
{
    const char *k = S(c->args[0].l);
    if (!strcmp(k, "prevent_load_game")) atomic_store(&g_save_gate_read, true);
    poll_trace("ConfigKeyExists", k);
    pthread_mutex_lock(&CFG.mu);
    int there = k[0] && cfg_find(k) != NULL;
    pthread_mutex_unlock(&CFG.mu);
    static char seen[256][64]; static int nseen;
    int known = 0;
    for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], k)) { known = 1; break; }
    if (!known && nseen < 256) { snprintf(seen[nseen++], 64, "%s", k); tl_log_line("pvz2: ConfigKeyExists(\"%s\") -> %d", k, there); }
    else if (getenv("TL_PVZ2_CFG_TRACE")) tl_log_line("pvz2: ConfigKeyExists(\"%s\") -> %d", k, there);
    /* The CDN's own gate: the guest requests the CDN only when it cannot read
     * "CDN Version", so this one line says which way the guest went. */
    if (strstr(k, "CDN")) tl_log_line("pvz2: ConfigKeyExists(\"%s\") -> %d   (the CDN gate)", k, there);
    c->ret = vz(there);
}
static void J_config_erase(tl_jcall *c)
{
    const char *k = S(c->args[0].l);
    pthread_mutex_lock(&CFG.mu);
    for (int i = 0; i < CFG.n; i++) if (!strcmp(CFG.v[i].key, k)) { free(CFG.v[i].val); CFG.v[i].val = NULL; CFG.v[i].is_int = 0; CFG.v[i].num = 0; }
    pthread_mutex_unlock(&CFG.mu);
    cfg_save();
}
static void J_config_read_str(tl_jcall *c)
{
    const char *k = S(c->args[0].l);
    pthread_mutex_lock(&CFG.mu);
    setting *e = cfg_find(k);
    jobj *r = e && e->val ? str(e->val) : NULL;
    pthread_mutex_unlock(&CFG.mu);
    c->ret = vl(r);
}
static void J_config_read_int(tl_jcall *c)
{
    const char *k = S(c->args[0].l);
    pthread_mutex_lock(&CFG.mu);
    setting *e = cfg_find(k);
    int32_t v = e ? e->num : 0;
    pthread_mutex_unlock(&CFG.mu);
    c->ret = vi(v);
}
static void J_config_read_bool(tl_jcall *c)
{
    const char *k = S(c->args[0].l);
    poll_trace("ConfigReadBoolean", k);
    pthread_mutex_lock(&CFG.mu);
    setting *e = cfg_find(k);
    int v = e && e->num != 0;
    pthread_mutex_unlock(&CFG.mu);
    static char seen[32][64]; static int nseen;
    int known = 0;
    for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], k)) { known = 1; break; }
    if (!known && nseen < 32) { snprintf(seen[nseen++], 64, "%s", k); tl_log_line("pvz2: ConfigReadBoolean(\"%s\") -> %d", k, v); }
    c->ret = vz(v);
}
static void J_config_write_str(tl_jcall *c)
{
    const char *k = S(c->args[0].l);
    char *v = strdup(S(c->args[1].l));
    tl_log_line("pvz2: ConfigWriteString(\"%s\", \"%s\")", k, v);
    setting s = { .val = v, .is_int = 0 };
    cfg_put(k, s);
    cfg_save();
    c->ret = vz(1);
}
static void J_config_write_int(tl_jcall *c)
{
    const char *k = S(c->args[0].l);
    tl_log_line("pvz2: ConfigWriteInteger(\"%s\", %d)", k, c->args[1].i);
    setting s = { .is_int = 1, .num = c->args[1].i };
    cfg_put(k, s);
    cfg_save();
    c->ret = vz(1);
}
static void J_config_write_bool(tl_jcall *c)
{
    const char *k = S(c->args[0].l);
    tl_log_line("pvz2: ConfigWriteBoolean(\"%s\", %d)", k, c->args[1].z != 0);
    setting s = { .is_int = 1, .num = c->args[1].z != 0 };
    cfg_put(k, s);
    cfg_save();
    c->ret = vz(1);
}

/* The engine's input queue: AndroidUIEventManager fills a direct ByteBuffer
 * through UI_ProcessEvents.  The buffer is 1024 bytes: a uint32 count at +0 and
 * 48-byte touch / 32-byte key records from +16.  The record layouts are the
 * ones the engine's own dispatch handlers step over (see PvZ-2-RE's
 * input_queue.cpp); a wrong size desynchronises every later record. */
typedef struct { uint32_t id; int phase, x, y, px, py; double time_s; } pvz_touch;
typedef struct { char text[64]; int action; double time_s; } pvz_text;
typedef struct { int keycode, down; double time_s; } pvz_key;
static struct {
    pvz_touch v[256];
    int n;
    uint32_t next_id, active_id;
    /* The guest's finger for each of the host's fingers. The host has one
     * finger (the mouse), but it can press again before a release has gone
     * out -- the shovel's tap is released 120 ms later, and the plant's drag
     * is released after its steps -- and the release has to go to the finger
     * that was pressed, not to the newest one. Without this, a finger stays
     * down in the game for ever. */
    uint32_t finger[8];
    int last_x, last_y;
    pthread_mutex_t mu;
} TQ = { .next_id = 1, .active_id = 1, .mu = PTHREAD_MUTEX_INITIALIZER };
static struct {
    pvz_text v[32];
    int n;
    pthread_mutex_t mu;
} TXQ = { .mu = PTHREAD_MUTEX_INITIALIZER };
static struct {
    pvz_key v[32];
    int n;
    pthread_mutex_t mu;
} KQ = { .mu = PTHREAD_MUTEX_INITIALIZER };

/* AndroidUIEventManager.GetTimeStamp() is System.nanoTime()/1e9: monotonic
 * seconds, not epoch milliseconds.  The engine compares the value against its
 * own clock; a different epoch makes every gesture look ancient. */
static double now_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1000000000.0;
}

static uint32_t touch_id_for(int phase, int host_id)
{
    int slot = (host_id >= 0 && host_id < 8) ? host_id : 0;
    if (phase != 0) {
        /* The move and the release belong to the finger that was pressed. */
        if (TQ.finger[slot]) return TQ.finger[slot];
        return TQ.active_id;
    }
    uint32_t id = TQ.next_id++;
    if (id == 0) id = TQ.next_id++;
    TQ.finger[slot] = id;
    TQ.active_id = id;
    return id;
}

static void J_process_events(tl_jcall *c)
{
    static int calls;
    jobj *b = c->args[0].l;
    void *addr = b ? tl_jni_get_field(b, "address", "J").l : NULL;
    int64_t cap = b ? tl_jni_get_field(b, "capacity", "J").j : 0;
    if (!addr || cap < 16) { c->ret = vz(0); return; }

    uint32_t limit = (uint32_t)(cap > 1024 ? 1024 : cap);
    memset(addr, 0, limit);
    uint8_t *p = (uint8_t *)addr + 16;
    uint32_t count = 0;
    size_t used = 0;
    pthread_mutex_lock(&TQ.mu);
    while (TQ.n > 0 && used + 48 <= limit - 16) {
        pvz_touch t = TQ.v[0];
        memmove(&TQ.v[0], &TQ.v[1], (size_t)(TQ.n - 1) * sizeof(TQ.v[0]));
        TQ.n--;
        uint32_t *w = (uint32_t *)p;
        w[0] = 0;              /* +0  type: touch */
        w[1] = t.id;           /* +4  touch id (never 0) */
        w[2] = (uint32_t)t.x;  /* +8  x */
        w[3] = (uint32_t)t.y;  /* +12 y */
        w[4] = (uint32_t)t.px; /* +16 previous x */
        w[5] = (uint32_t)t.py; /* +20 previous y */
        w[6] = 1;              /* +24 tap count */
        uint64_t bits; memcpy(&bits, &t.time_s, 8);
        w[7] = (uint32_t)(bits & 0xffffffffu);  /* +28 time low */
        w[8] = (uint32_t)(bits >> 32);          /* +32 time high */
        w[9] = (uint32_t)t.phase;               /* +36 phase */
        w[10] = 0xdeadbeef; w[11] = 0xdeadbeef; /* +40, +44 sentinels */
        p += 48; used += 48; count++;
    }
    pthread_mutex_unlock(&TQ.mu);
    pthread_mutex_lock(&TXQ.mu);
    while (TXQ.n > 0) {
        pvz_text t = TXQ.v[0];
        size_t len = strlen(t.text);
        size_t need = 12 + len + ((4 - len % 4) % 4);
        if (used + need > limit - 16) break;
        memmove(&TXQ.v[0], &TXQ.v[1], (size_t)(TXQ.n - 1) * sizeof(TXQ.v[0]));
        TXQ.n--;
        uint32_t *w = (uint32_t *)p;
        w[0] = 6; w[1] = (uint32_t)t.action; w[2] = (uint32_t)len;
        memcpy(p + 12, t.text, len);
        for (size_t pad = (4 - len % 4) % 4; pad; pad--) p[12 + len++] = 0;
        p += need; used += need; count++;
    }
    pthread_mutex_unlock(&TXQ.mu);
    pthread_mutex_lock(&KQ.mu);
    while (KQ.n > 0 && used + 32 <= limit - 16) {
        pvz_key k = KQ.v[0];
        memmove(&KQ.v[0], &KQ.v[1], (size_t)(KQ.n - 1) * sizeof(KQ.v[0]));
        KQ.n--;
        uint32_t *w = (uint32_t *)p;
        /* The engine's key record: the type, the Android keycode, the
         * down/up flag, and then a word that MUST BE ZERO -- a non-zero value
         * there makes the engine drop the key without dispatching it. The
         * harness had the flag and the zero the other way round, so every key
         * event was thrown away: the ENTER that commits a name, the BACKSPACE
         * that deletes one, the BACK that leaves a screen. */
        w[0] = 1; w[1] = (uint32_t)k.keycode; w[2] = (uint32_t)k.down; w[3] = 0;
        uint64_t bits; memcpy(&bits, &k.time_s, 8);
        w[4] = (uint32_t)(bits & 0xffffffffu); w[5] = (uint32_t)(bits >> 32);
        w[6] = 0; w[7] = 0xdeadbeef;
        p += 32; used += 32; count++;
    }
    pthread_mutex_unlock(&KQ.mu);
    *(uint32_t *)addr = count;
    if (count || (++calls % 600) == 0)
        tl_log_line("pvz2: UI_ProcessEvents call %d -> %u event(s)", calls, count);
    c->ret = vz(count != 0);
}
static void J_blocks(tl_jcall *c) { (void)c; c->ret = vj(1000000); }
static void J_blocks_free(tl_jcall *c) { (void)c; c->ret = vj(500000); }
static void J_block_size(tl_jcall *c) { (void)c; c->ret = vj(4096); }

static void J_holder(tl_jcall *c) { (void)c; c->ret = vl(P.holder); }
static void J_surface(tl_jcall *c) { (void)c; c->ret = vl(P.surface); }
static void J_context(tl_jcall *c) { (void)c; c->ret = vl(P.activity); }
static void J_width(tl_jcall *c) { (void)c; c->ret = vi(P.w); }
static void J_height(tl_jcall *c) { (void)c; c->ret = vi(P.h); }
static void J_set_scale(tl_jcall *c)
{
    float v = c->args[0].f;
    if (v != P.scale) tl_log_line("pvz2: GLViewScaleFactor -> %g", (double)v);
    P.scale = v; c->ret = vi(0);
}
static void J_get_scale(tl_jcall *c) { jvalue v; v.f = P.scale; c->ret = v; }

/* AssetManager lookups: Reflourished keeps its resources in the OBB, so a miss
 * here is not fatal; answer "not in the APK" cleanly. */
static void J_asset_info(tl_jcall *c) { tl_log_line("pvz2: asset info \"%s\"", S(c->args[0].l)); c->ret = vl(NULL); }
static void J_asset_size(tl_jcall *c) { tl_log_line("pvz2: asset size \"%s\"", S(c->args[0].l)); c->ret = vj(-1); }

/* HTTP: the transaction object exists and requests fail at once, except for an
 * optional cached Reflourished CDN package. A no-op Start would leave the
 * engine waiting forever on the loading screen: on a device the transaction
 * either completes or errors, and only the engine's own callbacks move it on.
 * The main.tar is replayed from a local copy (the same body the CDN serves);
 * everything else is failed immediately. */
typedef struct { jobj *obj; uint64_t native; char url[512]; int status; long length;
                 unsigned char *body; size_t body_len; } http_txn;
static struct { http_txn v[64]; int n; pthread_mutex_t mu; } HTX = { .mu = PTHREAD_MUTEX_INITIALIZER };

static void http_put(jobj *o, uint64_t native, const char *url)
{
    pthread_mutex_lock(&HTX.mu);
    int slot = -1;
    for (int i = 0; i < HTX.n; i++) if (HTX.v[i].obj == o) { slot = i; break; }
    if (slot < 0) {
        if (HTX.n == 64) {
            /* The table is full: this request is dropped, and the guest waits
             * for an answer that never comes. It is said once, because a
             * silent failure here looks like a game that has stopped
             * downloading anything. */
            static int said = 0;
            if (!said) {
                said = 1;
                tl_log_line("net: the transaction table is full (64); a request is dropped and the guest is not told");
            }
            pthread_mutex_unlock(&HTX.mu);
            return;
        }
        slot = HTX.n++;
        memset(&HTX.v[slot], 0, sizeof(HTX.v[slot]));
    } else {
        free(HTX.v[slot].body); HTX.v[slot].body = NULL; HTX.v[slot].body_len = 0;
    }
    HTX.v[slot].obj = o; HTX.v[slot].native = native;
    snprintf(HTX.v[slot].url, sizeof(HTX.v[slot].url), "%s", url ? url : "");
    HTX.v[slot].status = 0; HTX.v[slot].length = 0;
    pthread_mutex_unlock(&HTX.mu);
}
/* The body of a request. The guest's Java keeps it in the transaction and
 * sets "do output" on the connection; the host replaces that method (the
 * connection is the host's), so the host keeps the body itself -- otherwise a
 * POST would go out empty. */
static void http_set_body(jobj *o, const unsigned char *data, size_t len)
{
    pthread_mutex_lock(&HTX.mu);
    for (int i = 0; i < HTX.n; i++) if (HTX.v[i].obj == o) {
        free(HTX.v[i].body);
        HTX.v[i].body = NULL; HTX.v[i].body_len = 0;
        if (len && data) {
            HTX.v[i].body = malloc(len);
            /* A body that was not copied is no body: saying otherwise would
             * hand the guest an uninitialised buffer as its request. */
            if (HTX.v[i].body) { memcpy(HTX.v[i].body, data, len); HTX.v[i].body_len = len; }
        }
        break;
    }
    pthread_mutex_unlock(&HTX.mu);
}
static int http_info(jobj *o, char *url, size_t urlsz, long *length)
{
    int found = 0;
    pthread_mutex_lock(&HTX.mu);
    for (int i = 0; i < HTX.n; i++) if (HTX.v[i].obj == o) {
        if (url && urlsz) snprintf(url, urlsz, "%s", HTX.v[i].url);
        if (length) *length = HTX.v[i].length;
        found = HTX.v[i].status;
        break;
    }
    pthread_mutex_unlock(&HTX.mu);
    return found;
}
static void http_set(jobj *o, int status, long length)
{
    pthread_mutex_lock(&HTX.mu);
    for (int i = 0; i < HTX.n; i++) if (HTX.v[i].obj == o) { HTX.v[i].status = status; HTX.v[i].length = length; break; }
    pthread_mutex_unlock(&HTX.mu);
}
static void http_forget(jobj *o)
{
    pthread_mutex_lock(&HTX.mu);
    for (int i = 0; i < HTX.n; i++) if (HTX.v[i].obj == o) {
        free(HTX.v[i].body);
        HTX.v[i] = HTX.v[--HTX.n];
        /* The vacated slot must be cleared: it would otherwise keep a copy of
         * the body pointer that was just freed, and the next transaction into
         * that slot would free it a second time. */
        memset(&HTX.v[HTX.n], 0, sizeof(HTX.v[0]));
        break;
    }
    pthread_mutex_unlock(&HTX.mu);
}
/* The request body, as the host has it. Only the length is handed out: the
 * pointer would be used after the lock is dropped, and the body is freed when
 * the transaction goes. */
static size_t http_body_len(jobj *o)
{
    size_t len = 0;
    pthread_mutex_lock(&HTX.mu);
    for (int i = 0; i < HTX.n; i++) if (HTX.v[i].obj == o) { len = HTX.v[i].body_len; break; }
    pthread_mutex_unlock(&HTX.mu);
    return len;
}
static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64 * 1024 * 1024) { fclose(f); return NULL; }
    unsigned char *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f); fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    *len = got;
    return buf;
}
static void J_http_ctor(tl_jcall *c)
{
    const char *url = c->args[2].l ? S(c->args[2].l) : "";
    http_put(c->self, (uint64_t)c->args[0].j, url);
    tl_log_line("pvz2: HTTP transaction native=%#llx %s", (unsigned long long)c->args[0].j, url);
    c->ret = vi(0);
}
static void J_http_start(tl_jcall *c)
{
    char url[512] = ""; long length = 0;
    http_info(c->self, url, sizeof(url), &length);
    typedef void (*cb1_fn)(void *, void *, int64_t);
    typedef void (*cbdata_fn)(void *, void *, int64_t, void *, int32_t);
    uint64_t native = 0;
    pthread_mutex_lock(&HTX.mu);
    for (int i = 0; i < HTX.n; i++) if (HTX.v[i].obj == c->self) { native = HTX.v[i].native; break; }
    pthread_mutex_unlock(&HTX.mu);

    const char *path = getenv("TL_PVZ2_MAIN_TAR");
    char local[700];
    if (!path) { snprintf(local, sizeof(local), "%s/No_Backup/1.4.2.main.tar", P.data); path = local; }
    size_t n = 0;
    unsigned char *body = strstr(url, "main.tar") ? read_file(path, &n) : NULL;
    if (body) {
        cb1_fn resp = (cb1_fn)tl_jni_native(TXN, "HttpReceivedResponse", "(J)V");
        cbdata_fn data = (cbdata_fn)tl_jni_native(TXN, "HttpReceivedData", "(J[BI)V");
        cb1_fn done = (cb1_fn)tl_jni_native(TXN, "HttpTransactionComplete", "(J)V");
        tl_log_line("pvz2: HTTP callbacks resp=%p data=%p done=%p", (void *)resp, (void *)data, (void *)done);
        http_set(c->self, 200, (long)n);
        tl_log_line("pvz2: HTTP main.tar -> %zu bytes from %s", n, path);
        if (resp) resp(tl_jni_env(), c->self, (int64_t)native);
        if (data) {
            /* Java's AndroidHttpTransaction reuses one 16 KiB byte[] for every
             * read; do the same. */
            jobj *arr = tl_jni_new_prim_array('B', 16384);
            int chunks = 0;
            for (size_t off = 0; off < n; off += 16384) {
                size_t chunk = n - off > 16384 ? 16384 : n - off;
                memcpy(arr->arr.data, body + off, chunk);
                data(tl_jni_env(), c->self, (int64_t)native, arr, (int32_t)chunk);
                chunks++;
            }
            /* Say how much the guest was handed: a guest that does not accumulate it
             * writes an empty file, and this says whether the data was delivered. */
            tl_log_line("pvz2: HTTP delivered %d chunk(s) of %zu bytes to the guest", chunks, n);
        }
        if (done) done(tl_jni_env(), c->self, (int64_t)native);
        free(body);
    } else {
        cb1_fn err = (cb1_fn)tl_jni_native(TXN, "HttpTransactionError", "(J)V");
        size_t body_len = http_body_len(c->self);
        tl_log_line("pvz2: HTTP fail %s (native=%#llx)%s", url, (unsigned long long)native,
                    body_len ? " -- and the request had a body this host cannot send" : "");
        if (err && native) err(tl_jni_env(), c->self, (int64_t)native);
    }
    c->ret = vi(0);
}
static void J_http_release(tl_jcall *c) { http_forget(c->self); c->ret = vi(0); }
/* The request body: kept by the host, because the host replaces the guest's
 * Java method (the connection is the host's, so the guest's Java body never
 * runs). Without this a POST would go out empty -- the CDN is a GET, so this
 * is the mod's cloud upload that would break. */
static void J_http_set_body(tl_jcall *c)
{
    static bool logged = false;
    jobj *arr = (jobj *)c->args[0].l;
    const unsigned char *data = NULL;
    size_t len = 0;
    if (arr && arr->kind == TL_K_PRIM_ARRAY && arr->arr.data)
        { data = arr->arr.data; len = (size_t)arr->arr.len * (arr->arr.esz ? arr->arr.esz : 1); }
    http_set_body(c->self, data, len);
    if (!logged) { logged = true; tl_log_line("pvz2: a request body of %zu bytes was set", len); }
}
/* ---------------------------------------------------------------- reminders
 *
 * The host has no notification centre, so the daily-challenge reminders do
 * not fire. The guest asks the host to schedule them and to tell it what is
 * scheduled; both answers have to be ones the guest can carry on with. The
 * guest's own Java builds the scheduled list as the items' userInfo in
 * brackets -- and "[]" when there is nothing -- so "[]" is what the host
 * answers with. */
static void J_notif_schedule(tl_jcall *c)
{
    static bool logged = false;
    (void)c;
    if (!logged) {
        logged = true;
        tl_log_line("pvz2: a reminder was scheduled; this host has no notification centre, so it will not fire");
    }
}
static void J_notif_scheduled_data(tl_jcall *c)
{
    static bool logged = false;
    if (!logged) {
        logged = true;
        tl_log_line("pvz2: the scheduled reminders are asked for; this host has none");
    }
    c->ret = vl(str("[]"));
}
/* ---------------------------------------------------------------- the cloud
 *
 * The cloud save needs an auth code from Google Play. There is no Google Play
 * here, so the answer is the one the guest itself gives when the player is
 * not signed in: an empty code (the guest's own GameHelper calls the
 * callback with "" in that case). The callback is answered, so the game's
 * cloud save does not wait for an answer that will never come. */
static void J_cloud_request_auth_code(tl_jcall *c)
{
    static bool logged = false;
    jobj *cb = (jobj *)c->args[1].l;
    typedef void (*ac_fn)(void *, void *, void *);
    ac_fn fn = (ac_fn)tl_jni_native("com/popcap/SexyAppFramework/cloud/AuthCodeRequestCallback",
                                    "Native_AuthCodeRetrieved", "(Ljava/lang/String;)V");
    if (!logged) {
        logged = true;
        tl_log_line("pvz2: a cloud auth code was asked for; there is no Google Play here, so the answer is an empty code%s",
                    fn ? "" : " (and the guest has no handler for it)");
    }
    if (fn && cb) fn(tl_jni_env(), cb, str(""));
}
static void J_http_status(tl_jcall *c)
{
    long len = 0; int st = http_info(c->self, NULL, 0, &len);
    c->ret = vi(st);
}
static void J_http_length(tl_jcall *c)
{
    long len = 0; http_info(c->self, NULL, 0, &len);
    c->ret = vi((int)len);
}
static void J_http_statusline(tl_jcall *c) { (void)c; c->ret = vl(str("OK")); }
static void J_http_header(tl_jcall *c)
{
    const char *key = S(c->args[0].l);
    long len = 0; char url[512] = ""; http_info(c->self, url, sizeof(url), &len);
    if (!strcasecmp(key, "Content-Length")) { char b[32]; snprintf(b, sizeof(b), "%ld", len); c->ret = vl(str(b)); }
    else if (!strcasecmp(key, "Content-Type")) c->ret = vl(str("application/x-tar"));
    else if (!strcasecmp(key, "ETag")) c->ret = vl(str("\"8e4abd94e352d9301e15428c4021c177\""));
    else c->ret = vl(str(""));
}
static void J_http_url(tl_jcall *c)
{
    char url[512] = ""; http_info(c->self, url, sizeof(url), NULL);
    c->ret = vl(str(url));
}

/* ------------------------------------------------------------ Nimble broadcasts
 *
 * Nimble's components talk through Utility.registerReceiver() and
 * sendBroadcastSerializable().  A receiver is a BaseNativeCallback, and the int
 * its constructor got is the callback id.  A delivery calls the stock
 * BaseNativeCallback.nativeCallback export with args = { action, extras }, on the
 * GL thread between frames, never from inside the Java call that queued it.
 * Events carry only what the engine sent; nothing is invented here. */
#define NB_RECEIVERS 64
#define NB_QUEUE 32
#define NB_ACTION 160
typedef void (*nb_callback_fn)(void *env, jobj *receiver, int32_t id, jobj *args);
typedef void (*nb_life_fn)(void *env, jobj *self, jobj *arg);
typedef struct { char action[NB_ACTION]; jobj *receiver; int32_t id; } nb_receiver;
typedef struct { char action[NB_ACTION]; jobj *extras; } nb_event;
static struct {
    pthread_mutex_t mu;
    nb_receiver rx[NB_RECEIVERS];
    int nrx;
    nb_event q[NB_QUEUE];
    int qhead, qn;
} NB = { .mu = PTHREAD_MUTEX_INITIALIZER };
static nb_callback_fn nb_native;

/* java.util.HashMap, as much as Nimble's extras need: put, get, size, containsKey. */
typedef struct { int n; char *key[16]; jobj *val[16]; } nb_map;
static jobj *map_new(void)
{
    jobj *m = tl_jni_new_object(tl_jni_class("java/util/HashMap"));
    m->native = calloc(1, sizeof(nb_map));
    return m;
}
static nb_map *map_of(const tl_jcall *c) { return c->self ? c->self->native : NULL; }
static int map_find(const nb_map *m, const char *k)
{
    for (int i = 0; i < m->n; i++) if (!strcmp(m->key[i], k)) return i;
    return -1;
}
static void J_map_init(tl_jcall *c) { c->self->native = calloc(1, sizeof(nb_map)); }
static void J_map_put(tl_jcall *c)
{
    nb_map *m = map_of(c);
    const char *k = S(c->args[0].l);
    jobj *v = c->args[1].l ? tl_jni_ref(c->args[1].l) : NULL;
    c->ret = vl(NULL);
    if (!m) return;
    int i = map_find(m, k);
    if (i >= 0) { c->ret = vl(m->val[i]); m->val[i] = v; return; }
    if (m->n < 16) { m->key[m->n] = strdup(k); m->val[m->n] = v; m->n++; }
}
static void J_map_get(tl_jcall *c)
{
    nb_map *m = map_of(c);
    int i = m ? map_find(m, S(c->args[0].l)) : -1;
    c->ret = vl(i >= 0 ? m->val[i] : NULL);
}
static void J_map_size(tl_jcall *c) { nb_map *m = map_of(c); c->ret = vi(m ? m->n : 0); }
static void J_map_contains(tl_jcall *c) { nb_map *m = map_of(c); c->ret = vz(m && map_find(m, S(c->args[0].l)) >= 0); }

static void nb_resolve(void)
{
    if (nb_native || !GAME_LIB) return;
    nb_native = (nb_callback_fn)tl_ld_sym(GAME_LIB, "Java_com_ea_nimble_bridge_BaseNativeCallback_nativeCallback");
    if (!nb_native) nb_native = (nb_callback_fn)tl_jni_native("com/ea/nimble/bridge/BaseNativeCallback", "nativeCallback", NULL);
    tl_log_line("pvz2: BaseNativeCallback.nativeCallback %s", nb_native ? "found" : "missing");
}

static void nb_call(jobj *receiver, int32_t id, const char *action, jobj *extras)
{
    nb_resolve();
    if (!nb_native) { tl_log_line("pvz2: broadcast %s dropped (no nativeCallback)", action); return; }
    jobj *args = tl_jni_new_obj_array(tl_jni_class("java/lang/Object"), 2);
    args->oarr.v[0] = tl_jni_new_string(action);
    args->oarr.v[1] = extras ? tl_jni_ref(extras) : map_new();
    nb_native(tl_jni_env(), receiver, id, args);
}

/* Deliver queued broadcasts to a snapshot of the matching receivers, so a
 * receiver that unregisters from its own callback cannot disturb the loop. */
static void nb_pump(void)
{
    for (;;) {
        nb_event ev;
        nb_receiver hit[NB_RECEIVERS];
        int n = 0;
        pthread_mutex_lock(&NB.mu);
        if (NB.qn == 0) { pthread_mutex_unlock(&NB.mu); break; }
        ev = NB.q[NB.qhead];
        NB.qhead = (NB.qhead + 1) % NB_QUEUE;
        NB.qn--;
        for (int i = 0; i < NB.nrx; i++) if (!strcmp(NB.rx[i].action, ev.action)) hit[n++] = NB.rx[i];
        pthread_mutex_unlock(&NB.mu);
        tl_log_line("pvz2: broadcast %s -> %d receiver(s)", ev.action, n);
        for (int i = 0; i < n; i++) nb_call(hit[i].receiver, hit[i].id, ev.action, ev.extras);
    }
}

static void J_base_cb_init(tl_jcall *c) { c->self->native = (void *)(intptr_t)c->args[0].i; }

static void J_register_receiver(tl_jcall *c)
{
    const char *action = S(c->args[0].l);
    jobj *rcv = c->args[1].l;
    if (!rcv || !action[0]) return;
    pthread_mutex_lock(&NB.mu);
    if (NB.nrx < NB_RECEIVERS) {
        nb_receiver *r = &NB.rx[NB.nrx++];
        snprintf(r->action, sizeof(r->action), "%s", action);
        r->receiver = tl_jni_ref(rcv);
        r->id = (int32_t)(intptr_t)rcv->native;
    }
    pthread_mutex_unlock(&NB.mu);
    tl_log_line("pvz2: registerReceiver(%s) callback %d", action, (int32_t)(intptr_t)rcv->native);
}

static void J_unregister_receiver(tl_jcall *c)
{
    jobj *rcv = c->args[0].l;
    pthread_mutex_lock(&NB.mu);
    for (int i = 0; i < NB.nrx;) {
        if (NB.rx[i].receiver == rcv) {
            memmove(&NB.rx[i], &NB.rx[i + 1], (size_t)(NB.nrx - i - 1) * sizeof(nb_receiver));
            NB.nrx--;
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&NB.mu);
}

static void nb_queue(const char *action, jobj *extras)
{
    pthread_mutex_lock(&NB.mu);
    if (action[0] && NB.qn < NB_QUEUE) {
        nb_event *e = &NB.q[(NB.qhead + NB.qn) % NB_QUEUE];
        snprintf(e->action, sizeof(e->action), "%s", action);
        e->extras = extras ? tl_jni_ref(extras) : NULL;
        NB.qn++;
    } else {
        tl_log_line("pvz2: broadcast queue full, dropped %s", action);
    }
    pthread_mutex_unlock(&NB.mu);
}

static void J_send_broadcast(tl_jcall *c) { nb_queue(S(c->args[0].l), c->args[1].l); }

/* Base.setupNimble's first effect, once, before the first frame: the
 * componentIndependentSetupFinished event to callback 1 with no receiver and an
 * empty map.  Then the Nimble application lifecycle resumes. */
static void nb_independent_setup(void)
{
    nb_resolve();
    nb_call(NULL, 1, "nimble.notification.componentIndependentSetupFinished", NULL);
    /* Reflourished's classes.dex patches SynergyEnvironmentImpl's
     * startSynergyEnvironmentUpdate()V to return immediately (first code unit
     * 0x000e, verified in 1.4.2). No request starts and no STARTED/FINISHED
     * notification is sent. Do not reproduce the unreachable stock EA code. */
    /* The first network status, queued once setup has run (the port does the same). */
    nb_queue("nimble.notification.networkStatusChange", NULL);
    nb_life_fn resume = (nb_life_fn)tl_ld_sym(GAME_LIB, "Java_com_ea_nimble_bridge_NimbleCppApplicationLifeCycle_onApplicationResume");
    if (resume) { tl_log_line("pvz2: Nimble onApplicationResume"); resume(tl_jni_env(), NULL, NULL); }
}

/* The engine times itself per frame, and a phone's GLSurfaceView is vsync-paced,
 * so run at the display rate instead of as fast as the GPU allows (TL_PVZ2_FPS). */
static void pace_frame(long frame_ns)
{
    static struct timespec due;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (due.tv_sec == 0 && due.tv_nsec == 0) due = now;
    due.tv_nsec += frame_ns;
    while (due.tv_nsec >= 1000000000L) { due.tv_nsec -= 1000000000L; due.tv_sec++; }
    long long wait = (due.tv_sec - now.tv_sec) * 1000000000LL + (due.tv_nsec - now.tv_nsec);
    if (wait > 0) {
        struct timespec d = { (time_t)(wait / 1000000000LL), (long)(wait % 1000000000LL) };
        nanosleep(&d, NULL);
    } else if (wait < -4 * frame_ns) {
        due = now;
    }
}

/* ------------------------------------------------------ Nimble persistence
 *
 * PersistenceService hands each Nimble component a Persistence scoped to
 * "nimble.persistence.<component>".  Values live in memory for this run; writing
 * them to the data directory belongs to the save work. */
typedef struct { char scope[128]; } ps_scope_t;
typedef struct { char *key; jobj *value; } ps_entry;
#define PS_CAPACITY 256
static struct { pthread_mutex_t mu; ps_entry v[PS_CAPACITY]; int n; } PS = { .mu = PTHREAD_MUTEX_INITIALIZER };

static jobj *ps_new(const char *component)
{
    jobj *p = tl_jni_new_object(tl_jni_class("com/ea/nimble/Persistence"));
    ps_scope_t *s = calloc(1, sizeof(*s));
    snprintf(s->scope, sizeof(s->scope), "nimble.persistence.%s", component);
    p->native = s;
    return p;
}

static const char *ps_scope_of(const jobj *p) { return p && p->native ? ((const ps_scope_t *)p->native)->scope : ""; }

static void ps_set(const char *scope, const char *key, jobj *value)
{
    char full[512];
    snprintf(full, sizeof(full), "%s\x1f%s", scope, key);
    pthread_mutex_lock(&PS.mu);
    int i = 0;
    while (i < PS.n && strcmp(PS.v[i].key, full)) i++;
    if (i < PS.n) {
        if (PS.v[i].value) tl_jni_unref(PS.v[i].value);
        PS.v[i].value = value ? tl_jni_ref(value) : NULL;
    } else if (value && PS.n < PS_CAPACITY) {
        PS.v[PS.n].key = strdup(full);
        PS.v[PS.n].value = tl_jni_ref(value);
        PS.n++;
    } else if (value) {
        tl_log_line("pvz2: persistence full, dropped %s", full);
    }
    pthread_mutex_unlock(&PS.mu);
}

static jobj *ps_get(const char *scope, const char *key)
{
    char full[512];
    snprintf(full, sizeof(full), "%s\x1f%s", scope, key);
    jobj *v = NULL;
    pthread_mutex_lock(&PS.mu);
    for (int i = 0; i < PS.n; i++) if (!strcmp(PS.v[i].key, full)) { v = PS.v[i].value ? tl_jni_ref(PS.v[i].value) : NULL; break; }
    pthread_mutex_unlock(&PS.mu);
    return v;
}

static void J_ps_for_component(tl_jcall *c) { c->ret = vl(ps_new(S(c->args[0].l))); }
static void J_ps_set_value(tl_jcall *c)
{
    const char *key = S(c->args[0].l);
    const char *cls = c->args[1].l ? tl_jni_class_name(c->args[1].l) : "null";
    tl_log_line("pvz2: persistence %s.%s = %s %s", ps_scope_of(c->self), key, cls,
                c->args[1].l && !strcmp(cls, "java/lang/String") ? S(c->args[1].l) : "");
    ps_set(ps_scope_of(c->self), key, c->args[1].l);
}
static void J_ps_get_value(tl_jcall *c)
{
    const char *key = S(c->args[0].l);
    jobj *v = ps_get(ps_scope_of(c->self), key);
    tl_log_line("pvz2: persistence get %s.%s -> %s", ps_scope_of(c->self), key, v ? tl_jni_class_name(v) : "null");
    c->ret = vl(v);
}

/* Synergy has no environment data without its server, so the component exists
 * and reports isDataAvailable() == false. */
static void J_synergy_component(tl_jcall *c) { c->ret = vl(tl_jni_new_object(tl_jni_class("com/ea/nimble/ISynergyEnvironment"))); }

/* ------------------------------------------------------- com.ea.nimble.Error
 *
 * The engine reports failures through Nimble's Error (domain, code, message) and
 * logs getDomain()/getCode().  Keep what the engine sends, so the log says what
 * really failed instead of printing blanks. */
typedef struct { char *domain; int32_t code; } nb_error;
static void J_nb_error_init(tl_jcall *c)
{
    nb_error *e = calloc(1, sizeof(*e));
    e->domain = strdup(S(c->args[0].l));
    e->code = c->args[1].i;
    c->self->native = e;
    tl_jni_set_field(c->self, "detailMessage", "Ljava/lang/String;", vl(tl_jni_ref(c->args[2].l)));
}
static void J_nb_error_domain(tl_jcall *c)
{
    nb_error *e = c->self ? c->self->native : NULL;
    c->ret = vl(tl_jni_new_string(e && e->domain ? e->domain : ""));
}
static void J_nb_error_code(tl_jcall *c)
{
    nb_error *e = c->self ? c->self->native : NULL;
    c->ret = vi(e ? e->code : 0);
}
static void J_throwable_message(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "detailMessage", "Ljava/lang/String;"); }

/* Nimble: the EA service layer.  On a device these are real Java components;
 * here they only need to exist and answer "offline".  The engine calls
 * Nimble.getComponent(id) and then invokes a method on the returned interface,
 * so the object must be of the interface class or GetMethodID fails (and the
 * guest throws an NPE before the menu). */
static void J_nimble_component(tl_jcall *c)
{
    const char *id = S(c->args[0].l);
    const char *cls = "com/ea/nimble/NimbleComponent";
    if (strstr(id, "mtx") || strstr(id, "MTX")) cls = "com/ea/nimble/mtx/INimbleMTX";
    else if (strstr(id, "log") || strstr(id, "Log")) cls = "com/ea/nimble/ILog";
    else if (strstr(id, "network") || strstr(id, "Network")) cls = "com/ea/nimble/INetwork";
    else if (strstr(id, "message") || strstr(id, "Message")) cls = "com/ea/nimble/IMessage";
    else if (strstr(id, "tracking") || strstr(id, "Tracking")) cls = "com/ea/nimble/tracking/ITracking";
    tl_log_line("pvz2: Nimble.getComponent(\"%s\") -> %s", id, cls);
    c->ret = vl(tl_jni_new_object(tl_jni_class(cls)));
}
static void J_mtx_component(tl_jcall *c) { (void)c; c->ret = vl(tl_jni_new_object(tl_jni_class("com/ea/nimble/mtx/INimbleMTX"))); }
static void J_log_component(tl_jcall *c) { (void)c; c->ret = vl(tl_jni_new_object(tl_jni_class("com/ea/nimble/ILog"))); }
static void J_network_component(tl_jcall *c) { (void)c; c->ret = vl(tl_jni_new_object(tl_jni_class("com/ea/nimble/INetwork"))); }
static void J_network_status(tl_jcall *c) { (void)c; c->ret = vl(tl_jni_new_object(tl_jni_class("com/ea/nimble/Network$Status"))); }

/* ------------------------------------------------------------------- tables */

#define M(cls, name, fn) { cls, name, NULL, fn }
static const tl_jhle k_hle[] = {
    /* The Activity: the OBB path is the one that matters (it is how the RSB is
     * found), the rest are the expansion-file questions answered as "present". */
    M(ACT, "FrameworkInfo_SysGetMainExpansionFilePath", J_obb_path),
    M(ACT2, "FrameworkInfo_SysGetMainExpansionFilePath", J_obb_path),
    M(ACT, "expansionFileDelivered", J_true), M(ACT2, "expansionFileDelivered", J_true),
    M(ACT, "isGooglePlayExpansionEnabled", J_true), M(ACT2, "isGooglePlayExpansionEnabled", J_true),
    M(ACT, "needsToUpdateDB", J_false), M(ACT2, "needsToUpdateDB", J_false),
    M(ACT, "fetchExpansionFile", J_true), M(ACT2, "fetchExpansionFile", J_true),
    M(ACT, "needToDownloadExpansionFile", J_false), M(ACT2, "needToDownloadExpansionFile", J_false),

    /* AndroidGameApp: paths, device facts, settings. */
    M(APP, "FrameworkInfo_SysGetMainExpansionFilePath", J_obb_path),
    M(APP, "UI_ProcessEvents", J_process_events),
    M(APP, "Resources_GetAssetFileInfo", J_asset_info),
    M(APP, "Resources_GetAssetFileSize", J_asset_size),
    M(APP, "Resources_GetResourceFolder", J_data_folder),
    M(APP, "Resources_GetUserDataFolder", J_data_folder),
    M(APP, "Resources_GetCacheDataFolder", J_data_folder),
    M(APP, "Resources_GetAppSupportDataFolder", J_data_folder),
    M(APP, "Resources_GetExternalStorageDirectory", J_data_folder),
    M(APP, "Device_GetCachesDir", J_data_folder),
    M(APP, "Resources_GetFileSystemBlockCount", J_blocks),
    M(APP, "Resources_GetFileSystemBlocksFree", J_blocks_free),
    M(APP, "Resources_GetFileSystemBlockSize", J_block_size),
    M(APP, "Device_GetCurrentUIOrientation", J_orientation),
    M(APP, "Device_IsSupportedUIOrientation", J_true),
    M(APP, "Device_IsTablet", J_false),
    M(APP, "Device_GetDeviceName", J_device_name),
    M(APP, "Device_ShowKeyboard", J_show_keyboard),
    M(APP, "Device_ShowNumericKeyboard", J_show_keyboard),
    M(APP, "Device_HideKeyboard", J_hide_keyboard),
    M(APP, "Device_IsKeyboardShowing", J_keyboard_showing),
    M(APP, "Device_KillApplication", J_void),
    M(APP, "Device_ExitToHome", J_void),
    M(APP, "Device_ForceRotateToOrientation", J_void),
    M(APP, "Diag_GetDeviceID", J_device_id),
    /* The calls the game makes that the host answers with nothing: the
     * splash screen (the host has none), the crash-report check (there is no
     * crash report), the notification channels (the daily reminders do not
     * fire), the Nimble component registration (the SDK's own), and the HTTP
     * transaction's timeout (the host's own wait is bounded already). */
    M(APP, "UI_DidRecieveFocus", J_void),
    M(ACT, "getHasCrashReport", J_false),
    M(NOTIF, "<init>", J_void),
    M(NOTIF, "createNotificationChannel", J_void),
    M(NOTIF, "deleteNotificationChannel", J_void),
    M(NOTIF, "scheduleBasicNotification", J_notif_schedule),
    M(NOTIF, "getScheduledLocalNotificationsData", J_notif_scheduled_data),
    M(TXN, "SetTimeout", J_void),
    M(TXN, "SetRequestBody", J_http_set_body),
    M(CLOUD, "Cloud_RequestAuthCode", J_cloud_request_auth_code),
    M(APP, "Diag_GetMemoryUsed", J_memory_used),
    M(APP, "Diag_GetMemoryTotal", J_memory_total),
    M(APP, "Diag_GetHardwareModel", J_hardware),
    M(APP, "Diag_GetOSVersion", J_os_version),
    M(APP, "Util_GetUUIDString", J_uuid),
    M(APP, "Info_SysGetProductVersion", J_product_version),
    M(APP, "Info_SysGetProductVersionString", J_empty),
    M(APP, "Info_SysGetUserCurrencySymbol", J_currency_symbol),
    M(APP, "Info_SysGetUserCurrencyCode", J_currency_code),
    M(APP, "Info_SysGetPackageName", J_package),
    M(APP, "Info_SysGetActivityName", J_activity_name),
    M(APP, "Info_SysGetUserLocale", J_locale),
    M(APP, "Info_SysGetCountryCodeString", J_country),
    M(APP, "Info_SysGetIntentExtraDataString", J_empty),
    M(APP, "Web_SysOpenURL", J_true),
    M(APP, "Config_ConfigKeyExists", J_config_exists),
    M(APP, "Config_ConfigEraseKey", J_config_erase),
    M(APP, "Config_ConfigReadString", J_config_read_str),
    M(APP, "Config_ConfigReadInteger", J_config_read_int),
    M(APP, "Config_ConfigReadBoolean", J_config_read_bool),
    M(APP, "Config_ConfigWriteString", J_config_write_str),
    M(APP, "Config_ConfigWriteInteger", J_config_write_int),
    M(APP, "Config_ConfigWriteBoolean", J_config_write_bool),

    /* The view: sizes and the surface the engine draws into. */
    M(VIEW, "Graphics_GetScreenSizeInPixels", J_screen_size),
    M(VIEW, "Graphics_GetScreenSizeInPoints", J_screen_size),
    M(VIEW, "Graphics_GetSafeRect", J_safe_rect),
    M(VIEW, "Graphics_IsOpenGLES20", J_true),
    M(VIEW, "Graphics_CanSetGLViewScaleFactor", J_true),
    M(VIEW, "Graphics_GetGLViewSysFBO", J_ignore),
    M(VIEW, "Graphics_SetGLViewScaleFactor", J_set_scale),
    M(VIEW, "Graphics_SetFramerate", J_set_framerate),
    M(VIEW, "Graphics_GetGLViewScaleFactor", J_get_scale),
    M(VIEW, "getWidth", J_width),
    M(VIEW, "getHeight", J_height),
    M(VIEW, "getHolder", J_holder),
    M(VIEW, "getContext", J_context),
    M(VIEW, "SetScreenSizeInPixels", J_void),
    M(VIEW, "SetFocusState", J_void),

    /* Android's own view plumbing the engine touches. */
    M("android/view/SurfaceHolder", "getSurface", J_surface),
    M("android/view/Surface", "isValid", J_true),
    M("android/view/Surface", "getWidth", J_width),
    M("android/view/Surface", "getHeight", J_height),
    M("android/view/SurfaceView", "getHolder", J_holder),
    M("android/view/View", "getWidth", J_width),
    M("android/view/View", "getHeight", J_height),
    M("android/view/View", "getContext", J_context),
    M("android/view/View", "queueEvent", J_void),
    M("android/view/View", "requestFocus", J_true),

    /* Services: nobody is signed in and nothing is for sale. */
    M(CLOUD, "Cloud_Connect", J_void),
    M(CLOUD, "Cloud_attemptSilentSync", J_cloud_silent_sync),
    M(CLOUD, "Cloud_SetPcpId", J_void),
    M(CLOUD, "Cloud_SetAge", J_void),
    M(CLOUD, "Cloud_GetPcpId", J_empty),
    M(CLOUD, "Cloud_GetAge", J_empty),
    M(PLAYC, "Play_IsConnected", J_false),
    M(PLAYC, "Play_Connect", J_void),
    M(PLAYC, "Play_Connect_Silent", J_void),
    M(PLAYC, "Play_Disconnect", J_void),
    M(PLAYA, "Play_QueueAchievement_Percentage", J_void),
    M(PLAYA, "Play_ResetAchievements", J_void),
    M(PLAYA, "Play_ShowAchievementView", J_void),
    M(PLAYL, "Play_SubmitScoreToLeaderboard", J_void),
    M(PLAYL, "Play_ShowLeaderboardView", J_void),
    M(PLAYL, "Play_ShowLeaderboardViewAll", J_void),

    /* HTTP: the transaction object exists, requests fail at once, and the game
     * carries on offline (the same answer PvZ 2 RE gives). */
    M(HTTP, "GetNetworkStatus", J_http_network_status),
    M(TXN, "<init>", J_http_ctor),
    M(TXN, "Start", J_http_start),
    M(TXN, "Release", J_http_release),
    M(TXN, "GetStatusCode", J_http_status),
    M(TXN, "GetResponseLength", J_http_length),
    M(TXN, "GetStatusLine", J_http_statusline),
    M(TXN, "GetResponseHeader", J_http_header),
    M(TXN, "GetHumanReadableUrl", J_http_url),

    /* Nimble: the EA service layer.  Its Java classes only need to answer "not
     * available" so the native side takes its offline path. */
    M("com/ea/nimble/Utility", "isNimbleAvailable", J_false),
    M("com/ea/nimble/Utility", "getAppVersion", J_empty),
    M("com/ea/nimble/Utility", "getDeviceString", J_empty),
    M("com/ea/nimble/Utility", "getOsVersion", J_empty),
    M("com/ea/nimble/Utility", "getSDKVersion", J_empty),
    M("com/ea/nimble/bridge/BaseNativeCallback", "isAvailable", J_false),
    M("com/ea/nimble/bridge/NimbleCppComponentRegistrar", "register", J_void),
    M("com/ea/nimble/bridge/BaseNativeCallback", "<init>", J_base_cb_init),
    M("com/ea/nimble/Utility", "registerReceiver", J_register_receiver),
    M("com/ea/nimble/Utility", "unregisterReceiver", J_unregister_receiver),
    M("com/ea/nimble/Utility", "sendBroadcastSerializable", J_send_broadcast),
    M("com/ea/nimble/PersistenceService", "getPersistenceForNimbleComponent", J_ps_for_component),
    M("com/ea/nimble/Persistence", "setValue", J_ps_set_value),
    M("com/ea/nimble/Persistence", "getValue", J_ps_get_value),
    M("com/ea/nimble/Persistence", "setEncryption", J_void),
    M("com/ea/nimble/SynergyEnvironment", "getComponent", J_synergy_component),
    M("com/ea/nimble/ISynergyEnvironment", "isDataAvailable", J_false),
    M("java/util/HashMap", "<init>", J_map_init),
    M("java/util/HashMap", "put", J_map_put),
    M("java/util/HashMap", "get", J_map_get),
    M("java/util/HashMap", "size", J_map_size),
    M("java/util/HashMap", "containsKey", J_map_contains),
    M("java/util/Map", "put", J_map_put),
    M("java/util/Map", "get", J_map_get),
    M("java/util/Map", "size", J_map_size),
    M("java/util/Map", "containsKey", J_map_contains),
    M("com/ea/nimble/Error", "<init>", J_nb_error_init),
    M("com/ea/nimble/Error", "getDomain", J_nb_error_domain),
    M("com/ea/nimble/Error", "getCode", J_nb_error_code),
    M("java/lang/Throwable", "getMessage", J_throwable_message),
    M("com/ea/nimble/Nimble", "getComponent", J_nimble_component),
    M("com/ea/nimble/mtx/NimbleMTX", "getComponent", J_mtx_component),
    M("com/ea/nimble/Log", "getComponent", J_log_component),
    M("com/ea/nimble/Network", "getComponent", J_network_component),
    M("com/ea/nimble/INetwork", "getStatus", J_network_status),
    M("com/ea/nimble/INetwork", "isNetworkWifi", J_false),
    M("com/ea/nimble/Network$Status", "ordinal", J_ignore),
    M("java/lang/Enum", "ordinal", J_ignore),
    M("com/ea/nimble/mtx/INimbleMTX", "refreshAvailableCatalogItems", J_void),
    M("com/ea/nimble/mtx/INimbleMTX", "getRecoveredTransactions", J_ignore),
    M("com/ea/nimble/mtx/INimbleMTX", "getPendingTransactions", J_ignore),
    M("com/ea/nimble/mtx/INimbleMTX", "getAvailableCatalogItems", J_ignore),
    M("com/ea/nimble/ILog", "writeWithTitle", J_void),
    M("com/ea/nimble/ILog", "write", J_void),
    M("com/ea/nimble/ILog", "getThresholdLevel", J_ignore),
    M("com/ea/nimble/Nimble", "getApplicationEnvironment", J_ignore),
    M("com/ea/nimble/Nimble", "getSynergyEnvironment", J_ignore),
    M("com/ea/nimble/ApplicationEnvironment", "getApplicationLanguageCode", J_empty),
    M("com/ea/nimble/ApplicationEnvironment", "getCarrier", J_empty),
    M("com/ea/nimble/ApplicationEnvironment", "getEAHardwareId", J_empty),
    M("com/ea/nimble/ApplicationEnvironment", "getMACAddress", J_empty),
    M("com/ea/nimble/ApplicationEnvironment", "isDeviceRooted", J_false),
    M("com/ea/nimble/SynergyEnvironment", "getSynergyId", J_empty),
    M("com/ea/nimble/SynergyEnvironment", "getEaDeviceId", J_empty),
    M("com/ea/nimble/Log", "getThresholdLevel", J_ignore),
    M("com/ea/nimble/Identity", "getAuthenticator", J_ignore),
    { NULL, NULL, NULL, NULL },
};

static bool safe_read(uint64_t addr, void *out, size_t n)
{
    vm_size_t got = 0;
    if (!addr) return false;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)addr, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n;
}
static const char *host_str(uint64_t p)
{
    static char buf[128];
    if (!p || !safe_read(p, buf, sizeof(buf) - 1)) return NULL;
    buf[sizeof(buf) - 1] = 0;
    return buf[0] ? buf : NULL;
}
static void dump_obj(const char *tag, uint64_t p, uint8_t *rx, uint8_t *rw)
{
    if (!p) return;
    uint64_t vt = 0;
    if (!safe_read(p, &vt, sizeof(vt))) return;
    tl_log_line("monitor: %s obj=%#llx vtable=%#llx (rx off %#llx)", tag,
                (unsigned long long)p, (unsigned long long)vt,
                (unsigned long long)(vt - (uint64_t)rx));
    uint64_t ti = 0;
    if (vt > (uint64_t)rx && vt < (uint64_t)rx + 0x2615000)
        safe_read(vt - 8, &ti, sizeof(ti));
    tl_log_line("monitor: %s typeinfo=%#llx", tag, (unsigned long long)ti);
    if (ti) {
        uint64_t name = 0;
        safe_read(ti + 8, &name, sizeof(name));
        const char *s = host_str(name);
        tl_log_line("monitor: %s type=%s", tag, s ? s : "?");
    }
}

static bool printable(const char *s)
{
    if (!s || !*s) return false;
    for (int i = 0; s[i]; i++) if (s[i] < 32 || s[i] > 126) return false;
    return true;
}
static void scan_strings(const char *tag, uint64_t base, size_t span)
{
    int shown = 0;
    for (size_t off = 0; off + 8 <= span && shown < 40; off += 8) {
        uint64_t v = 0;
        if (!safe_read(base + off, &v, 8)) continue;
        const char *s = host_str(v);
        if (s && printable(s)) {
            tl_log_line("monitor: %s+%#zx str \"%s\"", tag, off, s);
            shown++;
        }
    }
}

static void scan_objects(const char *tag, uint64_t base, size_t span, uint8_t *rx, uint8_t *rw)
{
    int shown = 0;
    for (size_t off = 0; off + 8 <= span && shown < 80; off += 8) {
        uint64_t v = 0;
        if (!safe_read(base + off, &v, sizeof(v))) continue;
        if (v < 0x100000000ull || v > 0x800000000000ull) continue;
        uint64_t vt = 0;
        if (!safe_read(v, &vt, sizeof(vt))) continue;
        uint64_t voff = 0;
        if (vt >= (uint64_t)rx && vt < (uint64_t)rx + 0x2615000) voff = vt - (uint64_t)rx;
        else if (vt >= (uint64_t)rw && vt < (uint64_t)rw + 0x2615000) voff = vt - (uint64_t)rw;
        else continue;
        uint64_t ti = 0;
        if (!safe_read(vt - 8, &ti, sizeof(ti)) || !ti) continue;
        uint64_t name = 0;
        if (!safe_read(ti + 8, &name, sizeof(name))) continue;
        const char *s = host_str(name);
        if (!s) continue;
        tl_log_line("monitor: %s+%#zx -> %s (vtoff %#llx)", tag, off, s, (unsigned long long)voff);
        shown++;
    }
}

static void *monitor_thread(void *arg)
{
    tl_lib *L = arg;
    uint8_t *rw = tl_ld_lib_writable(L);
    uint8_t *rx = tl_ld_lib_base(L);
    if (!rw) return NULL;
    uint8_t prev[0x200]; memset(prev, 0, sizeof(prev));
    static int app_dumped;
    for (;;) {
        sleep(1);
        static int once;
        if (!once) {
            once = 1;
            uint32_t ins = *(volatile uint32_t *)(rx + 0x166e59c);
            int64_t imm = (((int64_t)(ins >> 5) & 0x7ffff) << 2) | ((ins >> 29) & 3);
            imm = (imm << 43) >> 43;                 /* sign-extend the 21-bit adrp immediate */
            uint64_t page = ((uint64_t)(rx + 0x166e59c) & ~0xfffull) + (uint64_t)(imm << 12);
            tl_log_line("monitor: rx=%p rw=%p adrp-global=%#llx (ins %#x)",
                        (void *)rx, (void *)rw, (unsigned long long)(page + 0xe50), ins);
            for (int i = -4; i <= 8; i++) {
                uint64_t v = *(volatile uint64_t *)(rw + 0x25a4e50 + (int64_t)i * 8);
                tl_log_line("monitor: [%+#x] = %#llx", (unsigned)(0x25a4e50 + i * 8), (unsigned long long)v);
            }
        }
        uint64_t app = *(volatile uint64_t *)(rw + 0x25a4e50);
        if (app && !app_dumped) { app_dumped = 1; dump_obj("app", app, rx, rw); }
        uint64_t p88 = app ? *(volatile uint64_t *)(app + 0x88) : 0;
        static int lawn_scanned, loop_count;
        loop_count++;
        if (p88 && !lawn_scanned) {
            lawn_scanned = 1;
            dump_obj("lawn", p88, rx, rw);
        }
        if (p88 && (loop_count == 2 || loop_count == 6 || loop_count == 8)) {
            char tag[24]; snprintf(tag, sizeof(tag), "lawn@%ds", loop_count);
            scan_objects(tag, p88, 0x2000, rx, rw);
        }
        uint64_t p98 = app ? *(volatile uint64_t *)(app + 0x98) : 0;
        uint64_t c48 = app ? *(volatile uint64_t *)(app + 0xc48) : 0;
        uint64_t c50 = app ? *(volatile uint64_t *)(app + 0xc50) : 0;
        uint64_t cb0 = app ? *(volatile uint64_t *)(app + 0xcb0) : 0;
        uint64_t overlay = p88 ? *(volatile uint64_t *)(p88 + 0xaf8) : 0;
        uint64_t mainmenu = p88 ? *(volatile uint64_t *)(p88 + 0xb00) : 0;
        if (mainmenu) {
            uint64_t gate_flags = 0, blocker = 0;
            uint32_t blocker_count = 0;
            safe_read(mainmenu + 0x118, &gate_flags, sizeof(gate_flags));
            safe_read((uint64_t)rw + 0x256d230, &blocker, sizeof(blocker));
            safe_read(blocker ? blocker + 8 : 0, &blocker_count, sizeof(blocker_count));
            tl_log_line("monitor: menu gate flags=%#llx blocker-count=%u",
                        (unsigned long long)gate_flags, blocker_count);
        }
        static int menu_scanned;
        if (mainmenu && !menu_scanned) {
            menu_scanned = 1;
            dump_obj("menu", mainmenu, rx, rw);
        }
        static int button_dumped;
        static int buttons2_dumped;
        if (mainmenu && !buttons2_dumped && loop_count >= 10) {
            buttons2_dumped = 1;
            uint64_t b1 = 0, b2 = 0, img = 0;
            safe_read(mainmenu + 0xe0, &b1, 8);
            safe_read(mainmenu + 0xe8, &img, 8);
            safe_read(mainmenu + 0xf0, &b2, 8);
            uint64_t bs[2] = { b1, b2 };
            for (int i = 0; i < 2; i++) if (bs[i]) {
                char tag[16]; snprintf(tag, sizeof(tag), "menuBtn%d", i);
                dump_obj(tag, bs[i], rx, rw);
                scan_strings(tag, bs[i], 0x200);
                for (int off = 0x18; off < 0x90; off += 8) {
                    uint64_t v = 0;
                    if (safe_read(bs[i] + off, &v, 8)) tl_log_line("monitor:   %s+%#x = %#llx", tag, off, (unsigned long long)v);
                }
            }
            if (img) {
                dump_obj("menuImg", img, rx, rw);
                scan_strings("menuImg", img, 0x200);
                for (int off = 0x18; off < 0x90; off += 8) {
                    uint64_t v = 0;
                    if (safe_read(img + off, &v, 8)) tl_log_line("monitor:   menuImg+%#x = %#llx", off, (unsigned long long)v);
                }
            }
            uint64_t pane10 = 0;
            if (safe_read(mainmenu + 0xd0, &pane10, 8) && pane10) {
                dump_obj("pane10", pane10, rx, rw);
                for (int off = 0x18; off < 0xa0; off += 8) {
                    uint64_t v = 0;
                    if (safe_read(pane10 + off, &v, 8)) tl_log_line("monitor:   pane10+%#x = %#llx", off, (unsigned long long)v);
                }
            }
        }
        if (mainmenu && !button_dumped && loop_count >= 6) {
            uint64_t btn = 0, edit = 0;
            safe_read(mainmenu + 0x1608, &btn, 8);
            safe_read(mainmenu + 0x1610, &edit, 8);
            if (btn) {
                button_dumped = 1;
                dump_obj("button", btn, rx, rw);
                scan_strings("button", btn, 0x200);
                for (int off = 0; off < 0x90; off += 8) {
                    uint64_t v = 0;
                    if (safe_read(btn + off, &v, 8)) tl_log_line("monitor:   button+%#x = %#llx", off, (unsigned long long)v);
                }
            }
            if (edit) {
                dump_obj("edit", edit, rx, rw);
                scan_strings("edit", edit, 0x200);
                for (int off = 0; off < 0x90; off += 8) {
                    uint64_t v = 0;
                    if (safe_read(edit + off, &v, 8)) tl_log_line("monitor:   edit+%#x = %#llx", off, (unsigned long long)v);
                }
            }
            uint64_t ad1 = 0, ad2 = 0;
            safe_read(mainmenu + 0x108, &ad1, 8);
            safe_read(mainmenu + 0x220, &ad2, 8);
            uint64_t ads[2] = { ad1, ad2 };
            for (int a = 0; a < 2; a++) if (ads[a]) {
                dump_obj(a ? "adscroller" : "adcarousel", ads[a], rx, rw);
                for (int off = 0; off < 0x90; off += 8) {
                    uint64_t v = 0;
                    if (safe_read(ads[a] + off, &v, 8)) tl_log_line("monitor:   ad%d+%#x = %#llx", a, off, (unsigned long long)v);
                }
            }
        }
        if (mainmenu && (loop_count == 6 || loop_count == 8 || loop_count == 12)) {
            char tag[24]; snprintf(tag, sizeof(tag), "menu@%ds", loop_count);
            scan_objects(tag, mainmenu, 0x2000, rx, rw);
            uint64_t pane = 0;
            if (safe_read(mainmenu + 0xd0, &pane, 8) && pane) {
                char ptag[24]; snprintf(ptag, sizeof(ptag), "pane@%ds", loop_count);
                scan_objects(ptag, pane, 0x2000, rx, rw);
            }
        }
        static uint64_t overlay_last;
        static uint8_t overlay_prev[0x100];
        if (overlay && overlay != overlay_last) {
            overlay_last = overlay;
            memset(overlay_prev, 0, sizeof(overlay_prev));
            dump_obj("overlay", overlay, rx, rw);
        }
        if (overlay) {
            uint64_t clock_obj = 0;
            float clock_s = 0, transition[3] = {0};
            uint16_t flags = 0;
            safe_read((uint64_t)rw + 0x256a010, &clock_obj, sizeof(clock_obj));
            safe_read(clock_obj ? clock_obj + 0x18 : 0, &clock_s, sizeof(clock_s));
            safe_read(overlay + 0xd4, transition, sizeof(transition));
            safe_read(overlay + 0xf8, &flags, sizeof(flags));
            tl_log_line("monitor: transition clock=%g duration=%g start=%g held=%g flags=%#x",
                        clock_s, transition[0], transition[1], transition[2], flags);
            for (int off = 0; off < 0x100; off += 8) {
                uint64_t v = 0, o = 0;
                if (!safe_read(overlay + off, &v, 8)) continue;
                memcpy(&o, overlay_prev + off, 8);
                if (v != o) tl_log_line("monitor:   overlay+%#x %#llx -> %#llx", off, (unsigned long long)o, (unsigned long long)v);
            }
            for (int off = 0; off < 0x100; off += 8) {
                uint64_t v = 0;
                if (safe_read(overlay + off, &v, 8)) memcpy(overlay_prev + off, &v, 8);
            }
        }
        int f1cc = app ? *(volatile uint8_t *)(app + 0x1cc) : -1;
        tl_log_line("monitor: app=%#llx f1cc=%d p88=%#llx p98=%#llx c48=%#llx c50=%#llx cb0=%#llx",
                    (unsigned long long)app, f1cc, (unsigned long long)p88, (unsigned long long)p98,
                    (unsigned long long)c48, (unsigned long long)c50, (unsigned long long)cb0);
        if (app) {
            uint8_t *p = (uint8_t *)app;
            for (int off = 0; off < 0x200; off += 8) {
                uint64_t v; memcpy(&v, p + off, 8);
                uint64_t o; memcpy(&o, prev + off, 8);
                if (v != o) {
                    tl_log_line("monitor:   +%#x %#llx -> %#llx", off, (unsigned long long)o, (unsigned long long)v);
                    if ((off == 0x120 || off == 0x128) && v) dump_obj(off == 0x120 ? "app+120" : "app+128", v, rx, rw);
                }
            }
            memcpy(prev, p, sizeof(prev));
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ loading */

static const char *probe_thread(void) { static char n[32]; pthread_getname_np(pthread_self(), n, sizeof(n)); return n; }
static uint64_t rd64(uint64_t p, uint64_t off) { return p ? *(volatile uint64_t *)(p + off) : 0; }
static uint32_t rd32(uint64_t p, uint64_t off) { return p ? *(volatile uint32_t *)(p + off) : 0; }

static void probe_store(uint64_t *r) { tl_log_line("probe[%s]: store surface object %#llx into driver %#llx", probe_thread(), (unsigned long long)r[20], (unsigned long long)r[19]); }
static void probe_load(uint64_t *r) { tl_log_line("probe[%s]: surfaceChanged reads x21=%#llx", probe_thread(), (unsigned long long)r[21]); }

static void probe_setter(uint64_t *r)
{
    uint64_t d = r[0];
    tl_log_line("probe[%s]: setSurfaceSize(driver=%#llx, w1=%lld, w2=%lld, w3=%lld, w4=%lld) "
                "cur w=%u h=%u c70=%u c74=%u cb0=%#llx cb8=%#llx",
                probe_thread(), (unsigned long long)d, (long long)r[1], (long long)r[2],
                (long long)r[3], (long long)r[4], rd32(d, 0xc68), rd32(d, 0xc6c),
                rd32(d, 0xc70), rd32(d, 0xc74), (unsigned long long)rd64(d, 0xcb0),
                (unsigned long long)rd64(d, 0xcb8));
}
static void probe_setter_past(uint64_t *r)
{
    uint64_t d = r[19];
    tl_log_line("probe[%s]: setter passed the early-out (driver=%#llx cb0=%#llx c58=%#llx)",
                probe_thread(), (unsigned long long)d, (unsigned long long)rd64(d, 0xcb0),
                (unsigned long long)rd64(d, 0xc58));
}
static void probe_update(uint64_t *r)
{
    uint64_t d = r[0];
    tl_log_line("probe[%s]: surface update(driver=%#llx cb0=%#llx cb8=%#llx c48=%#llx c50=%#llx)",
                probe_thread(), (unsigned long long)d, (unsigned long long)rd64(d, 0xcb0),
                (unsigned long long)rd64(d, 0xcb8), (unsigned long long)rd64(d, 0xc48),
                (unsigned long long)rd64(d, 0xc50));
}
static void probe_wrapper(uint64_t *r)
{
    uint64_t w = r[0];
    tl_log_line("probe[%s]: surface wrapper(obj=%#llx a1=%lld a2=%lld) app=%#llx oldw=%u force=%u",
                probe_thread(), (unsigned long long)w, (long long)r[1], (long long)r[2],
                (unsigned long long)rd64(w, 0x98), rd32(w, 0x118),
                (unsigned)rd32(w, 0x1b0) & 0xff);
}
static void probe_changed(uint64_t *r)
{
    tl_log_line("probe[%s]: Native_onSurfaceChanged native(w=%lld h=%lld)", probe_thread(),
                (long long)r[2], (long long)r[3]);
}
static void probe_changed_lambda(uint64_t *r)
{
    uint64_t l = r[0], packed = rd64(l, 8);
    tl_log_line("probe[%s]: surfaceChanged lambda(w=%u h=%u)", probe_thread(),
                (unsigned)(packed & 0xffffffffu), (unsigned)(packed >> 32));
}
static tl_lib *load(const char *name)
{
    tl_lib *L = tl_ld_load(name);
    if (!L) tl_log_line("pvz2: could not load %s", name);
    return L;
}

static int onload(tl_lib *L)
{
    typedef int (*fn_t)(void *, void *);
    fn_t fn = (fn_t)tl_ld_sym(L, "JNI_OnLoad");
    if (!fn) return -1;
    int v = fn(tl_jni_vm(), NULL);
    tl_log_line("pvz2: %s JNI_OnLoad -> %d", tl_ld_lib_name(L), v);
    return v;
}

static void mkdirs(const char *path)
{
    char t[1024]; snprintf(t, sizeof(t), "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0755); *p = '/'; }
    mkdir(t, 0755);
}

/* -------------------------------------------------------------------- start */

/* The test's own thread: it is not the game's, so it has no JNI environment --
 * exactly the case in which the guest's own crash handler must not run. */
static void *raise_on_a_host_thread(void *arg)
{
    int sig = (int)(intptr_t)arg;
    usleep(200000);
    tl_log_line("pvz2: the test's host thread raises signal %d", sig);
    raise(sig);
    return NULL;
}

static void *gl_main(void *arg);

bool tl_pvz2_start(const tl_ga_config *cfg)
{
    P.cfg = *cfg;
    snprintf(P.apk, sizeof(P.apk), "%s", cfg->apk_path);
    snprintf(P.data, sizeof(P.data), "%s", cfg->data_dir);
    snprintf(P.pkg, sizeof(P.pkg), "%s", cfg->package_name);
    P.cfg.apk_path = P.apk; P.cfg.data_dir = P.data; P.cfg.package_name = P.pkg;
    P.w = cfg->width; P.h = cfg->height;
    atomic_store(&P.start_wh, ((uint64_t)(uint32_t)P.w << 32) | (uint32_t)P.h);
    P.scale = 1.0f;
    /* The frame rate: the host's own default, unless the host has already
     * said what the display can do (the iPad's 120). The game's setting then
     * chooses between them. */
    {
        const char *fps_env = getenv("TL_PVZ2_FPS");
        const int fps = fps_env && atoi(fps_env) > 0 ? atoi(fps_env) : 60;
        if (!atomic_load(&P.target_fps)) atomic_store(&P.target_fps, fps);
        if (!atomic_load(&P.max_fps)) atomic_store(&P.max_fps, fps);
    }
    /* The guest asks the device for the orientation while it starts, so the
     * answer has to be there before it does: 4 is landscape. The host may
     * have set it already (the iPad can start in either landscape). */
    if (!P.orientation) P.orientation = 4;
    if (cfg->frame_dir) { snprintf(P.frame_dir, sizeof(P.frame_dir), "%s", cfg->frame_dir); P.cfg.frame_dir = P.frame_dir; }

    snprintf(P.files_guest, sizeof(P.files_guest), "/sdcard/Android/data/" PKG "/files");
    snprintf(P.obb_guest, sizeof(P.obb_guest), "/sdcard/Android/obb/" PKG "/" OBB_NAME);
    snprintf(P.obb_host, sizeof(P.obb_host), "%s/sdcard/Android/obb/%s/%s", P.data, PKG, OBB_NAME);
    { char d[900];
      snprintf(d, sizeof(d), "%s/sdcard/Android/data/%s/files", P.data, PKG); mkdirs(d);
      snprintf(d, sizeof(d), "%s/sdcard/Android/data/%s/cache", P.data, PKG); mkdirs(d);
      snprintf(d, sizeof(d), "%s/sdcard/Android/obb/%s", P.data, PKG); mkdirs(d); }

    tl_set_data_dir(P.data);
    cfg_load();
    tl_nwindow_configure(P.w, P.h, cfg->metal_layer);
    int n = tl_dexidx_open(P.apk);
    tl_log_line("pvz2: %d classes in the APK's DEX", n);
    if (!tl_ld_add_apk(P.apk)) return false;
    if (cfg->angle_egl && !tl_egl_init(cfg->angle_egl, cfg->angle_gles, cfg->frame_dir, cfg->frame_every))
        tl_log_line("pvz2: ANGLE did not load (%s); continuing without GL", cfg->angle_egl);

    tl_jni_init();
    tl_hle_configure(P.pkg, P.apk, P.data, P.w, P.h);
    tl_jni_hle_install();

    /* The Java world the native library expects to find.  The class hierarchy
     * matters: the engine asks the Activity for Context/Activity methods, and
     * the view for View/SurfaceView ones. */
    tl_jni_declare(ACT, "android/app/Activity");
    tl_jni_declare(ACT2, ACT);
    tl_jni_declare(APP, "java/lang/Object");
    tl_jni_declare(VIEW, "android/view/SurfaceView");
    tl_jni_declare("android/view/SurfaceHolder", "java/lang/Object");
    tl_jni_declare(HTTP, "java/lang/Object");
    tl_jni_declare(TXN, "java/lang/Object");
    tl_jni_declare(CLOUD, "java/lang/Object");
    tl_jni_declare(PLAYC, "java/lang/Object");
    tl_jni_declare(PLAYA, "java/lang/Object");
    tl_jni_declare(PLAYL, "java/lang/Object");
    tl_jni_declare(NOTIF, "java/lang/Object");
    tl_jni_declare(UIEV, "java/lang/Object");
    tl_jni_declare("com/popcap/SexyAppFramework/AndroidFacebookDriver", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/Utility", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/Nimble", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/NimbleComponent", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/mtx/NimbleMTX", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/mtx/INimbleMTX", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/Log", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/ILog", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/Network", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/INetwork", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/Network$Status", "java/lang/Enum");
    tl_jni_declare("java/lang/Enum", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/IMessage", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/tracking/Tracking", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/tracking/ITracking", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/ApplicationEnvironment", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/SynergyEnvironment", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/Log", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/Identity", "java/lang/Object");
    tl_jni_declare("com/ea/nimble/bridge/BaseNativeCallback", "java/lang/Object");
    tl_jni_register_hle(k_hle);

    P.activity = tl_jni_new_object(tl_jni_class(ACT2));
    tl_hle_set_activity(P.activity);
    P.view = tl_jni_new_object(tl_jni_class(VIEW));
    P.surface = tl_jni_new_object(tl_jni_class("android/view/Surface"));
    P.holder = tl_jni_new_object(tl_jni_class("android/view/SurfaceHolder"));
    P.http = tl_jni_new_object(tl_jni_class(HTTP));
    P.cloud = tl_jni_new_object(tl_jni_class(CLOUD));
    P.playc = tl_jni_new_object(tl_jni_class(PLAYC));
    P.playa = tl_jni_new_object(tl_jni_class(PLAYA));
    P.playl = tl_jni_new_object(tl_jni_class(PLAYL));
    P.notif = tl_jni_new_object(tl_jni_class(NOTIF));
    P.uiev = tl_jni_new_object(tl_jni_class(UIEV));
    P.gameapp = tl_jni_new_object(tl_jni_class(APP));

    /* Code patching has to be on before libRFLib's initialisers: they hook
     * engine functions with A64HookFunction, which stores straight into code.
     * A64HookFunction also builds its trampolines in libRFLib's own .bss and
     * branches to them, so the writable view has to execute too (best effort;
     * a W^X phone refuses and the in-place patch path still works).
     *
     * With converted libraries there is nothing to patch: their code is signed
     * Mach-O in a read-only segment, and the hooks have to be baked in ahead of
     * time (see scripts/aot-convert.py). Turning the emulation on here would only
     * have the guest ask for a writable view of a page it cannot have. */
    if (!tl_aot_ready()) tl_codewrite_enable();

    tl_lib *cxx = load("libc++_shared.so");
    tl_lib *nimble = load("libNimble.so");
    tl_lib *game = load("libPVZ2.so");
    if (!cxx || !nimble || !game) return false;
    GAME_LIB = game;
    if (getenv("TL_PVZ2_MONITOR")) {
        pthread_t mt; pthread_create(&mt, NULL, monitor_thread, game);
    }
    if (tl_xmem_writable_view_executable()) tl_log_line("pvz2: the writable view is executable (static trampolines)");
    else tl_log_line("pvz2: the writable view stayed W^X; static trampolines may fault");

    /* The game's own static initialisers, then JNI_OnLoad (the Switch port's
     * order: so_execute_init_array(game) before game_onload). */
    if (!tl_ld_init(cxx) || !tl_ld_init(nimble) || !tl_ld_init(game)) {
        tl_log_line("pvz2: constructors failed");
        return false;
    }
    onload(nimble);
    if (onload(game) < 0) tl_log_line("pvz2: libPVZ2 has no JNI_OnLoad");

    /* Reflourished's own library: loaded and initialised after the game's
     * JNI_OnLoad, so that the functions it hooks are registered. */
    /* Reflourished's own library is the mod, and it installs its hooks by storing branches
     * into the game. A converted library's code is signed and read-only, so A64HookFunction
     * is redirected to the shim: it records what was asked for (site and replacement) and
     * returns, and the game runs on the original code until the hooks are baked in. The
     * library itself is still loaded -- the game expects it, and its constructors run. */
    if (getenv("TL_AOT_NO_MOD"))
        tl_log_line("pvz2: libRFLib.so not loaded (TL_AOT_NO_MOD)");
    else {
        tl_lib *rfl = load("libRFLib.so");
        if (rfl) tl_ld_init(rfl);
    }
    if (getenv("TL_PVZ2_PROBE")) {
        tl_ld_probe(game, 0x166bf4c, probe_changed);
        tl_ld_probe(game, 0x166e59c, probe_changed_lambda);
        tl_ld_probe(game, 0x166ffc0, probe_wrapper);
        tl_ld_probe(game, 0x16752b4, probe_setter);
        tl_ld_probe(game, 0x16754dc, probe_setter_past);
        tl_ld_probe(game, 0x1675550, probe_store);
        tl_ld_probe(game, 0x167571c, probe_update);
        tl_ld_probe(game, 0x13f198c, probe_store);
        tl_ld_probe(game, 0x13f1bac, probe_store);
        tl_ld_probe(game, 0x1758f5c, probe_load);
    }
    tl_log_line("pvz2: libraries loaded, %zu unresolved imports", tl_ld_unresolved_count());

    /* ApplicationLifecycle.onActivityCreate (the Java onCreate's first act)
     * reaches Nimble's native lifecycle observer.  There is no Java to call
     * it, so the driver does: without this, the app object the engine builds
     * in will-finish is never created and the first surface call dereferences
     * a null singleton. */
    typedef void (*map_fn)(void *, void *, void *);
    map_fn nlaunch = (map_fn)tl_ld_sym(game, "Java_com_ea_nimble_bridge_NimbleCppApplicationLifeCycle_onApplicationLaunch");
    if (nlaunch) { tl_log_line("pvz2: Nimble onApplicationLaunch"); nlaunch(tl_jni_env(), NULL, NULL); }

    /* Native_GameAppInitialize(view, http, cloud, connect, achievements,
     * leaderboard, notification) -> boolean. */
    typedef unsigned char (*init_fn)(void *, void *, void *, void *, void *, void *, void *, void *, void *);
    init_fn init = (init_fn)tl_jni_native(APP, "Native_GameAppInitialize",
        "(L" VIEW ";L" HTTP ";L" CLOUD ";L" PLAYC ";L" PLAYA ";L" PLAYL ";L" NOTIF ";)Z");
    if (!init) {
        tl_log_line("pvz2: Native_GameAppInitialize is not registered");
        return false;
    }
    unsigned char ok = init(tl_jni_env(), P.gameapp, P.view, P.http, P.cloud, P.playc, P.playa, P.playl, P.notif);
    tl_log_line("pvz2: Native_GameAppInitialize -> %d", (int)ok);

    typedef void (*v_fn)(void *, void *);
    typedef void (*s_fn)(void *, void *, void *);
    typedef void (*b_fn)(void *, void *, unsigned char);

    /* The GL thread exists from the moment the view is added to the window, as
     * GLSurfaceView's does; the engine's will-finish waits for it, so it has to
     * be running before the lifecycle calls below.
     *
     * The stack is TL_GL_STACK_MB megabytes, 8 by default. The game's own vector
     * reallocation runs its copy down out of this thread's stack, so the size of
     * the stack decides whether that copy fits: at 8 MB it faults at the bottom of
     * the stack, and at 256 MB it does not. */
    size_t gl_stack_mb = 8;
    { const char *e = getenv("TL_GL_STACK_MB"); if (e && atoi(e) > 0) gl_stack_mb = (size_t)atoi(e); }
    pthread_attr_t attr; pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)gl_stack_mb << 20);
    if (pthread_create(&P.gl, &attr, gl_main, NULL) != 0) tl_log_line("pvz2: could not start the GL thread");
    pthread_attr_destroy(&attr);

    /* SexyAppFrameworkActivity.onCreate: the lifecycle observer. */
    v_fn obs = (v_fn)tl_jni_native(ACT, "Native_createNativeApplicationLifecycleObserver", "()V");
    tl_log_line("pvz2: lifecycle observer");
    if (obs) obs(tl_jni_env(), P.activity);
    tl_log_line("pvz2: observer done");

    /* AndroidGameApp.onActivityCreate -> Native_applicationWillFinishLaunching(null). */
    s_fn willfinish = (s_fn)tl_jni_native(APP, "Native_applicationWillFinishLaunching", "(Ljava/lang/String;)V");
    tl_log_line("pvz2: will finish launching");
    if (willfinish) willfinish(tl_jni_env(), P.gameapp, str(""));
    tl_log_line("pvz2: will finish done");

    /* onActivityStart -> Native_applicationDidFinishLaunching. */
    v_fn didfinish = (v_fn)tl_jni_native(APP, "Native_applicationDidFinishLaunching", "()V");
    tl_log_line("pvz2: did finish launching");
    if (didfinish) didfinish(tl_jni_env(), P.gameapp);
    tl_log_line("pvz2: did finish done");

    /* onActivityResume -> Native_NotifyAppRunning(true), Native_applicationWillBecomeForeground. */
    b_fn running = (b_fn)tl_jni_native(APP, "Native_NotifyAppRunning", "(Z)V");
    if (running) running(tl_jni_env(), P.gameapp, 1);
    v_fn foreground = (v_fn)tl_jni_native(APP, "Native_applicationWillBecomeForeground", "()V");
    if (foreground) foreground(tl_jni_env(), P.gameapp);

    /* onSurfaceCreated -> Native_NotifySurfaceChange(true), then focus. */
    b_fn surf = (b_fn)tl_jni_native(APP, "Native_NotifySurfaceChange", "(Z)V");
    if (surf) surf(tl_jni_env(), P.gameapp, 1);
    v_fn didactive = (v_fn)tl_jni_native(APP, "Native_applicationDidBecomeActive", "()V");
    if (didactive) didactive(tl_jni_env(), P.gameapp);

    P.started = true;
    atomic_store(&P.ready, true);
    return true;
}

/* -------------------------------------------------------------- GL and loop */

typedef void *EGLDisplay, *EGLSurface, *EGLContext, *EGLConfig;
typedef int32_t EGLint;
#define EGL_NONE 0x3038

/* The lifecycle requests, performed on the GL thread between frames. They are
 * defined below, next to the code that answers the host. */
static int lc_take_request(int *seq_out);
static void lc_report_done(int seq);
/* The guest's app object (the LawnApp) and the way to reach it: the driver's
 * own global holds the driver, and the driver holds the app at +0x98. */
static void *guest_app_object(void);
static atomic_ullong g_app_object;
/* The orientation change, posted from the host's thread and done on the game's
 * own. */
static atomic_int g_orientation_pending;
static atomic_int g_orientation_wanted;
/* The guest's own UI queries (they are defined further down, with the rest of
 * the geometry code). */
static void *guest_find_widget(const char *name);
static bool guest_anchored_rect(void *widget, int *out);
static double guest_surface_scale(void);
static void *guest_code(uint64_t guest_vaddr);
/* The seed tray and the shovel, in the surface's own space, and where they came
 * from. The host asks for a shortcut's position; this is what it answers with. */
static int g_tray[4];              /* x, y, w, h -- the surface's space */
static int g_shovel[4];
static atomic_int g_geometry_state;   /* 0: none yet, 1: the guest's, 2: measured */
static pthread_mutex_t g_geometry_lock = PTHREAD_MUTEX_INITIALIZER;


static void lifecycle_background_now(void);
static void lifecycle_foreground_now(void);
/* The guest-call guard: the fault in a guest call is caught, because the
 * guest's own SIGSEGV handler is not installed any more (the definitions are
 * further down, with the rest of the guest-call helpers). */
static sigjmp_buf g_guest_jmp;
static volatile sig_atomic_t g_guest_fault;
static void guest_guard_enter(void);
static void guest_guard_leave(void);
/* Reading the guest's memory without the risk of a fault (on the Mac through
 * the kernel, on the iPad directly -- the guest's heap is this process's). */
static bool guest_read(uint64_t addr, void *out, size_t size);

/* The native shovel: the guest's own "the shovel button was pressed".
 *
 * The game arms the shovel by pressing its own button: `ShovelUI::OnMouseDown`
 * (guest 0x13ab0a0) takes the widget named "UIShovel" and an event whose type
 * (int at +0x30) is 0, and it checks nothing else -- no sun, no cooldown, no
 * plant in hand, no "a shovel is already armed". It creates the ShovelCursor
 * entity (that is what "armed" is) and fires the button's delegate.
 *
 * Two ways to press it:
 *   1. the touch queue, at the button's centre -- the game's own path, and
 *      the better one: it also runs the HUD half of the tap, which the direct
 *      call does not;
 *   2. the call itself, when the button cannot be found or measured.
 *
 * The call has to happen on the guest's own thread, inside the frame callback,
 * so the request comes here from the host's thread and is done in the draw
 * loop. The mode is off by default (the ordinary tap at the shovel's place is
 * what the game expects); 1 is the touch first, 2 is the call. */
#define GUEST_SHOVEL_ON_MOUSE_DOWN 0x13ab0a0

static atomic_int g_shovel_mode;      /* 0 off, 1 the touch first, 2 the call */
static atomic_int g_shovel_pending;   /* a request is waiting */
static int g_shovel_touch_down;       /* the down of the touch path */

/* The memory warning: the game's Java has no memory callback at all, so the
 * host is the activity. The guest has the function -- LawnApp::LowMemoryWarning,
 * guest 0x93fcb0, which unloads the resource group "_inprogress" -- and nobody
 * calls it. It is called on the game's own thread, like every other guest call.
 *
 * It is off by default: the host's own warning is enough for now, and the
 * guest's function is experimental (the RE lane's MEMORY-WARNING.md). */
#define GUEST_LOW_MEMORY_WARNING 0x93fcb0
static atomic_int g_low_memory_pending;
static atomic_int g_low_memory_on;

void tl_pvz2_set_low_memory_warning(int on)
{
    atomic_store(&g_low_memory_on, on);
    tl_log_line("pvz2: the guest's own memory warning is %s", on ? "on" : "off");
}

void tl_pvz2_request_low_memory(void)
{
    if (!atomic_load(&g_low_memory_on)) return;
    atomic_store(&g_low_memory_pending, 1);
}

static void guest_low_memory_warning(void)
{
    void *fn = guest_code(GUEST_LOW_MEMORY_WARNING);
    uint64_t app = atomic_load(&g_app_object);
    if (!app) app = (uint64_t)(uintptr_t)guest_app_object();
    if (!fn || !app) {
        tl_log_line("pvz2: the guest's own memory warning is not reachable");
        return;
    }
    void (*call)(void *) = (void (*)(void *))fn;
    call((void *)app);
    tl_log_line("pvz2: the guest was told about the memory pressure");
}

/* ---- the native seed pick ------------------------------------------------
 *
 * The game's own answer to "this seed card was pressed": the SeedPacket's own
 * pointer-down handler (its vtable +0x90, guest 0x12278a4). It checks the
 * sun, the cooldown, the last-stand lock and the board's own state, and on
 * success creates the in-hand cursor (guest 0x1227c88). So the host does not
 * have to guess where the card is, and it does not have to fake a touch --
 * which is what the old shortcut did, and it was wrong at every surface size
 * but the one it was measured on.
 *
 * The chain to the card is the game's own (the R3 lane's static/ABI.md):
 *
 *     board  = 0xce4d8c()
 *     bank   = 0xaa1be8(board)            the seed bank's UI object
 *     count  = *(int8_t *)(bank + 0x1f8)  how many cards are in it
 *     0x12d2374(bank, &index, &ref)      the card's managed reference
 *     registry = 0x14ead90()
 *     0x14f39cc(registry, &ref, &resolved)
 *     0x14d926c(&resolved)               non-zero means null
 *     type   = 0x14d9178(&resolved)
 *     pool   = 0x14f31a4(registry, type)
 *     packet = 0x14ede00(pool, &resolved)
 *     0x14d9154(&resolved); 0x14d9154(&ref)
 *
 * and then the pick itself: packet->vtable[0x90](packet, &event), where the
 * event is the game's own pointer-event struct (0x34 bytes, and the int at
 * +0x30 is 0 -- a pointer down).
 */
#define GUEST_BOARD_GETTER        0xce4d8c
#define GUEST_SEED_BANK           0xaa1be8
#define GUEST_SEED_BANK_ITEM      0x12d2374
#define GUEST_UI_REGISTRY         0x14ead90
#define GUEST_UI_REF_RESOLVE      0x14f39cc
#define GUEST_UI_REF_IS_NULL      0x14d926c
#define GUEST_UI_REF_TYPE         0x14d9178
#define GUEST_UI_POOL             0x14f31a4
#define GUEST_UI_POOL_ITEM        0x14ede00
#define GUEST_UI_REF_RELEASE      0x14d9154
/* The game's own packet-release decision (R3's static/CURSOR.md): either
 * retire a repeated pick or move the active cursor to pending. Calling
 * abc634 directly skips the same-card cancellation flag set by AddEntity. */
#define GUEST_CURSOR_RELEASE      0x761074
/* The shovel's cursor (ShovelCursor, made by ShovelUI::OnMouseDown at
 * 0x146046c): its input handler (0x1460734) runs the same release decision
 * on a release over its own button, and digs on a release anywhere else. */
#define GUEST_SHOVEL_CURSOR_VTABLE 0x24d8b60

static atomic_int g_seed_pick_pending;   /* a slot is waiting */
static atomic_int g_seed_pick_slot;
static atomic_int g_seed_pick_on;         /* the switch: 0 off, 1 on */
static atomic_int g_collect_sun_pending;

void tl_pvz2_request_collect_sun(void)
{
    atomic_store(&g_collect_sun_pending, 1);
}

/* Escape: in live play the game's own pause button, pressed the way a finger
 * presses it (this game ignores Android's Back in a level); anywhere else
 * Android's Back, which leaves a screen. */
static atomic_int g_escape_pending;
static int g_pause_touch_down, g_pause_touch_x, g_pause_touch_y;

void tl_pvz2_request_escape(void)
{
    atomic_store(&g_escape_pending, 1);
}

void tl_pvz2_set_seed_pick(int on)
{
    atomic_store(&g_seed_pick_on, on);
    tl_log_line("pvz2: the native seed pick is %s", on ? "on (the game's own press)" : "off");
}

void tl_pvz2_request_seed_pick(int slot)
{
    if (!atomic_load(&g_seed_pick_on)) {
        tl_log_line("pvz2: the seed pick is off (the old shortcut is not used either)");
        return;
    }
    atomic_store(&g_seed_pick_slot, slot);
    atomic_store(&g_seed_pick_pending, 1);
}

/* A guest call with two arguments, and the out pointer in x8 (the C++
 * convention for a constructor's return value). The fault is caught: the
 * guest's SIGSEGV handler is not installed, and a fault here would end the
 * process with nothing in the log. */
static uint64_t guest_call_x8(void *fn, uint64_t a0, uint64_t a1, void *out, bool *ok)
{
    register void *x16 __asm__("x16") = fn;
    register void *x8 __asm__("x8") = out;
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    *ok = false;
    if (!fn) return 0;
    guest_guard_enter();
    if (sigsetjmp(g_guest_jmp, 1) == 0) {
        __asm__ volatile("blr %1"
                         : "+r"(x0), "+r"(x16), "+r"(x1)
                         : "r"(x8)
                         : "x2","x3","x4","x5","x6","x7","x9","x10","x11","x12",
                           "x13","x14","x15","x17","memory","cc");
        guest_guard_leave();
        *ok = true;
        return x0;
    }
    guest_guard_leave();
    tl_log_line("pvz2: the guest's own call faulted (signal %d); there is no answer",
                (int)g_guest_fault);
    return 0;
}

/* The same, without an out pointer. */
static uint64_t guest_call2(void *fn, uint64_t a0, uint64_t a1, bool *ok)
{
    register void *x16 __asm__("x16") = fn;
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    *ok = false;
    if (!fn) return 0;
    guest_guard_enter();
    if (sigsetjmp(g_guest_jmp, 1) == 0) {
        __asm__ volatile("blr %1"
                         : "+r"(x0), "+r"(x16), "+r"(x1)
                         :
                         : "x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12",
                           "x13","x14","x15","x17","memory","cc");
        guest_guard_leave();
        *ok = true;
        return x0;
    }
    guest_guard_leave();
    tl_log_line("pvz2: the guest's own call faulted (signal %d); there is no answer",
                (int)g_guest_fault);
    return 0;
}

/* The game's own pointer-event struct: 0x34 bytes, and the int at +0x30 is
 * the type (0 is a pointer down -- the same object the shovel's call takes). */
typedef struct { uint8_t b[0x34]; } guest_pointer_event;

static bool guest_read_bytes(uint64_t addr, void *out, size_t size)
{
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    return guest_read(addr, out, size);
#else
    /* The guest's heap is this process's heap (the converted libraries run
     * here), so the memory is read directly. */
    if (!addr) return false;
    memcpy(out, (const void *)(uintptr_t)addr, size);
    return true;
#endif
}

/* Keyboard presses bypass the widget manager's pointer dispatch. Apply its
 * modal ownership and the board's pause/state checks before calling a HUD
 * handler. Reflourished 1.4.2: ab6858 names state 5 PlayingLevel; ab21dc and
 * ab6064 consult the pause registry at board+6c8. Its own AnyPaused predicate
 * is 7dec64. WidgetManager::SetBaseModal (1555908) stores the modal at +e0;
 * +c8 is keyboard focus, NOT the modal. All reads/calls run on the game thread.
 * Unknown state fails closed, including a board that is being torn down. */
#define GUEST_ANY_PAUSED 0x7dec64
static uint64_t guest_live_shortcut_board(const char *what)
{
    const char *reason = NULL;
    bool ok = false;
    uint64_t board = 0, pause_registry = 0, manager = 0, modal = 0;
    int32_t state = -1;
    if (atomic_load(&P.paused) || atomic_load(&P.in_background)) {
        reason = "the host is paused or in the background";
        goto ignored;
    }
    board = guest_call2(guest_code(GUEST_BOARD_GETTER), 0, 0, &ok);
    if (!ok || !board) {
        reason = "there is no board";
        goto ignored;
    }
    if (!guest_read_bytes(board + 0xe0, &state, sizeof state) ||
        !guest_read_bytes(board + 0x6c8, &pause_registry, sizeof pause_registry) ||
        !guest_read_bytes(board + 0x20, &manager, sizeof manager) ||
        !manager || !pause_registry ||
        !guest_read_bytes(manager + 0xe0, &modal, sizeof modal)) {
        reason = "the board's input state is unavailable";
        goto ignored;
    }
    if (state != 5) {
        reason = "the board is not PlayingLevel";
        goto ignored;
    }
    uint64_t paused = guest_call2(guest_code(GUEST_ANY_PAUSED), pause_registry, 0, &ok);
    if (!ok || paused) {
        reason = ok ? "the game is paused" : "the pause predicate is unavailable";
        goto ignored;
    }
    if (modal) {
        reason = "a modal widget covers the board";
        goto ignored;
    }
    tl_log_line("pvz2: %s: live board (state=%d, paused=0, modal=nil)", what, state);
    return board;
ignored:
    tl_log_line("pvz2: %s: ignored: %s (state=%d, modal=%p)",
                what, reason, state, (void *)modal);
    return 0;
}

/* Board::GetCollectingSun (aa35e4) uses the guest's snapshot iterator over
 * pool 0x48, then resolves each managed reference. Use that same iterator,
 * including its destructor: it keeps the pool stable across callbacks.
 * CollectableSun and real CollectableCoin both dispatch pointer-down through
 * 64f804 -> vtable+198 (64f998). The latter emits the native collect request;
 * Board::OnCollectableClicked (a9c5cc) chooses the counter and flight, and
 * a9d838 awards its value on arrival. Never write sun/currency or object state.
 * Exact vtables exclude rewards, fake coins and other interactive objects. */
#define GUEST_COLLECTABLE_ITERATOR 0x11beef0
#define GUEST_ITERATOR_HAS_NEXT    0x14ea3cc
#define GUEST_ITERATOR_REFERENCE   0x14ea364
#define GUEST_ITERATOR_NEXT        0x14ea3b0
#define GUEST_ITERATOR_DESTROY     0x14ea1fc
#define GUEST_COLLECT_REQUEST      0x64f998
#define GUEST_SUN_VALUE            0x75a96c
#define GUEST_SUN_VTABLE           0x2395208
#define GUEST_COIN_VTABLE          0x23bb9b8

static uint64_t guest_collectable_from_ref(uint64_t registry, uint64_t *ref, bool *ok)
{
    uint64_t resolved = 0, item = 0;
    bool release_ok = false;
    guest_call_x8(guest_code(GUEST_UI_REF_RESOLVE), registry,
                  (uint64_t)(uintptr_t)ref, &resolved, ok);
    if (!*ok) return 0;
    uint64_t is_null = guest_call2(guest_code(GUEST_UI_REF_IS_NULL),
                                  (uint64_t)(uintptr_t)&resolved, 0, ok);
    if (*ok && !is_null) {
        uint64_t type = guest_call2(guest_code(GUEST_UI_REF_TYPE),
                                    (uint64_t)(uintptr_t)&resolved, 0, ok);
        uint64_t pool = *ok ? guest_call2(guest_code(GUEST_UI_POOL), registry, type, ok) : 0;
        if (*ok && pool)
            item = guest_call2(guest_code(GUEST_UI_POOL_ITEM), pool,
                               (uint64_t)(uintptr_t)&resolved, ok);
    }
    guest_call2(guest_code(GUEST_UI_REF_RELEASE), (uint64_t)(uintptr_t)&resolved, 0, &release_ok);
    *ok = *ok && release_ok;
    return *ok ? item : 0;
}

static void guest_collect_sun(void)
{
    uint64_t board = guest_live_shortcut_board("the collect shortcut");
    if (!board) return;
    static const uint64_t needed[] = {
        GUEST_COLLECTABLE_ITERATOR, GUEST_ITERATOR_HAS_NEXT, GUEST_ITERATOR_REFERENCE,
        GUEST_ITERATOR_NEXT, GUEST_ITERATOR_DESTROY, GUEST_COLLECT_REQUEST, GUEST_SUN_VALUE,
        GUEST_UI_REGISTRY, GUEST_UI_REF_RESOLVE, GUEST_UI_REF_IS_NULL, GUEST_UI_REF_TYPE,
        GUEST_UI_POOL, GUEST_UI_POOL_ITEM, GUEST_UI_REF_RELEASE
    };
    for (size_t i = 0; i < sizeof needed / sizeof *needed; i++) {
        if (!guest_code(needed[i])) {
            tl_log_line("pvz2: the collect shortcut: ignored: a native collect handler is unavailable");
            return;
        }
    }
    uint64_t sun_vtable = (uint64_t)(uintptr_t)tl_aot_guest_data_ptr_any(GUEST_SUN_VTABLE);
    uint64_t coin_vtable = (uint64_t)(uintptr_t)tl_aot_guest_data_ptr_any(GUEST_COIN_VTABLE);
    if (!sun_vtable || !coin_vtable) return;
    bool ok = false;
    uint64_t registry = guest_call2(guest_code(GUEST_UI_REGISTRY), 0, 0, &ok);
    if (!ok || !registry) return;
    uint64_t pool = guest_call2(guest_code(GUEST_UI_POOL), registry, 0x48, &ok);
    if (!ok || !pool) return;
    uint64_t iterator[4] __attribute__((aligned(16))) = { 0 };
    guest_call_x8(guest_code(GUEST_COLLECTABLE_ITERATOR), 0, 0x48, iterator, &ok);
    if (!ok) return;
    int suns = 0, coins = 0, value = 0, seen = 0;
    int32_t sun_before = 0;
    guest_read_bytes(board + 0x184, &sun_before, sizeof sun_before);
    while (ok && seen < 4096) {
        uint64_t more = guest_call2(guest_code(GUEST_ITERATOR_HAS_NEXT),
                                    (uint64_t)(uintptr_t)iterator, 0, &ok);
        if (!ok || !more) break;
        seen++;
        uint64_t ref = 0;
        guest_call_x8(guest_code(GUEST_ITERATOR_REFERENCE),
                      (uint64_t)(uintptr_t)iterator, 0, &ref, &ok);
        if (!ok) break;
        uint64_t item = guest_collectable_from_ref(registry, &ref, &ok);
        bool release_ok = false;
        guest_call2(guest_code(GUEST_UI_REF_RELEASE), (uint64_t)(uintptr_t)&ref, 0, &release_ok);
        ok = ok && release_ok;
        if (!ok) break;
        if (item) {
            uint64_t vtable = 0;
            uint32_t flags = 0;
            int32_t state = -1;
            if (!guest_read_bytes(item, &vtable, sizeof vtable) ||
                !guest_read_bytes(item + 0x88, &flags, sizeof flags) ||
                !guest_read_bytes(item + 0x90, &state, sizeof state)) {
                ok = false;
                break;
            }
            /* Pointer dispatch refuses disabled objects (80); the board's
             * collect callback refuses 40. State 1 is already collecting;
             * flag 10 marks a retired object. Repeated Space cannot award twice. */
            if ((vtable == sun_vtable || vtable == coin_vtable) &&
                !(flags & 0xd0) && state != 1) {
                int amount = 0;
                if (vtable == sun_vtable) {
                    amount = (int)guest_call2(guest_code(GUEST_SUN_VALUE), item, 0, &ok);
                    if (!ok) break;
                }
                guest_call2(guest_code(GUEST_COLLECT_REQUEST), item, 0, &ok);
                if (!ok) break;
                /* The callback may retire the object; never dereference it again. */
                if (vtable == sun_vtable) { suns++; value += amount; }
                else coins++;
            }
        }
        guest_call2(guest_code(GUEST_ITERATOR_NEXT), (uint64_t)(uintptr_t)iterator, 0, &ok);
    }
    bool destroy_ok = false;
    guest_call2(guest_code(GUEST_ITERATOR_DESTROY), (uint64_t)(uintptr_t)iterator, 0, &destroy_ok);
    tl_log_line("pvz2: the collect shortcut: sun before=%d; requested %d sun(s), value=%d, %d coin(s), scanned=%d%s",
                sun_before, suns, value, coins, seen,
                ok && destroy_ok && seen < 4096 ? " (the native flight awards on arrival)" : " (stopped: native state unavailable)");
}

/* The card of a slot, the game's own way. */
static void *guest_seed_packet(int slot, bool *ok, uint64_t *board_out)
{
    *ok = false;
    if (board_out) *board_out = 0;
    /* Where does the chain break? The getter reads a GOT slot, then the app
     * pointer that slot holds, and then the board at app+0x9a8. The host can
     * read the same three things, so the log can say which one is empty. */
    {
        uint64_t got_slot = 0, app_ptr = 0, board_ptr = 0;
        void *slot = tl_aot_guest_data_ptr_any(0x2511918);
        if (slot && guest_read((uint64_t)(uintptr_t)slot, &got_slot, sizeof got_slot)) {
            app_ptr = got_slot;
            if (app_ptr) {
                uint64_t at_9a8 = 0;
                if (guest_read(app_ptr + 0x9a8, &at_9a8, sizeof at_9a8)) board_ptr = at_9a8;
            }
        }
        tl_log_line("pvz2: the seed pick: the GOT slot is %p, the app pointer %p, the board %p",
                    (void *)got_slot, (void *)app_ptr, (void *)board_ptr);
    }
    bool call_ok = false;
    void *board_fn = guest_code(GUEST_BOARD_GETTER);
    uint64_t board = guest_call2(board_fn, 0, 0, &call_ok);
    tl_log_line("pvz2: the seed pick: the board getter is at %p, the board is %p (%s)",
                board_fn, (void *)board, call_ok ? "the call went through" : "the call faulted");
    if (!call_ok || !board) {
        tl_log_line("pvz2: the seed pick: there is no board");
        return NULL;
    }
    void *bank_fn = guest_code(GUEST_SEED_BANK);
    if (board_out) *board_out = board;
    uint64_t bank = guest_call2(bank_fn, board, 0, &call_ok);
    tl_log_line("pvz2: the seed pick: the seed bank is at %p (the getter at %p)", (void *)bank, bank_fn);
    if (!call_ok || !bank) {
        tl_log_line("pvz2: the seed pick: there is no seed bank");
        return NULL;
    }
    /* How many cards are in the bank: a signed byte at +0x1f8. The index is
     * checked against it -- the low-level lookup has no bounds check of its
     * own, and an index past the end reads whatever is there. */
    int8_t count = 0;
    if (!guest_read_bytes(bank + 0x1f8, &count, sizeof count)) {
        tl_log_line("pvz2: the seed pick: the bank's count cannot be read");
        return NULL;
    }
    tl_log_line("pvz2: the seed pick: the bank has %d card(s)", (int)count);
    if (slot < 0 || slot >= (int)count) {
        tl_log_line("pvz2: the seed pick: there is no card in slot %d (the bank has %d)",
                    slot, (int)count);
        return NULL;
    }
    /* The card's managed reference, and the object behind it. */
    uint64_t ref = 0, resolved = 0;
    int index = slot;
    uint64_t r = guest_call_x8(guest_code(GUEST_SEED_BANK_ITEM), bank,
                               (uint64_t)(uintptr_t)&index, (void *)&ref, &call_ok);
    if (!call_ok) return NULL;
    (void)r;
    uint64_t registry = guest_call2(guest_code(GUEST_UI_REGISTRY), 0, 0, &call_ok);
    if (!call_ok || !registry) {
        tl_log_line("pvz2: the seed pick: there is no UI registry");
        return NULL;
    }
    uint64_t rr = guest_call_x8(guest_code(GUEST_UI_REF_RESOLVE), registry,
                                (uint64_t)(uintptr_t)&ref, (void *)&resolved, &call_ok);
    if (!call_ok) return NULL;
    (void)rr;
    uint64_t is_null = guest_call2(guest_code(GUEST_UI_REF_IS_NULL),
                                   (uint64_t)(uintptr_t)&resolved, 0, &call_ok);
    if (!call_ok) return NULL;
    if (is_null) {
        tl_log_line("pvz2: the seed pick: the card of slot %d is not there", slot);
        return NULL;
    }
    uint64_t type = guest_call2(guest_code(GUEST_UI_REF_TYPE),
                                (uint64_t)(uintptr_t)&resolved, 0, &call_ok);
    if (!call_ok) return NULL;
    tl_log_line("pvz2: the seed pick: the reference resolved (type %llu)", (unsigned long long)type);
    uint64_t pool = guest_call_x8(guest_code(GUEST_UI_POOL), registry, type, NULL, &call_ok);
    if (!call_ok || !pool) {
        tl_log_line("pvz2: the seed pick: there is no UI pool for the card");
        return NULL;
    }
    uint64_t packet = guest_call_x8(guest_code(GUEST_UI_POOL_ITEM), pool,
                                    (uint64_t)(uintptr_t)&resolved, NULL, &call_ok);
    /* The references are released: they are the guest's own memory. */
    guest_call2(guest_code(GUEST_UI_REF_RELEASE), (uint64_t)(uintptr_t)&resolved, 0, &call_ok);
    guest_call2(guest_code(GUEST_UI_REF_RELEASE), (uint64_t)(uintptr_t)&ref, 0, &call_ok);
    if (!call_ok || !packet) {
        tl_log_line("pvz2: the seed pick: the card of slot %d cannot be found", slot);
        return NULL;
    }
    *ok = true;
    return (void *)(uintptr_t)packet;
}

/* The pick itself, on the game's own thread. */
/* After the game's own press: the one cursor the press added to the board's
 * active vector goes through the game's own release decision, the way a real
 * tap's release does it.
 *
 * The press creates a cursor with an unbound token and leaves it in the
 * board's active vector. The game's next-down path assumes that an unbound
 * cursor is the *pending* one -- which is at board+0x408, and which was
 * null -- so the next lawn click dereferenced null and the game died:
 *
 *   abc75c: ldr  x8, [x20, #0x408]   ; the pending cursor (null)
 *   abc764: strb w9, [x8, #0x21]      ; null + 0x21 -- the fault
 *
 * Release (guest 0x761074) first honours the cancellation flag that
 * AddEntity sets when the same source (the same packet, the shovel) was
 * already pending. If the board's module/reference condition also permits
 * it, release calls abc634 to remove the cursor from the active vector and
 * install it as pending, with an unbound token and input priority 7. No
 * finger is held: the player's next real click on the lawn uses it.
 *
 * (The R3 lane's static/CURSOR.md.) Returns 1 when the cursor is pending, 0
 * when the game refused the press or cancelled it, -1 on anything else. */
static int guest_release_new_cursor(const char *what, uint64_t board,
                                    uint64_t begin_before, uint64_t end_before,
                                    uint64_t want_vtable_vaddr)
{
    uint64_t begin_after = 0, end_after = 0;
    guest_read_bytes(board + 0x3f0, &begin_after, sizeof begin_after);
    guest_read_bytes(board + 0x3f8, &end_after, sizeof end_after);
    /* The press may have given the vector new storage, so the ends are not
     * compared: the count is. A refusal (no sun, a cooldown) also answers
     * handled=1, and it adds nothing. */
    size_t n_before = (end_before - begin_before) / 8, n_after = (end_after - begin_after) / 8;
    if (!end_after || end_after < begin_after || n_after != n_before + 1) {
        tl_log_line("pvz2: %s: the press added no cursor (%zu -> %zu); the game refused it",
                    what, n_before, n_after);
        return 0;
    }
    uint64_t cursor = 0;
    if (!guest_read_bytes(end_after - 8, &cursor, sizeof cursor) || !cursor) {
        tl_log_line("pvz2: %s: the new cursor cannot be read", what);
        return -1;
    }
    /* It must be the cursor this press makes (PlantCursor 0x2491a60 for a
     * card, ShovelCursor 0x24d8b60 for the shovel) -- and the comparison is
     * against the *runtime* address, because in this process the guest's
     * data lives in the converted library. */
    uint64_t vtable = 0;
    void *want_vtable = tl_aot_guest_data_ptr_any(want_vtable_vaddr);
    if (!guest_read_bytes(cursor, &vtable, sizeof vtable) || !want_vtable ||
        vtable != (uint64_t)(uintptr_t)want_vtable) {
        tl_log_line("pvz2: %s: the new cursor is not the game's own cursor (vtable %p, want %p)",
                    what, (void *)vtable, want_vtable);
        return -1;
    }
    void *release = guest_code(GUEST_CURSOR_RELEASE);
    bool call_ok = false;
    guest_call2(release, cursor, 0, &call_ok);
    if (!call_ok) {
        tl_log_line("pvz2: %s: the cursor release faulted", what);
        return -1;
    }
    /* Do not call a cancellation a success. Release may retire the cursor
     * through callbacks: only compare its saved address afterwards, never
     * dereference it. */
    uint64_t pending = 0;
    if (!guest_read_bytes(board + 0x408, &pending, sizeof pending)) {
        tl_log_line("pvz2: %s: the released cursor state cannot be read", what);
        return -1;
    }
    if (!pending) {
        tl_log_line("pvz2: %s: the guest cancelled it (no pending cursor)", what);
        return 0;
    }
    if (pending != cursor) {
        tl_log_line("pvz2: %s: unexpected pending cursor %p (wanted %p)",
                    what, (void *)pending, (void *)cursor);
        return -1;
    }
    tl_log_line("pvz2: %s: the cursor %p is pending, waiting for the player's click",
                what, (void *)cursor);
    return 1;
}

static void guest_seed_pick(int slot)
{
    if (!guest_live_shortcut_board("the seed shortcut")) return;
    bool ok = false;
    uint64_t board = 0;
    void *packet = guest_seed_packet(slot, &ok, &board);
    if (!ok || !packet || !board) return;
    /* The card's own vtable: the pointer-down handler is at +0x90. */
    uint64_t packet_vtable = 0;
    if (!guest_read_bytes((uint64_t)(uintptr_t)packet, &packet_vtable, sizeof packet_vtable) || !packet_vtable) {
        tl_log_line("pvz2: the seed pick: the card has no vtable");
        return;
    }
    void *normal_packet_vtable = tl_aot_guest_data_ptr_any(0x24b2940);
    if (!normal_packet_vtable || packet_vtable != (uint64_t)(uintptr_t)normal_packet_vtable) {
        tl_log_line("pvz2: the seed pick: slot %d is not a normal seed packet", slot);
        return;
    }
    uint64_t fn = 0;
    if (!guest_read_bytes(packet_vtable + 0x90, &fn, sizeof fn) || !fn) {
        tl_log_line("pvz2: the seed pick: the card has no press handler");
        return;
    }
    guest_pointer_event event;
    memset(&event, 0, sizeof event);
    /* The int at +0x30 is the type: 0 is a pointer down. */
    int type = 0;
    memcpy(&event.b[0x30], &type, sizeof type);
    /* The board's active cursor vector, before the press: the new cursor is
     * the one the press adds. */
    uint64_t begin_before = 0, end_before = 0;
    int32_t sun = 0;
    if (!guest_read_bytes(board + 0x3f0, &begin_before, sizeof begin_before) ||
        !guest_read_bytes(board + 0x3f8, &end_before, sizeof end_before) ||
        end_before < begin_before || (end_before - begin_before) % 8 ||
        !guest_code(GUEST_CURSOR_RELEASE)) {
        tl_log_line("pvz2: the seed pick: the cursor state or release handler is unavailable");
        return;
    }
    guest_read_bytes(board + 0x184, &sun, sizeof sun);
    int (*call)(void *, void *) = (int (*)(void *, void *))(uintptr_t)fn;
    int handled = call(packet, &event);
    tl_log_line("pvz2: the seed pick: slot %d, the guest's own press, handled=%d (sun %d)", slot, handled, sun);
    if (!handled) {
        tl_log_line("pvz2: the seed pick: slot %d was refused (no sun, a cooldown, or the board is busy)", slot);
        return;
    }

    char what[48];
    snprintf(what, sizeof what, "the seed pick: slot %d", slot);
    guest_release_new_cursor(what, board, begin_before, end_before, 0x2491a60);
}

void tl_pvz2_request_native_shovel(void)
{
    if (!atomic_load(&g_shovel_mode)) return;   /* off: the ordinary tap is used */
    atomic_store(&g_shovel_pending, 1);
}

void tl_pvz2_set_native_shovel(int mode)
{
    atomic_store(&g_shovel_mode, mode);
    tl_log_line("pvz2: the native shovel is %s", mode == 2 ? "the guest's own call"
                                       : mode == 1 ? "the touch first, the call as the fallback"
                                                   : "off (the ordinary tap)");
}

/* The game's own pause menu (PauseDialog): while it is up it is the widget
 * manager's modal, and its rect -- in the app's own space -- is at +0x44. */
#define GUEST_PAUSE_DIALOG_VTABLE 0x24bcf00
static bool guest_pause_menu_rect(int *r)
{
    bool ok = false;
    uint64_t board = guest_call2(guest_code(GUEST_BOARD_GETTER), 0, 0, &ok), manager = 0, modal = 0, vtable = 0;
    void *want = tl_aot_guest_data_ptr_any(GUEST_PAUSE_DIALOG_VTABLE);
    if (!ok || !board || !want ||
        !guest_read_bytes(board + 0x20, &manager, sizeof manager) || !manager ||
        !guest_read_bytes(manager + 0xe0, &modal, sizeof modal) || !modal ||
        !guest_read_bytes(modal, &vtable, sizeof vtable) || vtable != (uint64_t)(uintptr_t)want ||
        !guest_read_bytes(modal + 0x44, r, 4 * sizeof(int)))
        return false;
    return r[2] > 0 && r[3] > 0;
}

/* Escape, on the guest's thread: a tap, the down now and the up a frame later.
 * In live play it is the game's pause button; with the game's pause menu up it
 * is that menu's RESUME (so Escape goes in and out of the pause); anywhere
 * else it is Back. */
static void guest_escape(void)
{
    int rect[4] __attribute__((aligned(16))) = { 0, 0, 0, 0 };
    double cx, cy;
    const char *what;
    void *widget = guest_live_shortcut_board("the pause shortcut") ? guest_find_widget("UIPauseButton") : NULL;
    if (widget && guest_anchored_rect(widget, rect) && rect[2] > 0 && rect[3] > 0) {
        cx = rect[0] + rect[2] / 2.0;
        cy = rect[1] + rect[3] / 2.0;
        what = "the game's pause button";
    } else if (!atomic_load(&P.in_background) && guest_pause_menu_rect(rect)) {
        /* RESUME's place in the menu, measured: 82% across, 90% down. */
        cx = rect[0] + rect[2] * 0.82;
        cy = rect[1] + rect[3] * 0.904;
        what = "the pause menu's RESUME";
    } else {
        tl_pvz2_key(4, 1);
        tl_pvz2_key(4, 0);
        tl_log_line("pvz2: Escape is Back here");
        return;
    }
    /* The rect is in the app's own space: the point is scaled as a whole. */
    double s = guest_surface_scale();
    g_pause_touch_x = (int)(cx * s);
    g_pause_touch_y = (int)(cy * s);
    tl_pvz2_touch(0, 1, (float)g_pause_touch_x, (float)g_pause_touch_y);
    g_pause_touch_down = 1;
    tl_log_line("pvz2: Escape presses %s at (%d,%d)", what, g_pause_touch_x, g_pause_touch_y);
}

/* The shovel, on the guest's thread. */
static void guest_native_shovel(void)
{
    if (!guest_live_shortcut_board("the shovel shortcut")) return;
    int mode = atomic_load(&g_shovel_mode);
    void *widget = guest_find_widget("UIShovel");
    int rect[4] __attribute__((aligned(16))) = { 0, 0, 0, 0 };
    bool have_rect = widget && guest_anchored_rect(widget, rect);
    /* The touch first (the game's own path): the button's centre, in the
     * surface's space. */
    if (mode == 1 && have_rect && rect[2] > 0 && rect[3] > 0) {
        double s = guest_surface_scale();
        int x = (int)(rect[0] * s) + rect[2] / 2;
        int y = (int)(rect[1] * s) + rect[3] / 2;
        if (!g_shovel_touch_down) {
            tl_pvz2_touch(0, 1, (float)x, (float)y);
            g_shovel_touch_down = 1;
            tl_log_line("pvz2: the shovel: the touch at (%d,%d) (the guest's own button)", x, y);
        } else {
            tl_pvz2_touch(3, 1, (float)x, (float)y);
            g_shovel_touch_down = 0;
            tl_log_line("pvz2: the shovel: the touch was let go at (%d,%d)", x, y);
        }
        return;
    }
    /* The guest's own call. The event is the game's own pointer-event struct:
     * 0x34 bytes, and the type (the int at +0x30) is 0. */
    void *fn = guest_code(GUEST_SHOVEL_ON_MOUSE_DOWN);
    if (widget && fn) {
        g_shovel_touch_down = 0;
        /* The press only makes the cursor; the release a real tap would do
         * next is the game's own decision too (guest_release_new_cursor):
         * without it the next lawn tap died, and a second press made a
         * second shovel instead of putting the first one back. */
        bool call_ok = false;
        uint64_t board = guest_call2(guest_code(GUEST_BOARD_GETTER), 0, 0, &call_ok);
        uint64_t begin_before = 0, end_before = 0;
        if (!call_ok || !board ||
            !guest_read_bytes(board + 0x3f0, &begin_before, sizeof begin_before) ||
            !guest_read_bytes(board + 0x3f8, &end_before, sizeof end_before) ||
            end_before < begin_before || (end_before - begin_before) % 8 ||
            !guest_code(GUEST_CURSOR_RELEASE)) {
            tl_log_line("pvz2: the shovel: there is no board, or its cursor state or release handler is unavailable");
            return;
        }
        uint8_t event[0x34];
        memset(event, 0, sizeof event);
        int (*call)(void *, void *) = (int (*)(void *, void *))fn;
        int handled = call(widget, event);
        tl_log_line("pvz2: the shovel: the guest's own function, handled=%d", handled);
        if (handled) guest_release_new_cursor("the shovel", board, begin_before, end_before,
                                              GUEST_SHOVEL_CURSOR_VTABLE);
        return;
    }
    /* Neither: the ordinary tap is the fallback, at the shovel's place as the
     * game reported it (the measured numbers are the host's). */
    g_shovel_touch_down = 0;
    if (atomic_load(&g_geometry_state) && g_shovel[2] > 0) {
        int x = g_shovel[0] + g_shovel[2] / 2, y = g_shovel[1] + g_shovel[3] / 2;
        tl_pvz2_touch(0, 1, (float)x, (float)y);
        g_shovel_touch_down = 1;
        tl_log_line("pvz2: the shovel: the ordinary tap at (%d,%d) (the guest's own button is not reachable)", x, y);
        return;
    }
    tl_log_line("pvz2: the shovel: the guest's own button is not reachable and its place is not known");
}

/* The guest's message-queue pump.
 *
 * The guest's own Java runs the jobs it queues for its app thread through this
 * (AndroidSurfaceView.PumpMessageQueue), and the host calls it once per frame.
 * It is also what runs the jobs the guest queues on the way out and on the way
 * back -- on Android the app thread keeps pumping until the process is frozen,
 * so those jobs are run even though the app is going away.
 *
 * Where the pump sits in the way-out sequence is a choice, and it is measured:
 * after the surface is gone (Android's own order) the guest's teardown jobs
 * fault, because they touch a renderer that is not there any more. So the pump
 * runs before the surface is taken away, and TL_PVZ2_PUMP_WHEN=after puts it
 * back where Android puts it, for the comparison. off leaves it out. */
typedef void (*pump_fn)(void *, void *);
static pump_fn g_pump;

static int pump_when(void)
{
    const char *w = getenv("TL_PVZ2_PUMP_WHEN");
    if (!w || !*w) return 1;                    /* before the surface goes: the safe order */
    if (!strcmp(w, "off")) return 0;
    if (!strcmp(w, "after")) return 2;
    return 1;
}

static void pump_once(const char *when)
{
    if (!g_pump) return;                        /* nothing registered: nothing to do */
    if (pump_when() == 0) return;
    tl_log_line("pvz2: PumpMessageQueue (lifecycle) -- %s", when);
    g_pump(tl_jni_env(), P.view);
}

/* The guest's app object, and the screen size it keeps in it.
 *
 * The guest reads the screen size once, at start-up, and keeps it in its app
 * object: the pair at +0x86c (the width in the low half, the height in the
 * high half) and copies of it at +0x744 and +0x748. When the surface changes,
 * the guest re-lays-out, but nothing on that path re-reads the size -- so the
 * app keeps the aspect it started with, and its hit-testing keeps looking for
 * the buttons where they were (the RE lane's write-up:
 * $D/swarm/R/SURFACE-RESIZE.md).
 *
 * The object is not in the guest's data (the converted library's data does not
 * sit at the guest's vaddrs, so the pointer chain the RE lane found does not
 * work here). It is on the heap instead, and it is found by its own
 * signature: the two copies of the size, side by side. The pair at +0x86c has
 * to agree with them, and the app's own size (+0xf4, +0xf8) has to be
 * sensible. Anything else is left alone.
 *
 * The address is kept: it is an address, so it needs 64 bits (the heap is
 * above 4 GB). */


/* The size the guest has been told about (the width in the top half). */
static atomic_ullong g_told_size;

/* The size that is in the guest's app object as far as the host knows: the
 * start-up size when the object was found, and the last size the host put
 * there after that. It is what the object is checked against before every
 * write, so a block the guest has freed is never written to. */
static atomic_ullong g_app_packed;

/* Whether the search for the app object is going on. It can be asked for
 * again: the guest can free the object, and then it has to be found once
 * more. */
static atomic_int g_finder_running;

/* The app's own size, as the host knows it: the guest's size when the object
 * was found, and the size the host wrote after that. The host needs it to
 * turn a point of the game's own 1748x804 space into a point of the surface:
 * the guest's UI keeps its places in the app's space, and the app's space is
 * mapped into the surface by the app's size. */

/* When the last attempt to tell the guest its size was made. A failed tell is
 * retried, but not on every frame: the retry is a handful of system calls and
 * a line in the log, and at sixty frames a second that is a flood. */
static double g_last_tell;

/* Read a word out of the guest's memory without the risk of a fault: the
 * kernel says "not readable" instead of killing the process. */
#ifdef PVZ2_HAVE_MACH_VM
static bool guest_read(uint64_t addr, void *out, size_t size)
{
    vm_offset_t data_out = 0;
    mach_msg_type_number_t got = 0;
    if (mach_vm_read(mach_task_self(), (mach_vm_address_t)addr, (mach_vm_size_t)size,
                     &data_out, &got) != KERN_SUCCESS || got != size) {
        if (data_out) mach_vm_deallocate(mach_task_self(), data_out, got);
        return false;
    }
    memcpy(out, (const void *)data_out, size);
    mach_vm_deallocate(mach_task_self(), data_out, got);
    return true;
}

/* The search: the two copies of the start-up size, side by side.
 *
 * The pattern is eight bytes and the chunks are four megabytes, so the chunks
 * overlap by eight bytes: a match that straddles a boundary is still found.
 *
 * The object is the guest's own: a C++ object on the heap. So the heap is
 * searched first -- the writable regions only -- because the whole address
 * space of a running game is hundreds of megabytes and copying all of it
 * through mach_vm_read on the user's machine is a waste of a second of CPU
 * and a lot of memory. The other regions are searched only if the heap has
 * nothing, so a guest that keeps its object somewhere else is still found. */
static uint64_t guest_app_find(int start_w, int start_h, bool writable_only)
{
    int32_t pattern[2] = { start_w, start_h };
    mach_vm_address_t addr = 0;
    mach_vm_size_t size = 0;
    unsigned long long scanned = 0;
    uint64_t found = 0;
    for (;;) {
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object_name = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &addr, &size,
                                           VM_REGION_BASIC_INFO_64,
                                           (vm_region_info_t)&info, &count,
                                           &object_name);
        if (kr != KERN_SUCCESS) break;
        if (object_name != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), object_name);
        if (size == 0) { addr += 4096; continue; }   /* the walk has to move */
        mach_vm_address_t region_end = addr + size;
        if ((info.protection & VM_PROT_READ) &&
            (!writable_only || (info.protection & VM_PROT_WRITE))) {
            for (mach_vm_address_t p = addr; p < region_end && !found; ) {
                mach_vm_size_t want = region_end - p;
                if (want > 4u * 1024 * 1024) want = 4u * 1024 * 1024;
                vm_offset_t data_out = 0;
                mach_msg_type_number_t got = 0;
                if (mach_vm_read(mach_task_self(), p, want, &data_out, &got) != KERN_SUCCESS
                    || got < sizeof pattern) {
                    if (data_out) mach_vm_deallocate(mach_task_self(), data_out, got);
                    p += want;
                    continue;
                }
                uint8_t *b = (uint8_t *)data_out;
                for (mach_msg_type_number_t i = 0; i + sizeof pattern <= got; i += 4) {
                    if (memcmp(b + i, pattern, sizeof pattern) != 0) continue;
                    if (i < 0x744) continue;
                    /* The address of the object itself, not of the copy. */
                    uint64_t app = (uint64_t)(uintptr_t)(p + i - 0x744);
                    uint64_t packed = 0;
                    if (!guest_read(app + 0x86c, &packed, sizeof packed)) continue;
                    if ((int)(uint32_t)packed != start_w ||
                        (int)(uint32_t)(packed >> 32) != start_h) continue;
                    int32_t size_fields[2] = { 0, 0 };
                    if (!guest_read(app + 0xf4, size_fields, sizeof size_fields)) continue;
                    int mw = size_fields[0], mh = size_fields[1];
                    if (mw <= 0 || mh <= 0 || mw > 100000 || mh > 100000) continue;
                    /* The type test. The size pair alone is not one: the screen
                     * size is a value the engine copies into a number of
                     * structures, so any object that happens to hold it twice
                     * and two positive numbers would pass. The design height
                     * (a float, the game's own content resolution) is not:
                     * an object that has one, in a sane range, at that offset,
                     * is the app object. */
                    float design_h = 0.0f;
                    if (!guest_read(app + 0x9c4, &design_h, sizeof design_h)) continue;
                    if (!(design_h > 1.0f && design_h < 100000.0f)) continue;
                    /* And the guest's own invariant: the app's height is the
                     * content resolution height (SetWidthHeight writes both,
                     * and it is the only thing that writes them). A buffer that
                     * happens to hold the pattern is very unlikely to hold that
                     * too -- and this object is written to, so it must be the
                     * right one. */
                    if (mh != (int)design_h) continue;
                    tl_log_line("pvz2: the guest's app object is %p (its screen size %dx%d, its size %dx%d)",
                                (void *)app, start_w, start_h, mw, mh);
                    found = app;
                    break;
                }
                mach_vm_deallocate(mach_task_self(), data_out, got);
                scanned += want;
                p += want > 8 ? want - 8 : want;   /* the chunks overlap */
            }
        }
        addr = region_end;
        if (found) break;      /* the walk is over: the object is found */
    }
    if (!found)
        tl_log_line("pvz2: the guest's app object was not found (%d x %d) after %llu bytes%s",
                    start_w, start_h, scanned, writable_only ? " (the writable regions)" : "");
    return found;
}

/* Write a word into the guest's memory, without the risk of a fault: the
 * kernel says "not writable" instead of killing the process. */
static bool guest_write_u32(uint64_t addr, int32_t value)
{
    kern_return_t kr = mach_vm_write(mach_task_self(), (mach_vm_address_t)addr,
                                     (vm_offset_t)&value, sizeof value);
    return kr == KERN_SUCCESS;
}

/* Write a pair of words into the guest's memory, as one store: the guest
 * reads the packed size with a single 64-bit load, and two 32-bit stores
 * could leave it a torn pair for a moment. */
static bool guest_write_u64(uint64_t addr, uint64_t value)
{
    kern_return_t kr = mach_vm_write(mach_task_self(), (mach_vm_address_t)addr,
                                     (vm_offset_t)&value, sizeof value);
    return kr == KERN_SUCCESS;
}
#else /* not iOS: the guest's app object cannot be found there */
/* The mach_vm calls do not exist on iOS (mach_vm.h says so), so the app
 * object's address cannot be found and the screen size cannot be written into
 * it. That is no loss on the iPad: its surface is the game's own size for
 * ever, so there is never a new size to tell the guest about. */
static bool guest_read(uint64_t addr, void *out, size_t size)
{ (void)addr; (void)out; (void)size; return false; }
static uint64_t guest_app_find(int start_w, int start_h, bool writable_only)
{ (void)start_w; (void)start_h; (void)writable_only; return 0; }
static bool guest_write_u32(uint64_t addr, int32_t value)
{ (void)addr; (void)value; return false; }
static bool guest_write_u64(uint64_t addr, uint64_t value)
{ (void)addr; (void)value; return false; }
#endif /* PVZ2_HAVE_MACH_VM */

/* The thread that looks for it: the game must not stop while the memory is
 * read. It starts once the guest is up (the object is created then) and keeps
 * the address for the surface changes that follow.
 *
 * The object is created while the guest starts up, so the search waits for it
 * and tries a few times: one pass is not enough if the game is slow to come
 * up. */
/* ---------------------------------------------------------------- the guest's own UI geometry
 *
 * The game's own code finds a UI element by name (guest 0x5beb44) and reads its
 * anchored rect (guest 0x5c1788). Both are reads of the widget tree and of the
 * UI layout map, and the host can call them: the converting route keeps the
 * guest's code native, so a guest address still names the code
 * (tl_aot_guest_code_ptr_any).
 *
 * Three things about the call, all from the RE lane's RECT-EXPORT.md:
 *
 *   - the name is not a C string: it is the game's own string object,
 *     {u8 flags; char chars[]; u64 heap}, where the flags byte holds the
 *     length << 1 (the short form) and the characters follow it;
 *   - the rect comes back BY VALUE, so the out pointer arrives in x8 (the
 *     AAPCS64 indirect-result register) and the widget in x0: there is no second
 *     argument, and a plain C call would pass the pointer in x1, where the
 *     guest does not look;
 *   - the call has to happen on the game thread, where the guest's tree is:
 *     the queries are made right after the frame, in the loop that draws it.
 *
 * The rect is in the app's own space (the game's design size), not in the
 * surface's, so it is scaled before the host taps it. */
#define GUEST_FIND_WIDGET    0x5beb44
#define GUEST_ANCHORED_RECT  0x5c1788

static void *guest_code(uint64_t guest_vaddr)
{
    /* The game's own library, by name: all four converted libraries start at
     * the guest's address 0, so a lookup that takes the largest one would
     * answer with the game's code whatever the address was (the RE lane's
     * finding G4). */
    return tl_aot_guest_code_ptr("libPVZ2.so", guest_vaddr);
}

/* ---- a guest call that may fault ----------------------------------------
 *
 * The game's rect getter reads a null pointer when the widget has no layout
 * entry (guest 0x5c1804 -> 0x5c1814 `ldr w1,[x20,#0xe4]`, x20 = 0), and the
 * guest's own SIGSEGV handler is not installed any more -- it does JNI work,
 * and a signal can be delivered to a thread with no JNI environment, so the
 * host's own crash reporting is the better one. A fault in there would
 * therefore end the process with nothing in the log.
 *
 * So every guest call is made inside a protected region: the fault is caught
 * here, the answer is "no answer", and the game carries on. The guest's code
 * between the entry and the return does not throw (the RE lane's finding G7):
 * a guest function that can throw needs a guest-side wrapper with a try/catch
 * around the call, not the host. */
static struct sigaction g_guest_prev_segv, g_guest_prev_bus;

static void guest_fault_handler(int sig)
{
    g_guest_fault = sig;
    siglongjmp(g_guest_jmp, 1);
}

static void guest_guard_enter(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = guest_fault_handler;
    sa.sa_flags = SA_NODEFER | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_guest_prev_segv);
    sigaction(SIGBUS, &sa, &g_guest_prev_bus);
}

static void guest_guard_leave(void)
{
    sigaction(SIGSEGV, &g_guest_prev_segv, NULL);
    sigaction(SIGBUS, &g_guest_prev_bus, NULL);
    g_guest_fault = 0;
}

/* A surface size that can be shown on: at least 100x100 (anything smaller is
 * a mistake, not a screen) and at most 8192 on a side. */
static bool surface_size_ok(int w, int h)
{
    return w >= 100 && h >= 100 && w <= 8192 && h <= 8192;
}

static void guest_globals_probe(void);
static void tl_pvz2_release_stuck_touches(void);
static void guest_orientation_changed(int orientation);

/* Find a UI element by name. The widget lives in the guest's heap, which is the
 * host's heap, so the pointer it returns is a host pointer. */
static void *guest_find_widget(const char *name)
{
    void *fn = guest_code(GUEST_FIND_WIDGET);
    if (!fn) return NULL;
    unsigned char obj[24];              /* the game's string object */
    memset(obj, 0, sizeof obj);
    size_t n = strlen(name);
    if (n > 20) return NULL;             /* too long for the short form */
    obj[0] = (unsigned char)(n << 1);    /* the length, in bits 1..7 */
    memcpy(obj + 1, name, n + 1);
    void *(*call)(void *) = (void *(*)(void *))fn;
    return call(obj);
}

/* The element's anchored rect: the out pointer is in x8, the widget in x0. */
static bool guest_anchored_rect(void *widget, int *out)
{
    void *fn = guest_code(GUEST_ANCHORED_RECT);
    if (!fn || !widget) return false;
    /* The call is written out because the ABI here is not the C one: the out
     * pointer is in x8, which a C call would not do (it would pass it in x1).
     * x16 holds the target (the usual place for an indirect call), x8 the out
     * pointer, x0 the widget. */
    register void *x16 __asm__("x16") = fn;
    register void *x8 __asm__("x8") = out;
    register void *x0 __asm__("x0") = widget;
    guest_guard_enter();
    if (sigsetjmp(g_guest_jmp, 1) == 0) {
        __asm__ volatile("blr %1"
                         : "+r"(x0), "+r"(x16)
                         : "r"(x8)
                         : "x1","x2","x3","x4","x5","x6","x7","x9","x10","x11","x12",
                           "x13","x14","x15","x17","memory","cc");
        guest_guard_leave();
        return true;
    }
    guest_guard_leave();
    tl_log_line("pvz2: the guest's own rect query faulted (signal %d); there is no answer",
                (int)g_guest_fault);
    return false;
}

/* The scale from the app's own space to the surface's.
 *
 * The game's design size is the app's logical size (3339x1536 for this game),
 * and the surface is the app's size as the host told it (1748x804): the same
 * shape, so the scale is the surface's height over the design's height. The
 * design height is in the guest's own app object; without it (the iPad, where
 * the app object cannot be found) the measured numbers are used. */
static double guest_surface_scale(void)
{
    uint64_t app = atomic_load(&g_app_object);
    if (!app) return 0.0;
    float design_h = 0.0f;
    if (!guest_read(app + 0x9c4, &design_h, sizeof design_h)) return 0.0;
    if (!(design_h > 1.0f && design_h < 100000.0f)) return 0.0;
    if (P.h <= 0) return 0.0;
    return (double)P.h / (double)design_h;
}

/* Ask the guest where the seed tray and the shovel are, and keep the answer.
 *
 * The measured numbers (the ones the shortcuts were calibrated with) are the
 * fallback; the log says which was used, so a test can see it. */
static void guest_ui_geometry(void)
{
    int tray[4] __attribute__((aligned(16))) = { 0, 0, 0, 0 };
    int shovel[4] __attribute__((aligned(16))) = { 0, 0, 0, 0 };
    void *tw = guest_find_widget("UI_SeedPackets");
    void *sw = guest_find_widget("UIShovel");
    bool got_tray = tw && guest_anchored_rect(tw, tray);
    bool got_shovel = sw && guest_anchored_rect(sw, shovel);
    if (!got_tray && !got_shovel) return;
    double s = guest_surface_scale();
    if (s <= 0.0) return;
    int new_tray[4], new_shovel[4];
    /* A rect that cannot be vouched for (no size, or outside the game's own
     * design) is not published: a tap at (0,0) is worse than the measured
     * numbers. */
    if (got_tray && (tray[2] <= 0 || tray[3] <= 0 ||
                     tray[0] < -1000 || tray[1] < -1000 ||
                     tray[0] > 100000 || tray[1] > 100000)) {
        tl_log_line("pvz2: the guest's seed tray has no usable rect (%d,%d %dx%d); the measured numbers stand",
                    tray[0], tray[1], tray[2], tray[3]);
        got_tray = false;
    }
    if (got_shovel && (shovel[2] <= 0 || shovel[3] <= 0 ||
                       shovel[0] < -1000 || shovel[1] < -1000 ||
                       shovel[0] > 100000 || shovel[1] > 100000)) {
        tl_log_line("pvz2: the guest's shovel has no usable rect (%d,%d %dx%d); the measured numbers stand",
                    shovel[0], shovel[1], shovel[2], shovel[3]);
        got_shovel = false;
    }
    if (got_tray) for (int i = 0; i < 4; i++) new_tray[i] = (int)(tray[i] * s);
    else for (int i = 0; i < 4; i++) new_tray[i] = g_tray[i];
    if (got_shovel) for (int i = 0; i < 4; i++) new_shovel[i] = (int)(shovel[i] * s);
    else for (int i = 0; i < 4; i++) new_shovel[i] = g_shovel[i];
    int state = (got_tray && got_shovel) ? 1 : 2;
    int before = atomic_load(&g_geometry_state);
    bool changed = before != state;
    for (int i = 0; i < 4; i++)
        if (g_tray[i] != new_tray[i] || g_shovel[i] != new_shovel[i]) changed = true;
    /* The payload is written first and the state published after it: a reader
     * that sees "there is an answer" must see the answer, not half of one. */
    pthread_mutex_lock(&g_geometry_lock);
    memcpy(g_tray, new_tray, sizeof g_tray);
    memcpy(g_shovel, new_shovel, sizeof g_shovel);
    pthread_mutex_unlock(&g_geometry_lock);
    atomic_store(&g_geometry_state, state);
    if (changed) {
        tl_log_line("pvz2: the guest's own geometry: the seed tray is %d,%d %dx%d and the shovel is %d,%d %dx%d (the surface's space, scale %.4f)",
                    g_tray[0], g_tray[1], g_tray[2], g_tray[3],
                    g_shovel[0], g_shovel[1], g_shovel[2], g_shovel[3], s);
    }
}

/* The guest's own UI geometry, in the surface's space. */
bool tl_pvz2_ui_geometry(int *tray, int *shovel)
{
    if (atomic_load(&g_geometry_state) == 0) return false;
    /* The copy is under the lock the writer takes: the writer publishes the
     * state after the payload, and the reader never sees half of one. */
    pthread_mutex_lock(&g_geometry_lock);
    if (tray) memcpy(tray, g_tray, sizeof g_tray);
    if (shovel) memcpy(shovel, g_shovel, sizeof g_shovel);
    pthread_mutex_unlock(&g_geometry_lock);
    return true;
}

static void *guest_app_finder(void *arg)
{
    (void)arg;
    for (int i = 0; i < 60 && !atomic_load(&P.running); i++) {
        usleep(250000);
    }
    for (int attempt = 0; attempt < 5; attempt++) {
        /* What the object holds: the start-up size, or -- after a resize --
         * the size the host last wrote into it. */
        uint64_t packed = atomic_load(&g_app_packed);
        int w, h;
        if (packed) {
            w = (int)(uint32_t)(packed & 0xffffffffu);
            h = (int)(uint32_t)(packed >> 32);
        } else {
            uint64_t sw = atomic_load(&P.start_wh);
            w = (int)(sw >> 32), h = (int)(sw & 0xffffffffu);
        }
        /* The heap first: the object is a C++ object, and the whole address
         * space of a running game is far too much to copy through mach_vm_read
         * on the person's machine. */
        uint64_t app = guest_app_find(w, h, true);
        if (app) {
            atomic_store(&g_app_object, (unsigned long long)app);
            /* What is in the object now: the size we were looking for. Every
             * write is checked against this, so a block the guest has freed is
             * never written to. */
            atomic_store(&g_app_packed, (unsigned long long)
                         (((uint64_t)(uint32_t)w) | ((uint64_t)(uint32_t)h << 32)));
            /* And the app's own size, which the guest had at the start: the
             * host needs it to place a point of the game's own space on the
             * surface. */
            atomic_store(&g_finder_running, 0);
            return NULL;
        }
        usleep(1000000);
    }
    /* The heap had nothing: the object is somewhere else after all, so the
     * whole address space is searched once. It is slow, and it is the last
     * try. */
    uint64_t packed = atomic_load(&g_app_packed);
    int w, h;
    if (packed) {
        w = (int)(uint32_t)(packed & 0xffffffffu);
        h = (int)(uint32_t)(packed >> 32);
    } else {
        uint64_t sw = atomic_load(&P.start_wh);
        w = (int)(sw >> 32), h = (int)(sw & 0xffffffffu);
    }
    uint64_t app = guest_app_find(w, h, false);
    if (app) {
        atomic_store(&g_app_object, (unsigned long long)app);
        atomic_store(&g_app_packed, (unsigned long long)
                     (((uint64_t)(uint32_t)w) | ((uint64_t)(uint32_t)h << 32)));
    }
    atomic_store(&g_finder_running, 0);
    return NULL;
}

/* The finder, on a thread of its own: the game must not stop while the
 * memory is read. It can be asked for again: the object can go away (the
 * guest frees it) and then it has to be found once more. */
static void start_app_finder(void)
{
    if (atomic_exchange(&g_finder_running, 1)) return;   /* already looking */
    pthread_t ft;
    if (pthread_create(&ft, NULL, guest_app_finder, NULL) == 0) {
        pthread_detach(ft);
    } else {
        atomic_store(&g_finder_running, 0);
    }
}


/* Is this still the guest's app object?
 *
 * The guest can free the block the object was in, and the memory can be given
 * to something else -- and a write there would corrupt it, silently, because
 * mach_vm_write cannot fault. So the object is checked before every write:
 * the size it holds must be the one the host put there (or the start-up size),
 * and its height must still be the content resolution height, which is the
 * guest's own invariant (SetWidthHeight writes both, and nothing else does).
 *
 * The check is four reads; the write is three. */
static bool guest_app_is_ours(uint64_t app)
{
    uint64_t expect = atomic_load(&g_app_packed);
    if (!expect) return false;
    uint64_t packed = 0;
    if (!guest_read(app + 0x86c, &packed, sizeof packed)) return false;
    if (packed != expect) return false;
    float design_h = 0.0f;
    if (!guest_read(app + 0x9c4, &design_h, sizeof design_h)) return false;
    if (!(design_h > 1.0f && design_h < 100000.0f)) return false;
    int32_t size_fields[2] = { 0, 0 };
    if (!guest_read(app + 0xf4, size_fields, sizeof size_fields)) return false;
    return size_fields[1] == (int)design_h;
}

/* Tell the guest the new screen size, so that it uses it: the stores are in
 * the app object the guest reads at start-up, and they are what the guest's
 * own SetWidthHeight would have computed (the RE lane's recipe). This runs on
 * the game thread, before Native_onSurfaceChanged. */
static bool guest_tell_screen_size(int w, int h)
{
    uint64_t app = atomic_load(&g_app_object);
    if (!app) {
        tl_log_line("pvz2: the guest's screen size stays %dx%d -- the app object is not known yet",
                    P.w, P.h);
        return false;
    }
    if (!guest_app_is_ours(app)) {
        /* The guest has freed the object, or it was never ours. Nothing is
         * written: the block may belong to something else now. The object is
         * looked for again -- the size it held is still known, so the search
         * knows what to look for. */
        tl_log_line("pvz2: the guest's app object at %p is not the object any more -- nothing is written",
                    (void *)app);
        atomic_store(&g_app_object, 0);
        start_app_finder();
        return false;
    }
    bool ok = true;
    /* The app's own size is computed first, and nothing is written until it
     * is known: half a size is worse than none (the guest would draw for one
     * size and look for its touches in another). */
    float design_h = 0.0f;
    if (!guest_read(app + 0x9c4, &design_h, sizeof design_h))
        design_h = 0.0f;
    if (!(design_h > 1.0f && design_h < 100000.0f)) {
        /* The app's own size could not be computed. The guest would then draw
         * for one size and look for its touches in another, which is the bug
         * this whole thing is about, so nothing is written and the caller is
         * told that it did not happen. */
        tl_log_line("pvz2: the guest's design height is not a number (%.1f); the app size was not written",
                    design_h);
        return false;
    }
    int design = (int)design_h;
    int small = w < h ? w : h;
    int design_w = (int)(((long long)w * design) / (small > 0 ? small : 1));
    /* The packed pairs, as one store each: the guest reads them with a single
     * 64-bit load. */
    uint64_t packed = ((uint64_t)(uint32_t)w) | ((uint64_t)(uint32_t)h << 32);
    ok &= guest_write_u64(app + 0x86c, packed);
    ok &= guest_write_u64(app + 0x744, packed);
    /* What is in the object now: this size. It is remembered before anything
     * else, because the check before the next write compares the object with
     * it -- and a later failure (the design height, say) must not leave the
     * host's own guard out of step with the object. */
    atomic_store(&g_app_packed, (unsigned long long)packed);
    /* And the app's own size. Two numbers decide where the guest draws and
     * where it looks for a tap, and they have to agree:
     *
     *   * the drawing is scaled by the guest's own GL view scale factor, which
     *     it computes as (the app's height) / (the smaller of the surface's two
     *     sides) -- the log says "GLViewScaleFactor -> .." after every resize;
     *   * the touch is rescaled with the app's size: the surface's pixels go
     *     into the app's space, which is (mWidth / surface width) and
     *     (mHeight / surface height).
     *
     * So the app's size is written the way the guest would write it if the
     * surface's smaller side were the one that decides: the height is the
     * design height (the content resolution's height, read out of the app
     * itself) and the width is the surface's width times that height over the
     * smaller side. In a window that is wider than it is tall (the usual case,
     * and the game's own shape) that is the same as the surface's aspect; in a
     * window that is taller than it is wide it is not, and the two numbers
     * would disagree -- the guest would draw its buttons in one place and look
     * for them in another, which is the bug this whole thing is about.
     */
    {
        ok &= guest_write_u32(app + 0xf4, design_w);
        ok &= guest_write_u32(app + 0xf8, design);
        tl_log_line("pvz2: the guest's app size is now %dx%d (the design is %d tall, the surface's smaller side is %d)",
                    design_w, design, design, small);
    }
    if (ok) {
        tl_log_line("pvz2: the guest's screen size is now %dx%d", w, h);
    } else {        /* The address is not ours to write: say so and leave it alone. */
        tl_log_line("pvz2: the guest's app object at %p cannot be written", (void *)app);
    }
    return ok;
}

static void *gl_main(void *arg)
{
    (void)arg;
    pthread_setname_np("PVZ2 GL");
    /* This thread's stack, so a fault inside the guest can be read against it. The
     * guest's own vector reallocation runs its copy down out of this stack, so the
     * bounds are worth having in the log. */
    { char *b = (char *)pthread_get_stackaddr_np(pthread_self());
      size_t n = pthread_get_stacksize_np(pthread_self());
      tl_log_line("pvz2: GL thread %p, stack %p..%p (%zu MiB)", pthread_self(),
                  b, b + n, n >> 20); }
    /* The guest's thread pointer, before any of its code runs on this thread. */
    tl_aot_enter();

    EGLDisplay (*getDisplay)(void *) = tl_egl_resolve("eglGetDisplay");
    unsigned (*initialize)(EGLDisplay, EGLint *, EGLint *) = tl_egl_resolve("eglInitialize");
    unsigned (*bindAPI)(unsigned) = tl_egl_resolve("eglBindAPI");
    unsigned (*chooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = tl_egl_resolve("eglChooseConfig");
    EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = tl_egl_resolve("eglCreateContext");
    EGLSurface (*createWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *) = tl_egl_resolve("eglCreateWindowSurface");
    unsigned (*makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = tl_egl_resolve("eglMakeCurrent");
    unsigned (*swapBuffers)(EGLDisplay, EGLSurface) = tl_egl_resolve("eglSwapBuffers");
    unsigned (*swapInterval)(EGLDisplay, EGLint) = tl_egl_resolve("eglSwapInterval");
    EGLint (*getError)(void) = tl_egl_resolve("eglGetError");

    /* Let lifecycle setup create the application object before the first
     * surface call. Keep the wait bounded so startup can also wait on GL. */
    for (int i = 0; i < 2000 && !atomic_load(&P.ready); i++) usleep(1000);
    if (!getDisplay || !createWindowSurface) { tl_log_line("pvz2: no EGL"); return NULL; }
    EGLDisplay dpy = getDisplay(NULL);
    EGLint maj = 0, min = 0;
    initialize(dpy, &maj, &min);
    bindAPI(0x30A0);
    const EGLint want[] = { 0x3040, 0x4, 0x3033, 0x4, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8,
                            0x3025, 16, 0x3026, 8, EGL_NONE };
    EGLConfig ec = NULL; EGLint n = 0;
    if (!chooseConfig(dpy, want, &ec, 1, &n) || n < 1) { tl_log_line("pvz2: eglChooseConfig failed (%#x)", getError()); return NULL; }
    const EGLint ca[] = { 0x3098, 2, EGL_NONE };
    EGLContext ctx = createContext(dpy, ec, NULL, ca);
    EGLSurface surf = createWindowSurface(dpy, ec, tl_nwindow_get(), NULL);
    if (!ctx || !surf || !makeCurrent(dpy, surf, surf, ctx)) { tl_log_line("pvz2: could not make GL current (%#x)", getError()); return NULL; }
    if (swapInterval) swapInterval(dpy, 1);
    tl_log_line("pvz2: EGL %d.%d ready", maj, min);

    typedef void (*v_fn)(void *, void *);
    typedef void (*ii_fn)(void *, void *, int, int);

    /* The surface's size, before the guest is told about it.
     *
     * A surface of zero -- or of the -1 that a size which is not known yet
     * becomes -- gives the guest a 1x1 picture: the game draws nothing, the
     * log fills with "resource not found", and the screen is black with no
     * reason in it. So the size is checked, the wait is until the host knows
     * one, and a size too small to be a screen is refused with a loud line.
     *
     * (This is the safety net under the host's own work: the iPad's display
     * is not always known when the scene starts, and the "no limit" sentinel
     * iPadOS hands out as the scene's maximum size is not a size.) */
    for (int i = 0; i < 600 && !surface_size_ok(P.w, P.h); i++) {
        /* The host may know the size by now: it is put into the wanted size,
         * and it is adopted here as the first size (the guest has not been
         * told about any size yet). */
        uint64_t wh = atomic_load(&P.want_wh);
        int w = (int)(wh >> 32), h = (int)(wh & 0xffffffffu);
        if (w > 0 && h > 0) { P.w = w; P.h = h; }
        if (surface_size_ok(P.w, P.h)) break;
        if ((i % 10) == 0)
            tl_log_line("pvz2: surface size not ready (%dx%d); waiting", P.w, P.h);
        usleep(100000);
    }
    if (!surface_size_ok(P.w, P.h)) {
        tl_log_line("pvz2: BLACK SCREEN RISK: the surface is %dx%d -- the guest is not told about it",
                    P.w, P.h);
        return NULL;
    }

    v_fn created = (v_fn)tl_jni_native(VIEW, "Native_onSurfaceCreated", "()V");
    ii_fn changed = (ii_fn)tl_jni_native(VIEW, "Native_onSurfaceChanged", "(II)V");
    v_fn draw = (v_fn)tl_jni_native(VIEW, "Native_onDrawFrame", "()V");
    v_fn pump = (v_fn)tl_jni_native(VIEW, "PumpMessageQueue", "()V");
    if (!draw) { tl_log_line("pvz2: Native_onDrawFrame is not registered"); return NULL; }
    if (!pump) tl_log_line("pvz2: PumpMessageQueue is not registered");
    g_pump = pump;

    if (created) { tl_log_line("pvz2: Native_onSurfaceCreated"); created(tl_jni_env(), P.view); }
    if (changed) {
        const char *d = getenv("TL_PVZ2_CHANGED_DELAY"); if (d) sleep((unsigned)atoi(d));
        tl_log_line("pvz2: Native_onSurfaceChanged(%d,%d)", P.w, P.h); changed(tl_jni_env(), P.view, P.w, P.h);
    }
    /* The surface change is queued; AndroidSurfaceView.PumpMessageQueue is what
     * runs it (and every other app-thread job) on a device. */
    if (pump) { tl_log_line("pvz2: PumpMessageQueue (surface change)"); pump(tl_jni_env(), P.view); }
    /* The engine handles the surface change on its app thread; a real
     * GLSurfaceView does not call onDrawFrame until that callback has run.
     * Give the app loop time to install its surface object before drawing. */
    if (getenv("TL_PVZ2_SURFACE_WAIT")) usleep((useconds_t)atoi(getenv("TL_PVZ2_SURFACE_WAIT")) * 1000);
    else usleep(250000);
    if (getenv("TL_PVZ2_HOLD")) { tl_log_line("pvz2: holding before the draw loop"); for (;;) sleep(1); }
    tl_log_line("pvz2: starting the draw loop");

    /* The guest's app object is looked for on a thread of its own: it takes a
     * moment, and the game must not stop for it. */
    start_app_finder();

    nb_independent_setup();

    atomic_store(&P.running, true);
    atomic_store(&P.loop_running, true);
    while (atomic_load(&P.running)) {
        /* A window resize: the surface is a new size, and the guest is told, the
         * way Android tells it -- onSurfaceChanged -> Native_onSurfaceChanged(w, h),
         * which is what makes the engine lay its UI out again. */
        /* The pair is read in one go: two separate reads could see the width of
         * one resize and the height of the next, and tell the guest a size
         * that was never asked for. */
        uint64_t wh = atomic_load(&P.want_wh);
        int want_w = (int)(wh >> 32), want_h = (int)(wh & 0xffffffffu);
        if (want_w > 0 && want_h > 0 && (want_w != P.w || want_h != P.h)) {
            tl_log_line("pvz2: the surface is now %dx%d (it was %dx%d)", want_w, want_h, P.w, P.h);
            P.w = want_w; P.h = want_h;
            /* The guest must be told the new size *and* made to use it: the
             * size it read at start-up is still in its app object, and nothing
             * on the surface-change path re-reads it (the RE lane's write-up:
             * $D/swarm/R/SURFACE-RESIZE.md). */
            if (guest_tell_screen_size(want_w, want_h))
                atomic_store(&g_told_size, ((unsigned long long)(uint32_t)want_w << 32)
                                           | (uint32_t)want_h);
            if (changed) {
                tl_log_line("pvz2: Native_onSurfaceChanged(%d,%d)", P.w, P.h);
                changed(tl_jni_env(), P.view, P.w, P.h);
            }
            if (pump) { tl_log_line("pvz2: PumpMessageQueue (surface change)"); pump(tl_jni_env(), P.view); }
            /* The guest's own re-layout runs in there, and it may write the
             * app's size back: read it and say what it says. Measured: it does
             * not (the size after the re-layout is the size the host wrote),
             * so the host does not write it again -- a fight over the value
             * would be worse than the guest keeping it. */
            if (atomic_load(&g_app_object)) {
                int32_t back[2] = { 0, 0 };
                if (guest_read(atomic_load(&g_app_object) + 0xf4, back, sizeof back)) {
                    tl_log_line("pvz2: after the guest's re-layout its app size is %dx%d",
                                back[0], back[1]);
                    int small = want_w < want_h ? want_w : want_h;
                    float design_h = 0.0f;
                    if (guest_read(atomic_load(&g_app_object) + 0x9c4,
                                   &design_h, sizeof design_h) &&
                        design_h > 1.0f && design_h < 100000.0f) {
                        int design = (int)design_h;
                        int design_w = (int)(((long long)want_w * design) /
                                              (small > 0 ? small : 1));
                        if (back[0] != design_w || back[1] != design)
                            tl_log_line("pvz2: the guest changed its own app size (%dx%d, the host wrote %dx%d)",
                                        back[0], back[1], design_w, design);
                    }
                }
            }
        }
        /* A resize that happened before the app object was found is not lost:
         * the guest is told the size it has not been told yet. The retry is
         * not on every frame: a failed tell is a handful of system calls and
         * a line in the log, and at sixty frames a second that is a flood. */
        if (atomic_load(&g_app_object)) {
            /* The wanted size is read again here: the copy at the top of the
             * loop can be a frame old, and a resize that lands in between
             * would be told to the guest late. */
            uint64_t wh2 = atomic_load(&P.want_wh);
            int w2 = (int)(wh2 >> 32), h2 = (int)(wh2 & 0xffffffffu);
            unsigned long long told = atomic_load(&g_told_size);
            unsigned long long now = ((unsigned long long)(uint32_t)w2 << 32)
                                     | (uint32_t)h2;
            if (w2 > 0 && h2 > 0 && told != now) {
                /* A failed tell is retried once a second, not on every frame:
                 * a failed tell is a handful of system calls and a line in the
                 * log, and at sixty frames a second that is a flood. */
                double t = now_s();
                if (t - g_last_tell > 1.0) {
                    g_last_tell = t;
                    if (guest_tell_screen_size(w2, h2))
                        atomic_store(&g_told_size, now);
                }
            }
        }
        /* The lifecycle request is handled even while the loop is paused --
         * that is exactly when the guest is told it is going away, and it must
         * save before iOS can kill the app. */
        int lc_seq = 0;
        int lc_what = lc_take_request(&lc_seq);
        if (lc_what == 1) {
            /* The state is set BEFORE the work: if the work is interrupted (the
             * wait times out, or the app is killed while it runs), the guest is
             * still known to be in the background, and the "back" that follows
             * is then really done instead of being skipped as "already done".
             * That skip is what left the game half-paused on the iPad: the
             * guest never heard that it came back. */
            atomic_store(&P.in_background, true);
            lifecycle_background_now();
            /* The guest has just been told that its surface is gone. On Android
             * the render thread stops there -- the view's surface is destroyed
             * when the activity is stopped, and the game is not asked to draw
             * again until it comes back. Drawing into the destroyed surface is
             * what crashed the app on the way out. */
            lc_report_done(lc_seq);
        } else if (lc_what == 2) {
            atomic_store(&P.in_background, false);
            lifecycle_foreground_now();
            lc_report_done(lc_seq);
        }
        if (atomic_load(&P.paused)) {
            /* The app is not in front, but the thread is still running: the
             * guest's jobs keep being queued and they are run here, so that a
             * job the guest queued on the way out (its save) is not left in the
             * queue until the app comes back. (A build whose view class has no
             * PumpMessageQueue has no pump to run.) */
            if (pump && pump_when() != 0) pump(tl_jni_env(), P.view);
            usleep(20000);
            continue;
        }
        /* The guest's surface is gone. The jobs it queues now are the ones that
         * tear it down, and its renderer is not there to answer them -- so the
         * queue is left alone until it comes back. The save it made on the way
         * out was made by the event itself, which is over. */
        if (atomic_load(&P.in_background)) {
            if (pump && pump_when() != 0) pump(tl_jni_env(), P.view);
            pace_frame(20000000L);   /* 20 ms: the guest's thread idles, it does not draw */
            continue;
        }
        if (pump) pump(tl_jni_env(), P.view);
        nb_pump();
        draw(tl_jni_env(), P.view);
        /* The guest's own UI geometry, asked of the guest on the game's own
         * thread, right after the frame: the widget tree is the guest's, and it
         * is the guest's thread that owns it. */
        if ((tl_pvz2_frames() % 30) == 0) guest_ui_geometry();
        /* The memory warning, on the game's own thread. */
        if (atomic_exchange(&g_low_memory_pending, 0)) guest_low_memory_warning();
        /* The host's own touches: any that are left down are released. */
        if ((tl_pvz2_frames() % 30) == 0) tl_pvz2_release_stuck_touches();
        /* The orientation change, on the game's own thread. */
        if (atomic_exchange(&g_orientation_pending, 0))
            guest_orientation_changed(atomic_load(&g_orientation_wanted));
        /* The native seed pick, on the game's own thread: the game's own
         * press handler, which is not the host's to call from anywhere else. */
        if (atomic_exchange(&g_seed_pick_pending, 0))
            guest_seed_pick(atomic_load(&g_seed_pick_slot));
        if (atomic_exchange(&g_collect_sun_pending, 0)) guest_collect_sun();
        if (g_pause_touch_down) {
            g_pause_touch_down = 0;
            tl_pvz2_touch(3, 1, (float)g_pause_touch_x, (float)g_pause_touch_y);
        } else if (atomic_exchange(&g_escape_pending, 0)) {
            guest_escape();
        }
        /* The native shovel, on the game's own thread (the touch path needs a
         * frame between the down and the up). */
        if (atomic_load(&g_shovel_pending)) {
            atomic_store(&g_shovel_pending, 0);
            guest_native_shovel();
        } else if (g_shovel_touch_down) {
            /* The second half of the touch: the up, a frame later. */
            g_shovel_touch_down = 0;
            int rect[4] __attribute__((aligned(16))) = { 0, 0, 0, 0 };
            void *widget = guest_find_widget("UIShovel");
            if (widget && guest_anchored_rect(widget, rect)) {
                double s = guest_surface_scale();
                tl_pvz2_touch(3, 1, (float)((int)(rect[0] * s) + rect[2] / 2),
                              (float)((int)(rect[1] * s) + rect[3] / 2));
                tl_log_line("pvz2: the shovel: the touch was let go");
            }
        }
        /* A test hook: a look at the guest's own globals, so the rotation
         * recipe's addresses can be checked against the running game. */
        {
            static int probed = 0;
            if (!probed && getenv("TL_PVZ2_TEST_GUESTS") && tl_pvz2_frames() >= 300) {
                probed = 1;
                guest_globals_probe();
            }
        }
        if (tl_jni_pending()) tl_jni_clear();
        swapBuffers(dpy, surf);
        if (!atomic_load(&g_started)) {
            static double last, smooth_since;
            double t = now_s();
            if (last == 0 || t - last > 0.15 || !atomic_load(&g_save_gate_read)) smooth_since = t;
            last = t;
            if (t - smooth_since >= 2.0) {
                atomic_store(&g_started, true);
                tl_log_line("pvz2: the game has started (its save is loaded, and it has run smoothly for 2 s)");
            }
        }
        /* A test hook: raise a signal on the game thread, so that the guest's
         * own handler for it can be seen to run. TL_PVZ2_TEST_SIGNAL=11 raises
         * SIGSEGV after TL_PVZ2_TEST_SIGNAL_DELAY frames (60 by default). */
        {
            static int sig = -2;
            static int at = 60;
            if (sig == -2) {
                const char *s = getenv("TL_PVZ2_TEST_SIGNAL");
                sig = s ? atoi(s) : 0;
                const char *d = getenv("TL_PVZ2_TEST_SIGNAL_DELAY");
                if (d && *d) at = atoi(d);
            }
            if (sig > 0 && tl_pvz2_frames() >= (unsigned)at) {
                tl_log_line("pvz2: the test raises signal %d on the game thread", sig);
                raise(sig);
            }
        }
        /* The same hook, but on a HOST thread -- one that is not the game's and
         * has no JNI environment. That is the case the signal review warns
         * about: a signal delivered to such a thread must never reach the
         * guest's own handler, which does JNI work and would crash there.
         *
         * TL_PVZ2_TEST_SIGNAL_THREAD=28 raises SIGWINCH (28) on a plain
         * thread, once, after TL_PVZ2_TEST_SIGNAL_DELAY frames (60 by
         * default). SIGWINCH is the right signal for the test: the guest
         * installs a handler for it (it installs one for every signal from 1
         * to 60) and the host's own default for it is to ignore the signal,
         * so the app survives either way and the log says what happened. */
        {
            static int done = 0;
            static int at = 60;
            static int sig = 0;
            if (!done) {
                const char *s = getenv("TL_PVZ2_TEST_SIGNAL_THREAD");
                sig = s ? atoi(s) : 0;
                const char *d = getenv("TL_PVZ2_TEST_SIGNAL_DELAY");
                if (d && *d) at = atoi(d);
                if (sig > 0 && tl_pvz2_frames() >= (unsigned)at) {
                    done = 1;
                    pthread_t th;
                    tl_log_line("pvz2: the test raises signal %d on a host thread", sig);
                    if (pthread_create(&th, NULL, raise_on_a_host_thread, (void *)(intptr_t)sig) == 0)
                        pthread_detach(th);
                }
            }
        }
        /* A test hook: turn the game the way the device would, so the rotation
         * path can be measured on the Mac (the guest's own log says whether it
         * adopted the new orientation). */
        {
            static int turned = 0;
            if (!turned && getenv("TL_PVZ2_TEST_ORIENTATION")) {
                const char *d = getenv("TL_PVZ2_TEST_ORIENTATION_DELAY");
                int at = d ? atoi(d) : 300;
                if (tl_pvz2_frames() >= (unsigned)at) {
                    turned = 1;
                    tl_pvz2_set_orientation(1);        /* portrait */
                }
            }
        }
        /* The rate follows the game's own setting, and it may change while
         * the game runs. */
        int target = atomic_load(&P.target_fps);
        pace_frame(1000000000L / (target > 0 ? target : 60));
    }
    return NULL;
}

bool tl_pvz2_run(void)
{
    /* The GL thread was started in tl_pvz2_start, before the lifecycle calls. */
    return P.started;
}

/* The window was resized and the game is to *adapt* to the new size, rather than
 * being drawn into a fixed surface and letterboxed: the guest is told the new
 * size the way Android tells it, and it lays its UI out again.
 *
 * The host asks for it; the GL thread does it, between frames, where the surface
 * change belongs. The letterbox path never comes here: the host keeps the
 * surface at the game's own size and only the presentation changes. */
void tl_pvz2_set_surface_size(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    atomic_store(&P.want_wh, ((uint64_t)(uint32_t)w << 32) | (uint32_t)h);
}

/* The touches the HOST injected (the shortcuts), and the ones that are still
 * down. A touch that is left down in the guest is a finger the player cannot
 * take back: the game waits for it to be lifted, and every later tap does
 * nothing. So the host's own touches are tracked here, and any that are still
 * down after their time is up are released, with a line in the log.
 *
 * The player's own touches are not tracked: they are the player's, and the
 * guest lifts them itself. */
#define INJ_TOUCHES 8
static atomic_int g_inj_id[INJ_TOUCHES];      /* the touch id, 0 = none */
static atomic_llong g_inj_since[INJ_TOUCHES];  /* when it went down */
static atomic_int g_inj_next;

void tl_pvz2_touch_injected(int phase, int id, float x, float y)
{
    if (phase == 0) {
        int slot = atomic_fetch_add(&g_inj_next, 1) % INJ_TOUCHES;
        atomic_store(&g_inj_id[slot], id + 1);
        atomic_store(&g_inj_since[slot], (long long)now_s() * 1000.0);
        tl_log_line("pvz2: the host's touch %d goes down at (%.0f,%.0f)", id, x, y);
    } else {
        for (int i = 0; i < INJ_TOUCHES; i++)
            if (atomic_load(&g_inj_id[i]) == id + 1) atomic_store(&g_inj_id[i], 0);
        tl_log_line("pvz2: the host's touch %d comes up", id);
    }
    tl_pvz2_touch(phase, id, x, y);
}

/* The host's touches that are still down, released: the log says which, so a
 * stuck finger is visible and not a mystery. */
static void tl_pvz2_release_stuck_touches(void)
{
    double now = now_s();
    for (int i = 0; i < INJ_TOUCHES; i++) {
        int id = atomic_load(&g_inj_id[i]);
        if (!id) continue;
        long long since = atomic_load(&g_inj_since[i]);
        if (now * 1000.0 - (double)since < 2000.0) continue;   /* two seconds */
        atomic_store(&g_inj_id[i], 0);
        tl_log_line("pvz2: the host's touch %d is still down; it is released", id - 1);
        tl_pvz2_touch(3, id - 1, 0.0f, 0.0f);
    }
}

void tl_pvz2_touch(int phase, int id, float x, float y)
{
    (void)id;
    pthread_mutex_lock(&TQ.mu);
    if (TQ.n == (int)(sizeof(TQ.v) / sizeof(TQ.v[0]))) {
        memmove(&TQ.v[0], &TQ.v[1], (sizeof(TQ.v) / sizeof(TQ.v[0]) - 1) * sizeof(TQ.v[0]));
        TQ.n--;
    }
    pvz_touch *t = &TQ.v[TQ.n++];
    t->phase = phase;
    t->x = (int)(x + 0.5f);
    t->y = (int)(y + 0.5f);
    t->time_s = now_s();
    t->id = touch_id_for(phase, id);
    if (phase == 0) { t->px = t->x; t->py = t->y; TQ.last_x = t->x; TQ.last_y = t->y; }
    else { t->px = TQ.last_x; t->py = TQ.last_y; TQ.last_x = t->x; TQ.last_y = t->y; }
    tl_log_line("pvz2: touch phase %d id %u at (%d,%d), queued %d", t->phase, t->id, t->x, t->y, TQ.n);
    pthread_mutex_unlock(&TQ.mu);
}

void tl_pvz2_text(const char *utf8)
{
    tl_pvz2_text_action(0, utf8);
}

/* The text event carries an action, the way Android's EditInputConnection
 * sends it: 0 is "these characters were committed", 3 is "one character was
 * deleted", 1 is "this is the text being composed", 2 is "the composing text
 * is finished". A host with a keyboard sends the same numbers, and the guest
 * cannot tell the two apart. */
void tl_pvz2_text_action(int action, const char *utf8)
{
    /* The guest's own EditInputConnection sends an empty text with the
     * delete (3) and the composing-finished (2) actions -- "one character
     * was deleted" carries no characters -- so only the commit (0) action
     * insists on text. */
    if (!utf8) return;
    if (!*utf8 && action == 0) return;
    pthread_mutex_lock(&TXQ.mu);
    if (TXQ.n == (int)(sizeof(TXQ.v) / sizeof(TXQ.v[0]))) {
        memmove(&TXQ.v[0], &TXQ.v[1], (size_t)(TXQ.n - 1) * sizeof(TXQ.v[0]));
        TXQ.n--;
    }
    pvz_text *t = &TXQ.v[TXQ.n++];
    snprintf(t->text, sizeof(t->text), "%s", utf8);
    t->action = action;
    t->time_s = now_s();
    tl_log_line("pvz2: text %d \"%s\", queued %d", action, t->text, TXQ.n);
    pthread_mutex_unlock(&TXQ.mu);
}
void tl_pvz2_key(int keycode, int down)
{
    pthread_mutex_lock(&KQ.mu);
    if (KQ.n == (int)(sizeof(KQ.v) / sizeof(KQ.v[0]))) {
        memmove(&KQ.v[0], &KQ.v[1], (sizeof(KQ.v) / sizeof(KQ.v[0]) - 1) * sizeof(KQ.v[0]));
        KQ.n--;
    }
    pvz_key *k = &KQ.v[KQ.n++];
    k->keycode = keycode;
    k->down = down != 0;
    k->time_s = now_s();
    tl_log_line("pvz2: key %d %s, queued %d", keycode, k->down ? "down" : "up", KQ.n);
    pthread_mutex_unlock(&KQ.mu);
}

void tl_pvz2_set_paused(bool paused) { atomic_store(&P.paused, paused); }

/* The display's own maximum (120 on an iPad Pro). The game's "Target 60 FPS"
 * setting asks for 60 through Graphics_SetFramerate, and the host answers
 * with this: the game is delta-driven -- its speed follows the clock, not
 * the number of frames -- so a faster presentation makes it smoother, not
 * faster. */
void tl_pvz2_set_max_fps(int fps)
{
    int max = fps > 0 ? fps : 60;
    if (atomic_exchange(&P.max_fps, max) == max) return;
    /* A new display (the window went to another screen): the game's own
     * setting is answered again with this display's maximum. */
    int asked = atomic_load(&P.asked_fps);
    if (asked > 0) {
        int target = tl_framerate_target(asked, max);
        atomic_store(&P.target_fps, target);
        tl_log_line("pvz2: the display can do %d fps now; the host presents at %d", max, target);
    }
}
int tl_pvz2_target_fps(void) { return atomic_load(&P.target_fps); }

/* Graphics_SetFramerate(int): the game's own setting, in frames per second.
 * The guest's Java implements it as a sleep before every frame, and that
 * sleep never runs here (the host calls the native update directly), so the
 * call used to be dropped and the setting did nothing. Now it sets the
 * host's rate: 60 (the "Target 60 FPS" setting) means the display's own
 * maximum, and 30 (the stock rate) means 30. */
static void J_set_framerate(tl_jcall *c);
unsigned long tl_pvz2_frames(void) { return tl_egl_frames_presented(); }

/* The guest is an Android game, and it expects its host to tell it when the
 * app goes to the background: it saves on that event, and the user quits
 * through the app switcher, which is exactly that event.
 *
 * The startup half of that lifecycle is driven in tl_pvz2_start (the
 * onActivityCreate/Start/Resume calls). The shutdown half was never written,
 * so the guest never learned it was going away and never saved on the way
 * out. The calls below are the ones the guest registers -- same objects, same
 * signatures, as the startup calls. */
typedef void (*lc_v_fn)(void *, void *);
typedef void (*lc_b_fn)(void *, void *, unsigned char);

/* The guest's own order, taken from its Java (AndroidGameApp.onActivityPause,
 * onWindowFocusChanged and onActivityResume):
 *
 *   going away: the window loses the focus, the app is no longer running, the
 *               audio focus is released, the app resigns active, the app is in
 *               the background.
 *   coming back: the app is running, the app becomes foreground, the window
 *               has the focus, the app becomes active.
 *
 * The order is the guest's, because the guest's engine is written against it:
 * what it saves, and when, hangs off these calls. */
static void lifecycle_background_now(void)
{
    lc_b_fn focus = (lc_b_fn)tl_jni_native(APP, "Native_NotifyFocusChange", "(Z)V");
    lc_b_fn running = (lc_b_fn)tl_jni_native(APP, "Native_NotifyAppRunning", "(Z)V");
    lc_v_fn audio = (lc_v_fn)tl_jni_native(APP, "Native_NotifyAudioLostFocus", "()V");
    lc_v_fn resign = (lc_v_fn)tl_jni_native(APP, "Native_applicationWillResignActive", "()V");
    lc_v_fn background = (lc_v_fn)tl_jni_native(APP, "Native_applicationDidEnterBackground", "()V");
    lc_b_fn surface = (lc_b_fn)tl_jni_native(APP, "Native_NotifySurfaceChange", "(Z)V");

    tl_log_line("pvz2: lifecycle -> background (focus=%p running=%p audio=%p resign=%p background=%p surface=%p)",
                (void *)focus, (void *)running, (void *)audio, (void *)resign, (void *)background, (void *)surface);
    if (focus) focus(tl_jni_env(), P.gameapp, 0);
    if (running) running(tl_jni_env(), P.gameapp, 0);
    if (audio) audio(tl_jni_env(), P.gameapp);
    if (resign) resign(tl_jni_env(), P.gameapp);
    if (background) background(tl_jni_env(), P.gameapp);
    /* The pump: the guest's Java queues jobs for its own app thread, and this is
     * what runs them. On Android the app thread keeps pumping until the process
     * is frozen, so the jobs the guest queued for the way out are run even though
     * the app is going away -- the host used to run none of them, which left one
     * job (the guest's teardown) undone.
     *
     * It runs here, before the surface is taken away, because the teardown jobs
     * touch the renderer: with the surface gone they fault (measured: a wild
     * pointer, then a null pointer), and a fault on the way out is the one thing
     * the app must not do. TL_PVZ2_PUMP_WHEN=after puts the pump where Android
     * puts it (after the surface is gone), and off leaves it out; the default is
     * before. */
    pump_once("the app is going to the background");
    /* The surface goes away with the app -- on Android the view's surface is
     * destroyed when the activity is stopped, and the game is told so. The
     * drawable is released here too, in the host. */
    if (surface) surface(tl_jni_env(), P.gameapp, 0);
    if (pump_when() == 2) pump_once("the app is going to the background, after the surface");
    tl_log_line("pvz2: lifecycle -> background done");
}

static void lifecycle_foreground_now(void)
{
    lc_b_fn running = (lc_b_fn)tl_jni_native(APP, "Native_NotifyAppRunning", "(Z)V");
    lc_v_fn foreground = (lc_v_fn)tl_jni_native(APP, "Native_applicationWillBecomeForeground", "()V");
    lc_b_fn focus = (lc_b_fn)tl_jni_native(APP, "Native_NotifyFocusChange", "(Z)V");
    lc_v_fn didactive = (lc_v_fn)tl_jni_native(APP, "Native_applicationDidBecomeActive", "()V");
    lc_v_fn audio = (lc_v_fn)tl_jni_native(APP, "Native_NotifyAudioGainedFocus", "()V");
    lc_b_fn surface = (lc_b_fn)tl_jni_native(APP, "Native_NotifySurfaceChange", "(Z)V");

    tl_log_line("pvz2: lifecycle -> foreground (running=%p foreground=%p focus=%p didactive=%p audio=%p surface=%p)",
                (void *)running, (void *)foreground, (void *)focus, (void *)didactive, (void *)audio, (void *)surface);
    if (running) running(tl_jni_env(), P.gameapp, 1);
    if (foreground) foreground(tl_jni_env(), P.gameapp);
    if (surface) surface(tl_jni_env(), P.gameapp, 1);
    if (focus) focus(tl_jni_env(), P.gameapp, 1);
    if (didactive) didactive(tl_jni_env(), P.gameapp);
    if (audio) audio(tl_jni_env(), P.gameapp);
    /* The mirror of the pump on the way out: the jobs the guest queued while it
     * was away are run now, before the first frame is drawn again. */
    pump_once("the app is coming back");
    tl_log_line("pvz2: lifecycle -> foreground done");
}

/* The guest's engine is not built to be poked from a foreign thread, so the
 * lifecycle is *requested* here and *performed* on the GL thread, between
 * frames. The caller waits for it, because iOS gives only a few seconds
 * before it may kill the app, and the guest's save must be written inside
 * that window.
 *
 * The requests are a queue, not a slot: a foreground request that arrives
 * while a background request is still pending must not replace it (the guest
 * would be told "foreground" without ever having been told "background", and
 * the save that the background event is for would never happen). It is a ring
 * of slots: an empty queue has head == tail, and that is not full. */
static pthread_mutex_t LC_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t LC_cv = PTHREAD_COND_INITIALIZER;
#define LC_QUEUE 8
static int LC_queue[LC_QUEUE];      /* 1 = background, 2 = foreground */
static int LC_queue_seq[LC_QUEUE];   /* and the request's number */
static int LC_head, LC_tail;         /* the queue's ends */
static int LC_seq;                   /* the request's number */
static int LC_done;                  /* the request number that was handled */

static int lc_take_request(int *seq_out)
{
    pthread_mutex_lock(&LC_mu);
    int want = 0;
    if (LC_head != LC_tail) {
        want = LC_queue[LC_head];
        *seq_out = LC_queue_seq[LC_head];
        LC_head = (LC_head + 1) % LC_QUEUE;
    }
    pthread_mutex_unlock(&LC_mu);
    return want;
}

static void lc_report_done(int seq)
{
    pthread_mutex_lock(&LC_mu);
    if (seq > LC_done) LC_done = seq;
    pthread_cond_broadcast(&LC_cv);
    pthread_mutex_unlock(&LC_mu);
}

static void lifecycle_request(int what)
{
    /* The same event twice is the same event: the guest is not torn down
     * twice (its surface is released once, and releasing it twice is a
     * double-free in the engine). */
    static atomic_int bg_asked;      /* a background event is on its way */
    /* What is queued matters as much as what has been done: a foreground that
     * arrives while the background is still queued is not "already done" (the
     * guest has not been told it went away yet), and dropping it would leave
     * the guest in a state it was never in. */
    int queued_bg = 0, queued_fg = 0;
    pthread_mutex_lock(&LC_mu);
    for (int i = LC_head; i != LC_tail; i = (i + 1) % LC_QUEUE) {
        if (LC_queue[i] == 1) queued_bg = 1; else queued_fg = 1;
    }
    pthread_mutex_unlock(&LC_mu);
    if (what == 1) {
        if (atomic_load(&P.in_background) || atomic_exchange(&bg_asked, 1) || queued_bg) {
            tl_log_line("pvz2: the lifecycle request (background) is already on its way");
            return;
        }
    } else {
        atomic_store(&bg_asked, 0);
        if (!atomic_load(&P.in_background) && !queued_bg && !queued_fg) {
            tl_log_line("pvz2: the lifecycle request (foreground) is already done");
            return;
        }
    }
    if (!P.started) {                 /* the guest never came up: nothing to tell */
        return;
    }
    if (!atomic_load(&P.loop_running)) {
        /* The draw loop has not started yet -- the guest is still coming up,
         * and the GL thread may be inside the guest right now (the surface
         * callbacks). Calling the guest from this thread at that moment would
         * be two threads in the engine at once, which is what the game is not
         * built for. So the request waits in the queue, and the loop does it
         * when it comes up. */
        pthread_mutex_lock(&LC_mu);
        int seq0 = ++LC_seq;
        int next0 = (LC_tail + 1) % LC_QUEUE;
        if (next0 == LC_head) {
            LC_head = (LC_head + 1) % LC_QUEUE;
            tl_log_line("pvz2: the lifecycle queue is full; the oldest request is dropped");
        }
        LC_queue[LC_tail] = what;
        LC_queue_seq[LC_tail] = seq0;
        LC_tail = next0;
        tl_log_line("pvz2: the lifecycle request %d waits for the draw loop", seq0);
        pthread_mutex_unlock(&LC_mu);
        return;
    }
    pthread_mutex_lock(&LC_mu);
    int seq = ++LC_seq;
    int next = (LC_tail + 1) % LC_QUEUE;
    if (next == LC_head) {
        /* The queue is full (an empty queue has head == tail, and it is not
         * full): the oldest request is dropped, and the log says so (a
         * dropped request is a lost event, and it must be visible). */
        LC_head = (LC_head + 1) % LC_QUEUE;
        tl_log_line("pvz2: the lifecycle queue is full; the oldest request is dropped");
    }
    LC_queue[LC_tail] = what;
    LC_queue_seq[LC_tail] = seq;
    LC_tail = next;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (what == 1) ? 5 : 2;
    while (LC_done < seq) {
        if (pthread_cond_timedwait(&LC_cv, &LC_mu, &deadline) == ETIMEDOUT) {
            tl_log_line("pvz2: the lifecycle request %d was not handled in time", seq);
            break;
        }
    }
    tl_log_line("pvz2: the lifecycle request %d is done (done=%d)", seq, LC_done);
    pthread_mutex_unlock(&LC_mu);
}

void tl_pvz2_lifecycle_background(void) { lifecycle_request(1); }
void tl_pvz2_lifecycle_foreground(void) { lifecycle_request(2); }

/* The rotation path: the guest adopts the new orientation.
 *
 * Off by default: the game is a landscape game, and the iPad plays it in
 * landscape (the host tells the guest that the device turned, and the guest
 * stays in landscape). TL_PVZ2_ORIENTATION=1 turns the path on, and then the
 * guest is given the new orientation and re-lays out for it, the way Android
 * does.
 *
 * The recipe (the RE lane's ORIENTATION.md):
 *
 *   1. the size pair: the guest reads it at app+0x86c and derives its own width
 *      from it -- written here, in the guest's own memory. Its data segment is
 *      mapped in this process at a known place, so no mach_vm is needed (there
 *      is none on iOS);
 *   2. the guest's own LawnApp::SetWidthHeight, which recomputes the size and
 *      re-lays out;
 *   3. Native_onSurfaceChanged and Native_onOrientationChanged, which POST
 *      jobs;
 *   4. the pump, which runs them.
 */
#define GUEST_DRIVER_GLOBAL     0x25a4e50    /* the driver's own global */
#define GUEST_ORIENTATION_CACHE 0x2555610    /* the orientation the guest believes in */
#define GUEST_SET_WIDTH_HEIGHT  0x93ead0     /* LawnApp::SetWidthHeight(app, height) */

/* The guest's own object: the driver's global holds the driver, and the driver
 * holds the LawnApp at +0x98. */
static void *guest_app_object(void)
{
    void *global = tl_aot_guest_data_ptr_any(GUEST_DRIVER_GLOBAL);
    if (!global) return NULL;
    uint64_t driver = *(uint64_t *)global;
    if (!driver) return NULL;
    return (void *)(uintptr_t)*(uint64_t *)(driver + 0x98);
}

static bool orientation_path_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *s = getenv("TL_PVZ2_ORIENTATION");
        on = (s && s[0] == '1') ? 1 : 0;
    }
    return on == 1;
}

/* A look at the guest's own globals, for the addresses: the log says what is
 * there, so the recipe's addresses can be checked against the running game
 * instead of against a disassembly. */
static void guest_globals_probe(void)
{
    static const uint64_t cache_candidates[] = { 0x2555610, 0x2455610 };
    static const uint64_t driver_candidates[] = { 0x25a4e50, 0x259a4e50 };
    for (size_t i = 0; i < sizeof cache_candidates / sizeof *cache_candidates; i++) {
        void *p = tl_aot_guest_data_ptr_any(cache_candidates[i]);
        int32_t v = p ? *(int32_t *)p : -1;
        tl_log_line("pvz2: the guest's globals: at %llx is %d%s",
                    (unsigned long long)cache_candidates[i], v,
                    p ? "" : " (not in the converted data)");
    }
    for (size_t i = 0; i < sizeof driver_candidates / sizeof *driver_candidates; i++) {
        void *p = tl_aot_guest_data_ptr_any(driver_candidates[i]);
        uint64_t v = p ? *(uint64_t *)p : 0;
        tl_log_line("pvz2: the guest's globals: at %llx is %llx%s",
                    (unsigned long long)driver_candidates[i], (unsigned long long)v,
                    p ? "" : " (not in the converted data)");
    }
    void *app = guest_app_object();
    tl_log_line("pvz2: the guest's globals: the app object is %p", app);
    uint64_t code = 0, data = 0, dbase = 0, csize = 0, dsize = 0;
    tl_aot_layout("libPVZ2.so", &code, &data, &dbase, &csize, &dsize);
    tl_log_line("pvz2: the guest's globals: the code is at %llx (%llu bytes), the data at %llx (%llu bytes), and the data starts at the guest's %llx",
                (unsigned long long)code, (unsigned long long)csize,
                (unsigned long long)data, (unsigned long long)dsize,
                (unsigned long long)dbase);
}

/* The device turned. The guest asks the device for the orientation when its
 * configuration changes, and the answer tells it which way up the game is --
 * 4 for landscape, 3 for the same landscape turned over (the 180-degree flip
 * the player makes when they turn the tablet the other way). The guest
 * re-lays-out when it changes, so the host has to tell it.
 *
 * The request is posted and done on the game's own thread: the guest's
 * re-layout and the guest's job queue belong to that thread, and doing them on
 * the host's main thread is two threads in the engine at once (the RE lane's
 * finding G2). */
static void guest_orientation_changed(int orientation)
{
    int old = P.orientation;
    P.orientation = orientation;
    tl_log_line("pvz2: the orientation is now %d (was %d)", orientation, old);
    /* The same dispatcher the guest's Java uses when the configuration changes:
     * Native_onOrientationChanged(old, new). */
    typedef void (*oi_fn)(void *, void *, int, int);
    oi_fn changed = (oi_fn)tl_jni_native(APP, "Native_onOrientationChanged", "(II)V");
    /* The rotation path (off by default): the guest is given the new
     * orientation and re-lays out for it. The size pair first (the guest reads
     * it at app+0x86c and derives its own width from it), then the guest's own
     * recompute, then the surface change. */
    if (orientation_path_on()) {
        typedef void (*ii_fn)(void *, void *, int, int);
        void *app = guest_app_object();
        int w = P.w, h = P.h;
        if (orientation == 1 || orientation == 2) { int t = w; w = h; h = t; }
        if (app) {
            int32_t *pair = (int32_t *)((uint8_t *)app + 0x86c);
            int32_t *copy = (int32_t *)((uint8_t *)app + 0x744);
            pair[0] = w; pair[1] = h;
            copy[0] = w; copy[1] = h;
            void *fn = tl_aot_guest_code_ptr_any(GUEST_SET_WIDTH_HEIGHT);
            if (fn) {
                void (*set_wh)(void *, int) = (void (*)(void *, int))fn;
                set_wh(app, h);
                tl_log_line("pvz2: the guest was told the new size %dx%d and re-laid out", w, h);
            } else {
                tl_log_line("pvz2: the guest's SetWidthHeight is not in the converted code");
            }
            ii_fn sc = (ii_fn)tl_jni_native(VIEW, "Native_onSurfaceChanged", "(II)V");
            if (sc) sc(tl_jni_env(), P.view, w, h);
        } else {
            tl_log_line("pvz2: the guest's app object is not known; the size pair was not written");
        }
        void *cache = tl_aot_guest_data_ptr_any(GUEST_ORIENTATION_CACHE);
        if (cache) {
            *(int32_t *)cache = orientation;
            tl_log_line("pvz2: the guest's own orientation is now %d", orientation);
        }
    }
    if (changed) changed(tl_jni_env(), P.gameapp, old, orientation);
    /* The guest's orientation change POSTS A JOB (guest 0x16658b8 -> 0x1670578 ->
     * the queue's post 0x1670250), so it is only run when the queue is pumped.
     * The per-frame pump would run it a frame later; the rotation path pumps
     * here, so the guest hears about the rotation at once.
     * (The RE lane's ORIENTATION.md, section 6.4.) */
    pump_once("the orientation changed");
}

void tl_pvz2_set_orientation(int orientation)
{
    if (orientation == P.orientation) return;
    /* The work is done on the game's own thread (the draw loop), not here. */
    atomic_store(&g_orientation_wanted, orientation);
    atomic_store(&g_orientation_pending, 1);
}
