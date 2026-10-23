/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * bionic-shim -- the platform a converted guest library binds to.
 *
 * A converted library is a normal dylib: its imports are ordinary dynamic links. Some of
 * them are the *other* converted libraries (the game library exports the inline-hooking
 * API its engine libraries call), and the rest are what Android's runtime would have
 * provided: the C library, the C++ runtime, OpenGL, and a handful of vendor libraries.
 *
 * Those come from here. Every function is deliberately small and honest:
 *
 *   - where Darwin already has the same function (pthread_*, malloc, memcpy, ...), this
 *     file does not define it, so the bind goes to libSystem. That is the right answer
 *     and it is what the conversion's own tests exercise.
 *   - where Darwin's equivalent differs in a way the guest cannot see (errno as a macro,
 *     a different name for the same call), the function forwards.
 *   - where there is nothing to forward to, the function is a stub that reports itself and
 *     returns a benign value. A guest library can be *loaded* with any number of these
 *     present; it is only when one is called that the gap shows, and then loudly.
 *
 * Add what a library actually needs as the conversions find it: the converter's link step
 * names the symbols it cannot resolve, which is the list to work from.
 */
/* The guest's names are C names (the ELF's dynamic symbols carry no underscore and no
 * C++ mangling), so these have C linkage whatever driver compiles this file -- clang++
 * would otherwise export __Z7__errnov and the bind would not resolve. */
#ifdef __cplusplus
extern "C" {
#endif

/* Which platform this is. The only thing in the file that asks is the
 * __cxa_thread_atexit further down, which only some of the platforms have. */
#include <TargetConditionals.h>

/* The layout the converter bakes the mod's hooks in: the stub the branch at a hooked site
 * points at, and the offsets of the slot and the trampoline inside it. Both ends read the
 * same file, so the numbers cannot drift (scripts/hook-layout.h). */
#include "hook-layout.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <execinfo.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/mount.h>
#include <grp.h>
#include <pwd.h>
#include <time.h>
#include <unistd.h>

/* The shim's own messages go through the host's own printf. The guest's printf family,
 * further down, is the Android-shaped one, and the two conventions are opposites, so the
 * shim cannot use the guest's: its own text would be read out of the wrong place.
 *
 * The host's printf takes its arguments on the stack and its va_list is a single
 * pointer, so the shim's calls go to the v-forms: a va_list that came from the host's
 * va_start can only be handed to a function that takes one. */
void husk_shim_logf(const char *fmt, ...)
{
    static int (*real)(FILE *, const char *, va_list);
    if (!real) *(void **)(&real) = dlsym(RTLD_NEXT, "vfprintf");
    va_list ap;
    va_start(ap, fmt);
    if (real) real(stderr, fmt, ap);
    va_end(ap);
}

/* The host's snprintf, for the same reason. */
static void shim_snprintf(char *b, size_t l, const char *fmt, ...)
{
    static int (*real)(char *, size_t, const char *, va_list);
    if (!real) *(void **)(&real) = dlsym(RTLD_NEXT, "vsnprintf");
    va_list ap;
    va_start(ap, fmt);
    if (real) real(b, l, fmt, ap);
    va_end(ap);
}

/* How a gap announces itself. Once per symbol, so a guest in a loop is not a flood. */
static void shim_missing(const char *name)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static __thread const char *reported;
    if (name == reported)
        return;
    reported = name;
    pthread_mutex_lock(&lock);
    husk_shim_logf("bionic-shim: %s is not implemented yet\n", name);
    fflush(stderr);
    pthread_mutex_unlock(&lock);
}

/* ------------------------------------------------------------------ logging */

/* The guest's log calls. The Android-shaped entry points are further down: __android_log_print
 * and __android_log_vprint are there, because a variadic call and a call with a va_list
 * reach the guest's code by two different routes and both have to be the guest's own. */
int __android_log_write(int prio, const char *tag, const char *text)
{
    (void)prio;
    (void)tag;
    husk_shim_logf("%s\n", text ? text : "");
    return 1;
}

/* The guest reads errno through a function on Android; on Darwin it is a macro over
 * __error(). */
int *__errno(void)
{
    return __error();
}

int __ctype_get_mb_cur_max(void)
{
    return 4;
}

/* ------------------------------------------------------------------ files */

/* The shim's own open, openat and fcntl, with the variadic tail taken as an ordinary
 * argument. Defined with the file hooks below, where the host's own VFS is known. */
static int shim_open(const char *path, int flags, unsigned mode);
static int shim_openat(int d, const char *p, int f, unsigned mode);
static int shim_fcntl(int fd, int cmd, long arg);

int __open_2(const char *path, int flags)
{
    /* Android's "force close-on-exec" variant. Darwin has O_CLOEXEC already. */
    return shim_open(path, flags | O_CLOEXEC, 0666);
}

int __openat(int dirfd, const char *path, int flags, int mode)
{
    return shim_openat(dirfd, path, flags | O_CLOEXEC, mode);
}

int __dup(int fd)
{
    return shim_fcntl(fd, F_DUPFD_CLOEXEC, 0);
}

ssize_t __read_chk(int fd, void *buf, size_t n, size_t buflen)
{
    if (n > buflen) {
        errno = EINVAL;
        return -1;
    }
    return read(fd, buf, n);
}

ssize_t __write_chk(int fd, const void *buf, size_t n, size_t buflen)
{
    if (n > buflen) {
        errno = EINVAL;
        return -1;
    }
    return write(fd, buf, n);
}

int __fstat(int fd, struct stat *st)
{
    return fstat(fd, st);
}

int __lstat(const char *path, struct stat *st)
{
    return lstat(path, st);
}

int __getpid(void)
{
    return getpid();
}

int __gettid(void)
{
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (int)tid;
}

pid_t __getppid(void)
{
    return getppid();
}

long __sysconf(int name)
{
    return sysconf(name);
}

int __isatty(int fd)
{
    return isatty(fd);
}

int __android_access(const char *path, int mode)
{
    return access(path, mode);
}

int __android_faccessat(int dirfd, const char *path, int mode, int flags)
{
    return faccessat(dirfd, path, mode, flags);
}

/* Android's networking helpers name things differently; nothing in the game calls these
 * on the paths we run, so they announce the gap rather than guessing. */
int __android_getaddrinfofornet(void)
{
    shim_missing("__android_getaddrinfofornet");
    return -1;
}

/* ------------------------------------------------------------------ Linux-isms */

/* The engine reads these to size its own structures. Bionic exports the kernel's answers;
 * what matters here is that the values are the ones Android would give. */
unsigned long __getauxval(unsigned long type)
{
    switch (type) {
    case 25:                                  /* AT_PAGESZ */
        return 4096;
    case 6:                                   /* AT_PAGESIZE */
        return 4096;
    case 16:                                  /* AT_HWCAP */
        return 0;
    default:
        return 0;
    }
}

int android_set_abort_message(const char *msg)
{
    /* Darwin aborts with its own message; the guest's is a diagnostic, not a contract. */
    (void)msg;
    return 0;
}

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *, size_t, void *), void *data)
{
    /* The guest walks the loaded images to find its own library and the ones beside it.
     * On iOS this is the wrong tool anyway (the image list is not the process's), and the
     * converter's runtime answers from its own tables instead. */
    (void)callback;
    (void)data;
    return 0;
}

/* epoll and eventfd are how bionic's timers and sockets wait. The guest only reaches
 * these through the game's own threading code, which the runtime drives differently. */
int eventfd(unsigned int count, int flags)
{
    (void)count;
    (void)flags;
    shim_missing("eventfd");
    errno = ENOSYS;
    return -1;
}

int epoll_create1(int flags)
{
    (void)flags;
    shim_missing("epoll_create1");
    errno = ENOSYS;
    return -1;
}

int epoll_ctl(int epfd, int op, int fd, void *event)
{
    (void)epfd;
    (void)op;
    (void)fd;
    (void)event;
    errno = ENOSYS;
    return -1;
}

int epoll_wait(int epfd, void *events, int maxevents, int timeout)
{
    (void)epfd;
    (void)events;
    (void)maxevents;
    (void)timeout;
    errno = ENOSYS;
    return -1;
}

/* ------------------------------------------------------------------ time */

/* Android's clock ids are not Darwin's. CLOCK_MONOTONIC is 1 in bionic and 6 on
 * Darwin; CLOCK_PROCESS_CPUTIME_ID and CLOCK_THREAD_CPUTIME_ID are 2 and 3 against
 * Darwin's 12 and 16. The guest's C++ runtime calls clock_gettime(CLOCK_MONOTONIC)
 * while it starts up, and Darwin rejects 1 with EINVAL, which libc++ turns into a
 * system_error and an abort. Map the guest's ids before the call.
 *
 * The real function is looked up through libSystem rather than called by name: this
 * definition has the same name, so a direct call would recurse into itself. */
static int shim_real_clock_gettime(clockid_t clk, struct timespec *ts)
{
    static int (*real)(clockid_t, struct timespec *);
    if (!real) {
        void *sys = dlopen("/usr/lib/libSystem.B.dylib", RTLD_LAZY | RTLD_LOCAL);
        if (sys) *(void **)(&real) = dlsym(sys, "clock_gettime");
        if (!real) *(void **)(&real) = dlsym(RTLD_NEXT, "clock_gettime");
        if (!real) *(void **)(&real) = dlsym(RTLD_DEFAULT, "clock_gettime");
    }
    if (!real) {
        errno = ENOSYS;
        return -1;
    }
    return real(clk, ts);
}

int clock_gettime(clockid_t clk, struct timespec *ts)
{
    clockid_t host;
    switch (clk) {
    case 0: host = CLOCK_REALTIME; break;              /* CLOCK_REALTIME */
    case 1: host = CLOCK_MONOTONIC; break;             /* CLOCK_MONOTONIC */
    case 2: host = CLOCK_PROCESS_CPUTIME_ID; break;    /* CLOCK_PROCESS_CPUTIME_ID */
    case 3: host = CLOCK_THREAD_CPUTIME_ID; break;     /* CLOCK_THREAD_CPUTIME_ID */
    case 4: host = CLOCK_MONOTONIC_RAW; break;         /* CLOCK_MONOTONIC_RAW */
    case 5: host = CLOCK_REALTIME; break;              /* CLOCK_REALTIME_COARSE */
    case 6: host = CLOCK_MONOTONIC; break;             /* CLOCK_MONOTONIC_COARSE */
    case 7: host = CLOCK_MONOTONIC; break;             /* CLOCK_BOOTTIME */
    case 8: host = CLOCK_REALTIME; break;              /* CLOCK_REALTIME_ALARM */
    case 9: host = CLOCK_MONOTONIC; break;             /* CLOCK_BOOTTIME_ALARM */
    default: host = clk; break;                        /* already a Darwin id */
    }
    return shim_real_clock_gettime(host, ts);
}

int32_t __gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    return (int32_t)gettimeofday(tv, NULL);
}

/* ------------------------------------------------------------------ process */

int __android_fork(void)
{
    shim_missing("__android_fork");
    errno = ENOSYS;
    return -1;
}

int __android_set_sched_policy(int tid, int policy, int nice)
{
    (void)tid;
    (void)policy;
    (void)nice;
    return 0;
}

/* __android_log_assert is one of the guest's variadic entry points, so it is defined with
 * them further down. */

/* zstd calls these to report progress when a build enables tracing; the game builds with
 * it off, so the hooks do nothing, exactly as they do on the device. */
void ZSTD_trace_decompress_begin(void *ctx)
{
    (void)ctx;
}

void ZSTD_trace_decompress_end(void *ctx, size_t ret)
{
    (void)ctx;
    (void)ret;
}

/* ------------------------------------------------------------------ fd_set

 * bionic's FD_SET/FD_ISSET are macros that call these, and a game compiled with
 * _FORTIFY_SOURCE keeps calling them. Each takes the descriptor and the set. */
void __FD_SET_chk(int fd, void *setp)
{
    unsigned char *set = (unsigned char *)setp;
    if (fd < 0 || fd >= 1024) { errno = EBADF; return; }
    set[fd >> 3] |= (unsigned char)(1u << (fd & 7));
}

int __FD_ISSET_chk(int fd, const void *setp)
{
    const unsigned char *set = (const unsigned char *)setp;
    if (fd < 0 || fd >= 1024) { errno = EBADF; return 0; }
    return (set[fd >> 3] >> (fd & 7)) & 1;
}

void __FD_CLR_chk(int fd, void *setp)
{
    unsigned char *set = (unsigned char *)setp;
    if (fd < 0 || fd >= 1024) { errno = EBADF; return; }
    set[fd >> 3] &= (unsigned char)~(1u << (fd & 7));
}

/* ------------------------------------------------------------------ C++ runtime

 * The guard a static initialiser is protected by. The guest's own libc++ calls the ABI
 * for these and never touches the memory itself, so the layout is ours to choose -- but
 * it is not ours to *grow*: the guest allocated the object with Android's sizes, and
 * Darwin's pthread_mutex_t is bigger than Android's. So the state and the mutex live in
 * a table here, keyed by the guard's address, and the guest's own bytes are left alone. */
typedef struct {
    const void *key;
    pthread_mutex_t mutex;
    int state;                    /* 0 new, 1 a thread is initialising, 2 done */
    pthread_t owner;              /* which thread claimed it */
} guard_slot;

#define GUARD_SLOTS 1024
static guard_slot g_guards[GUARD_SLOTS];
static pthread_mutex_t g_guard_table_lock = PTHREAD_MUTEX_INITIALIZER;

static guard_slot *guard_for(const void *gv)
{
    size_t i = (size_t)(((uintptr_t)gv >> 4) % GUARD_SLOTS);
    for (size_t n = 0; n < GUARD_SLOTS; n++) {
        if (g_guards[i].key == gv || g_guards[i].key == NULL) {
            if (g_guards[i].key == NULL) {
                pthread_mutexattr_t a;
                pthread_mutexattr_init(&a);
                pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
                pthread_mutex_init(&g_guards[i].mutex, &a);
                pthread_mutexattr_destroy(&a);
                g_guards[i].key = gv;
            }
            return &g_guards[i];
        }
        i = (i + 1) % GUARD_SLOTS;
    }
    return NULL;
}

int __cxa_guard_acquire(void *gv)
{
    if (getenv("HUSK_GUARD_TRACE"))
        husk_shim_logf("shim: guard_acquire %p\n", gv);
    pthread_mutex_lock(&g_guard_table_lock);
    guard_slot *g = guard_for(gv);
    if (!g) { pthread_mutex_unlock(&g_guard_table_lock); return 0; }
    for (;;) {
        if (g->state == 2) {                     /* already initialised */
            pthread_mutex_unlock(&g_guard_table_lock);
            return 0;
        }
        if (g->state == 0) {                     /* this thread owns the guard */
            g->state = 1;
            g->owner = pthread_self();
            pthread_mutex_lock(&g->mutex);
            pthread_mutex_unlock(&g_guard_table_lock);
            return 1;
        }
        if (pthread_equal(g->owner, pthread_self())) {
            /* The same thread is inside the initialiser again -- a static initialiser
             * that reaches an object it is already constructing. Waiting for itself
             * would never end, and the mutex is recursive anyway. */
            pthread_mutex_unlock(&g_guard_table_lock);
            return 1;
        }
        /* Another thread is inside the initialiser. Wait for it to finish, holding the
         * table lock so that nobody else starts the same dance. */
        pthread_mutex_lock(&g->mutex);
        pthread_mutex_unlock(&g->mutex);
        pthread_mutex_unlock(&g_guard_table_lock);
        sched_yield();
        pthread_mutex_lock(&g_guard_table_lock);
    }
}

int __cxa_guard_release(void *gv)
{
    pthread_mutex_lock(&g_guard_table_lock);
    guard_slot *g = guard_for(gv);
    if (g) {
        pthread_mutex_unlock(&g->mutex);
        g->state = 2;
        memset(&g->owner, 0, sizeof g->owner);
    }
    pthread_mutex_unlock(&g_guard_table_lock);
    return 0;
}

void __cxa_guard_abort(void *gv)
{
    pthread_mutex_lock(&g_guard_table_lock);
    guard_slot *g = guard_for(gv);
    if (g) {
        pthread_mutex_unlock(&g->mutex);
        g->state = 1;                 /* the initialiser threw: another one may run */
    }
    pthread_mutex_unlock(&g_guard_table_lock);
}

/* ------------------------------------------------------------------ __cxa_thread_atexit
 *
 * The C++ runtime's way of saying "run this when the thread that owns it exits".
 *
 * Apple's libc++abi does not export it. It is in the macOS SDK's libc.tbd --
 * ___cxa_thread_atexit, for x86_64-macos, arm64e-macos and the Catalyst targets --
 * and in no .tbd in the iPhoneOS SDK at all, nor in the simulator's. So the macOS
 * build of this file links, because the symbol comes from libSystem, and the iOS
 * build does not:
 *
 *     Undefined symbols for architecture arm64:
 *       "___cxa_thread_atexit", referenced from:
 *           ___cxa_thread_atexit_impl in bionic-shim-*.o
 *
 * which is the forwarder at the end of this section reaching for a name the device
 * does not have.
 *
 * The guest needs it for a second reason. Its own libc++_shared.dylib -- the C++
 * runtime the game carries with it, converted into a Mach-O dylib -- imports
 * __cxa_thread_atexit_impl, which is the name Android's libc++abi exports, and the
 * shim is what that import binds to. The guest's thread_locals have real
 * destructors, so those calls are real: they have to run at the end of the thread
 * that made them, in the reverse of the order they were registered in, and each of
 * them once.
 *
 * So the implementation is here, for the platforms that have none of their own. It
 * is deliberately absent on macOS: the platform's implementation is the one that
 * should be in charge there, and two implementations would each keep their own list
 * of registrations, so a destructor registered with one would be run by the other
 * and never by the one that was asked.
 *
 * TARGET_OS_IPHONE is the test, and it is the right one because it names the
 * platform that lacks the symbol rather than the one that has it: it is set for
 * iOS proper and for the iOS simulator, and neither of those SDKs has a
 * __cxa_thread_atexit. Mac Catalyst is excluded, because Catalyst is the macOS
 * library under another name and has the symbol -- defining it there would give
 * the Catalyst build two of them, exactly as on macOS.
 */
#ifndef TARGET_OS_MACCATALYST
#define TARGET_OS_MACCATALYST 0
#endif
#if TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
#define HUSK_HAS_CXA_THREAD_ATEXIT 1
#else
#define HUSK_HAS_CXA_THREAD_ATEXIT 0
#endif

/* Both platforms have the name, but only one of them has the symbol: this is the
 * declaration the forwarder below calls through, and on macOS it names libSystem's
 * implementation while on iOS it names the one below. */
extern int __cxa_thread_atexit(void (*)(void *), void *, void *);

#if HUSK_HAS_CXA_THREAD_ATEXIT

/* Darwin's own hook for "run this when the thread that owns it exits".
 *
 * The compiler emits a call to it for every C++ thread_local that has a
 * destructor. Its Mach-O name is __tlv_atexit, which is the C name _tlv_atexit
 * with the usual leading underscore, and it takes the same three arguments the
 * ABI's __cxa_thread_atexit takes: the destructor, the object it destroys, and
 * the handle of the shared library the object belongs to. libSystem exports it
 * on every Apple platform, iOS included, and the platform's own
 * __cxa_thread_atexit is built on top of it -- the two share one list and one
 * order, which is what makes it the right thing to register with.
 *
 * It is also the only hook that runs the destructors at the right moment, and
 * the moment matters. A thread's TLS is reset to its initial values when the
 * thread exits, and the reset happens *before* the pthread key destructors run:
 *
 *     [thread] alive: g_tls = 0x3333  g_plain = 0x4444
 *     [cxa_thread_atexit dtor] g_tls = 0x3333  g_plain = 0x4444
 *     [pthread key destructor]  g_tls = 0x2222  g_plain = 0x1111
 *
 * A thread_local's destructor is handed the address of the object it destroys,
 * and that object lives in the thread's TLS. A destructor that runs after the
 * reset is therefore working on an object that has already been reset, which is
 * not what the C++ runtime means and not what the guest's destructors expect.
 * So the registrations go to libSystem's hook, which runs them in the phase
 * where the thread's TLS is still whole.
 *
 * The dso_handle is passed through unchanged, which is what the platform's own
 * __cxa_thread_atexit does with it and what the macOS build of this file does
 * by forwarding to libSystem. It names the shared library the object belongs
 * to, so that a library unloaded before the thread exits does not leave a
 * destructor pointing into an image that is gone; the guest's libraries are
 * loaded for the life of the process and are never unloaded, so there is
 * nothing for it to protect against here. */
extern void _tlv_atexit(void (*)(void *), void *, void *);

int __cxa_thread_atexit(void (*dtor)(void *), void *obj, void *dso_handle)
{
    if (!dtor) return 0;
    _tlv_atexit(dtor, obj, dso_handle);
    return 0;
}

#endif /* HUSK_HAS_CXA_THREAD_ATEXIT */

/* The name the guest's runtime imports. Android's libc++abi exports the aborting
 * variant under this name and the real one as __cxa_thread_atexit, and the shim is
 * what that import binds to. On macOS the call goes to the platform's own
 * __cxa_thread_atexit; on iOS, to the implementation above. */
int __cxa_thread_atexit_impl(void (*dtor)(void *), void *obj, void *dso_handle)
{
    if (!dtor) return 0;
    return __cxa_thread_atexit(dtor, obj, dso_handle);
}

/* ------------------------------------------------------------------ fortify and atexit

 * bionic's own __chk_fail aborts with a message; this is the same idea with the name iOS
 * will already have in its logs. */
void __chk_fail(void)
{
    husk_shim_logf("bionic-shim: buffer overflow detected\n");
    abort();
}

/* Android's _FORTIFY_SOURCE rewrites the string and array primitives into calls to these
 * checked versions. Each is the unchecked one with the precondition the caller already
 * promised, so the behaviour is the same as the inline it replaced. */
size_t __strlen_chk(const char *s, size_t n)
{
    size_t len = 0;
    while (len < n && s[len]) len++;
    return len;
}

void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dstlen)
{
    if (n > dstlen) __chk_fail();
    return memcpy(dst, src, n);
}

void *__memmove_chk(void *dst, const void *src, size_t n, size_t dstlen)
{
    if (n > dstlen) __chk_fail();
    return memmove(dst, src, n);
}

void *__memset_chk(void *dst, int c, size_t n, size_t dstlen)
{
    if (n > dstlen) __chk_fail();
    return memset(dst, c, n);
}

/* bionic's version records a handler to run before the next exec; nobody re-execs inside
 * the game, so the call is a no-op rather than a fork-registration we cannot honour. */
int __register_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void))
{
    (void)prepare; (void)parent; (void)child;
    return 0;
}

/* The condition-variable operations are not here: they travel to the translation layer
 * with the rest of the thread primitives, because a Darwin pthread_cond_t written into a
 * guest that sized its own for Android's is not a condition variable. See the forwarding
 * section above. */

/* ------------------------------------------------------------------ the game's files
 *
 * The guest's paths are Android's (/sdcard/Android/obb/...), the files are the host's, and
 * the mapping between them is the host's business: it knows where the APK was unpacked and
 * where the OBB tar lives. So the host registers its implementations once, here, and these
 * wrappers call them; with nothing registered they are the host's own libc calls, which is
 * what a program that only uses its own files wants.
 *
 * The implementations are the translation layer's own -- the same ones the ELF loader
 * binds an import to when it maps the guest (husk-tl-bionic-io.c's b_open, b_stat, ...), so
 * the game sees the same filesystem either way.
 */
/* ------------------------------------------- Android's libc, where Darwin's differs */

/* Not every name a Bionic guest imports exists on Darwin with the same meaning, and quite a
 * few exist with the same name and a different one. Three kinds of difference matter:
 *
 *   - the struct layouts differ -- pthread_attr_t, pthread_cond_t, sem_t, struct statfs,
 *     struct passwd -- so a Darwin result read by a guest expecting Android's is not the
 *     same fields, and a guest that sizes its own object from ours writes past it;
 *   - the semantics differ -- dirname() rewrites its argument on Darwin and returns a
 *     pointer to a static buffer on Android, and syscall() numbers are Darwin's;
 *   - the call cannot mean anything useful here -- mknod() and mkfifo() would create
 *     device nodes and FIFOs in the host's filesystem, which is neither wanted nor allowed.
 *
 * Every one of them already has an implementation: the translation layer is what runs
 * Android's libc for the translating route, so it knows these layouts and this behaviour,
 * and it is what the ELF loader binds an import to there. Keeping a second, hand-written
 * copy here would mean two places to be right. So this file asks for the implementation by
 * name and calls it, and a name with no implementation answers "it did not happen" -- which
 * is the honest answer for the third group and only ever a gap for the rest. */
typedef void *(*husk_host_lookup)(const char *name);
static husk_host_lookup host_lookup;
static void guest_va_resolve(void);

void husk_shim_set_host_lookup(void *fn)
{
    host_lookup = (husk_host_lookup)fn;
    guest_va_resolve();
}

static void *host_impl(const char *name) { return host_lookup ? host_lookup(name) : NULL; }

/* The generated half of the shim resolves its forwarders through these two. */
void *husk_shim_host_impl(const char *name) { return host_impl(name); }
void husk_shim_set_gles_resolver(void *fn);

/* A constant the guest takes the address of -- an OpenSLES interface id, say -- is storage
 * here whose value the host sets, because the value is an interface the host built and a
 * number in this file could not be one. */
void husk_shim_set_data(const char *name, const void *src)
{
    void *p = dlsym(RTLD_DEFAULT, name);
    if (p) *(unsigned long *)p = *(const unsigned long *)src;
    else husk_shim_logf("bionic-shim: no storage for the constant %s\n", name);
}

/* Forward one name. `absent` is what to answer if the host has no implementation for it.
 * Each wrapper caches the pointer it was handed: the host's table is registered once, before
 * any guest code runs, so the answer cannot change afterwards. */
#define FORWARD_ANDROID(ret, name, decl, args, absent) \
    ret name decl                                      \
    {                                                  \
        static ret (*impl) decl;                       \
        if (!impl) impl = (ret (*) decl)host_impl(#name); \
        return impl ? impl args : absent;              \
    }

/* ------------------------------------------------------------------ the guest's
 * variadic functions
 *
 * The guest is an Android binary, so AAPCS64 applies to it: the first eight integer
 * arguments of a variadic call are in x0-x7, the first eight floating-point ones in
 * v0-v7, and the rest go on the stack. The guest's va_list is a 32-byte structure --
 * the next stack argument, the top of the general register save area, the top of the
 * vector register save area, and the two offsets into them -- and because the type is
 * an array of one it reaches a function as a pointer.
 *
 * Darwin's arm64 ABI is the opposite on both counts: every variadic argument goes on
 * the stack, and the va_list is a single pointer. So a variadic function written in C
 * and compiled for Darwin reads the guest's arguments from the wrong place. The format
 * string arrives -- it is a named argument, and named arguments are in the same
 * registers in both ABIs -- and everything after it is stack junk.
 *
 * That is not a cosmetic problem. The game builds its paths, its sizes and its asset
 * names with snprintf, so a garbled argument is a path that does not exist and a size
 * that is wrong. It is also why the guest's own log lines came out with a host pointer
 * where a number should have been.
 *
 * The translation layer already has the Android-shaped entry points: tl_va_common
 * spills x0-x7 and v0-v7 into a frame and hands them to the implementation, and the
 * tl_va_list it builds is the guest's own structure, byte for byte. So the guest's
 * variadic entry points here are stubs that jump to the translation layer's, leaving
 * the registers and the stack exactly as the guest left them.
 */

/* Where the translation layer's entry point for each name lives, and the name itself for
 * the report below. The stub reads the pointer and jumps; x16 and x17 are scratch in
 * both ABIs, so nothing the guest passed in x0-x7 or v0-v7 is disturbed.
 *
 * The jump is a tail jump, so the guest's stack is the callee's stack: an argument that
 * was on the stack in the guest's call is still there, at the same address. */
#define GUEST_STUB(name)                                                            \
    void *husk_va_impl_##name;                                                     \
    const char *husk_va_name_##name = #name;                                       \
    __asm__(".text\n.p2align 2\n.globl _" #name "\n_" #name ":\n"                  \
            "  adrp x16, _husk_va_impl_" #name "@PAGE\n"                          \
            "  add  x16, x16, _husk_va_impl_" #name "@PAGEOFF\n"                 \
            "  ldr  x17, [x16]\n"                                                  \
            "  cbz  x17, 1f\n"                                                     \
            "  br   x17\n"                                                         \
            "1:  adrp x16, _husk_va_name_" #name "@PAGE\n"                        \
            "  add  x16, x16, _husk_va_name_" #name "@PAGEOFF\n"                 \
            "  mov  x0, x16\n"                                                     \
            "  b    _husk_va_missing\n")

/* The guest called one of these before the host registered its table. The name is in x0. */
void husk_va_missing(const char *name)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static const char *reported;
    pthread_mutex_lock(&lock);
    if (name != reported) {
        reported = name;
        husk_shim_logf("bionic-shim: %s was called before the translation layer was ready\n",
                       name);
    }
    pthread_mutex_unlock(&lock);
}

/* The guest's variadic entry points. */
GUEST_STUB(printf);
GUEST_STUB(fprintf);
GUEST_STUB(snprintf);
GUEST_STUB(sprintf);
GUEST_STUB(asprintf);
GUEST_STUB(swprintf);
/* The scanning half of the same family. A guest's sscanf is a variadic call
 * the Android way: the arguments are in registers. libSystem's sscanf reads
 * them the Darwin way, from the stack, and parses a format string that is not
 * one -- which is a crash, not a wrong answer. */
GUEST_STUB(sscanf);
GUEST_STUB(fscanf);
GUEST_STUB(swscanf);
GUEST_STUB(vsscanf);
GUEST_STUB(syslog);
GUEST_STUB(__android_log_print);
GUEST_STUB(__android_log_assert);

/* Bionic's fcntl, open and openat take an argument after the ones they name, and it is a
 * variadic tail: AAPCS64 keeps the first eight arguments in registers whatever their
 * kind, so the guest's argument is in x2 or x3. A Darwin variadic function would look
 * for it on the stack, where the guest did not put it, so these are tail jumps to the
 * translation layer's, which take the argument as an ordinary one. */
GUEST_STUB(fcntl);
GUEST_STUB(open);
GUEST_STUB(openat);

/* The v-forms: the guest's va_list travels as a pointer, so these are ordinary calls.
 * Darwin's va_list is a single pointer and Android's is a 32-byte structure that reaches
 * a function as a pointer, so what arrives here is the guest's own address either way,
 * and the translation layer reads it with Android's layout. */
FORWARD_ANDROID(int, vprintf,      (const char *fmt, va_list ap),
                (fmt, ap), -1)
FORWARD_ANDROID(int, vfprintf,     (FILE *f, const char *fmt, va_list ap),
                (f, fmt, ap), -1)
FORWARD_ANDROID(int, vsnprintf,    (char *s, size_t n, const char *fmt, va_list ap),
                (s, n, fmt, ap), -1)
FORWARD_ANDROID(int, vsprintf,     (char *s, const char *fmt, va_list ap),
                (s, fmt, ap), -1)
FORWARD_ANDROID(int, vasprintf,    (char **p, const char *fmt, va_list ap),
                (p, fmt, ap), -1)
FORWARD_ANDROID(int, vswprintf,    (wchar_t *s, size_t n, const wchar_t *fmt, va_list ap),
                (s, n, fmt, ap), -1)
FORWARD_ANDROID(int, __vsnprintf_chk,
                (char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list ap),
                (s, maxlen, flag, slen, fmt, ap), -1)
FORWARD_ANDROID(int, __vsprintf_chk,
                (char *s, int flag, size_t slen, const char *fmt, va_list ap),
                (s, flag, slen, fmt, ap), -1)
FORWARD_ANDROID(int, vsyslog,      (int pri, const char *fmt, va_list ap),
                (pri, fmt, ap), 0)
FORWARD_ANDROID(int, __android_log_vprint,
                (int prio, const char *tag, const char *fmt, va_list ap),
                (prio, tag, fmt, ap), 0)

/* The translation layer's entry points, once the host has registered its table. */
static void guest_va_resolve(void)
{
    husk_va_impl_printf               = host_impl("printf");
    husk_va_impl_fprintf              = host_impl("fprintf");
    husk_va_impl_snprintf             = host_impl("snprintf");
    husk_va_impl_sprintf              = host_impl("sprintf");
    husk_va_impl_asprintf             = host_impl("asprintf");
    husk_va_impl_swprintf             = host_impl("swprintf");
    husk_va_impl_sscanf               = host_impl("sscanf");
    husk_va_impl_fscanf               = host_impl("fscanf");
    husk_va_impl_swscanf              = host_impl("swscanf");
    husk_va_impl_vsscanf              = host_impl("vsscanf");
    husk_va_impl_syslog               = host_impl("syslog");
    husk_va_impl___android_log_print  = host_impl("__android_log_print");
    husk_va_impl___android_log_assert = host_impl("__android_log_assert");
    husk_va_impl_fcntl                = host_impl("fcntl");
    husk_va_impl_open                 = host_impl("open");
    husk_va_impl_openat               = host_impl("openat");
}

/* -- paths. dirname() is the reason this section exists: on Darwin it is a BSD extension
 * that writes the directory part back into the caller's buffer, so a guest that passes a
 * string it still needs afterwards would find it rewritten under it. */
FORWARD_ANDROID(char *,        dirname,        (char *p),               (p),               NULL)
FORWARD_ANDROID(char *,        basename,       (const char *p),         (p),               NULL)
FORWARD_ANDROID(char *,        realpath,       (const char *p, char *o), (p, o),          NULL)

/* -- files whose descriptor belongs to the host's virtual filesystem. These must travel
 * together: open() hands out a descriptor in that space, and every later call on it means
 * the same descriptor. The ones that carry a mode or a third argument are written out
 * rather than forwarded by the macro, which cannot pass an argument it does not name. */
static int  (*android_close)(int);
static int  (*android_dup)(int);
static int  (*android_dup2)(int, int);
static ssize_t (*android_read)(int, void *, size_t);
static ssize_t (*android_write)(int, const void *, size_t);
static ssize_t (*android_pread)(int, void *, size_t, off_t);
static ssize_t (*android_pwrite)(int, const void *, size_t, off_t);
static off_t (*android_lseek)(int, off_t, int);
static int  (*android_ftruncate)(int, off_t);
static int  (*android_fsync)(int);
static int  (*android_fcntl)(int, int, void *);
static int  (*android_openat)(int, const char *, int, int);

static void fs_ready(void)
{
    if (android_close) return;
    android_close    = (int (*)(int))host_impl("close");
    android_dup      = (int (*)(int))host_impl("dup");
    android_dup2     = (int (*)(int, int))host_impl("dup2");
    android_read     = (ssize_t (*)(int, void *, size_t))host_impl("read");
    android_write    = (ssize_t (*)(int, const void *, size_t))host_impl("write");
    android_pread    = (ssize_t (*)(int, void *, size_t, off_t))host_impl("pread");
    android_pwrite   = (ssize_t (*)(int, const void *, size_t, off_t))host_impl("pwrite");
    android_lseek    = (off_t (*)(int, off_t, int))host_impl("lseek");
    android_ftruncate = (int (*)(int, off_t))host_impl("ftruncate");
    android_fsync    = (int (*)(int))host_impl("fsync");
    android_fcntl    = (int (*)(int, int, void *))host_impl("fcntl");
    android_openat   = (int (*)(int, const char *, int, int))host_impl("openat");
}

int close(int fd) { fs_ready(); return android_close ? android_close(fd) : -1; }
int dup(int fd) { fs_ready(); return android_dup ? android_dup(fd) : -1; }
int dup2(int fd, int g) { fs_ready(); return android_dup2 ? android_dup2(fd, g) : -1; }

ssize_t read(int fd, void *b, size_t n)
{ fs_ready(); return android_read ? android_read(fd, b, n) : -1; }

ssize_t write(int fd, const void *b, size_t n)
{ fs_ready(); return android_write ? android_write(fd, b, n) : -1; }

ssize_t pread(int fd, void *b, size_t n, off_t o)
{ fs_ready(); return android_pread ? android_pread(fd, b, n, o) : -1; }

ssize_t pwrite(int fd, const void *b, size_t n, off_t o)
{ fs_ready(); return android_pwrite ? android_pwrite(fd, b, n, o) : -1; }

off_t lseek(int fd, off_t o, int w)
{ fs_ready(); return android_lseek ? android_lseek(fd, o, w) : (off_t)-1; }

int ftruncate(int fd, off_t n) { fs_ready(); return android_ftruncate ? android_ftruncate(fd, n) : -1; }
int fsync(int fd) { fs_ready(); return android_fsync ? android_fsync(fd) : -1; }


/* -- the filesystem the guest cannot get from the host: its own view of the data
 * directory, packed assets and the OBB tar. stat() is already forwarded by the file hooks
 * above; statfs() has no equivalent layout and goes the same way. */
FORWARD_ANDROID(int,           statfs,         (const char *p, struct statfs *s), (p, s), -1)
FORWARD_ANDROID(int,           fstatfs,        (int fd, struct statfs *s), (fd, s),  -1)
FORWARD_ANDROID(int,           rmdir,          (const char *p),         (p),      -1)
FORWARD_ANDROID(int,           rename,         (const char *a, const char *b), (a, b), -1)
FORWARD_ANDROID(int,           chmod,          (const char *p, mode_t m), (p, m),  -1)
FORWARD_ANDROID(int,           chown,          (const char *p, uid_t u, gid_t g), (p, u, g), -1)
FORWARD_ANDROID(int,           chdir,          (const char *p),         (p),      -1)
FORWARD_ANDROID(char *,        getcwd,         (char *b, size_t n),      (b, n),   NULL)
FORWARD_ANDROID(int,           symlink,        (const char *a, const char *b), (a, b), -1)
FORWARD_ANDROID(int,           link,           (const char *a, const char *b), (a, b), -1)
FORWARD_ANDROID(ssize_t,       readlink,       (const char *p, char *b, size_t n), (p, b, n), -1)

/* -- the account database. Android resolves these against its own passwd/group, Darwin
 * against the host's, and the two structures are laid out differently enough that a guest
 * reading pw_dir out of a Darwin struct reads a padding field. There is no guest account to
 * look up either way; the host's answers are forwarded so the guest sees the shape it
 * compiled against, and a miss is a miss on both sides. */
FORWARD_ANDROID(struct passwd *, getpwnam,     (const char *n),         (n),       NULL)
FORWARD_ANDROID(struct passwd *, getpwuid,     (uid_t u),               (u),       NULL)
FORWARD_ANDROID(struct group *, getgrnam,      (const char *n),         (n),       NULL)
FORWARD_ANDROID(struct group *, getgrgid,      (gid_t g),               (g),       NULL)

/* getpwuid_r takes its arguments in a different order on the two platforms -- Darwin puts the
 * buffer and its size before the result -- so there is no signature that would forward, and
 * the name cannot even be declared here. What the guest needs is a symbol of that name
 * bound to something: this is exported from an assembly stub at the bottom of the file,
 * which jumps to husk_getpwuid_r and carries the guest's four arguments. */
int husk_getpwuid_r(uid_t uid, void *pw, void *result, void *len)
{
    (void)uid; (void)pw; (void)len;
    if (result) *(void **)result = NULL;
    return ENOENT;
}

/* The export, under the name the guest binds. Darwin's prototype has five arguments in a
 * different order, so the symbol has to exist without a declaration behind it; a branch is
 * enough, and it carries the guest's own four arguments through untouched. */
__asm__(".globl _getpwuid_r\n"
        ".p2align 2\n"
        "_getpwuid_r:\n"
        "        b       _husk_getpwuid_r\n"
        ".section __TEXT,__const\n");

FORWARD_ANDROID(struct hostent *, gethostbyname, (const char *n),        (n),       NULL)
FORWARD_ANDROID(int,           getaddrinfo,    (const char *n, const char *s, const void *h, void *r), (n, s, h, r), -1)
FORWARD_ANDROID(void,          freeaddrinfo,   (void *r),               (r),       (void)0)
FORWARD_ANDROID(const char *,  gai_strerror,   (int e),                 (e),       "unknown")

/* -- device nodes and FIFOs. There is no such thing to create here, and creating one in
 * the host's filesystem would be a side effect of loading a game, so these fail the way a
 * host without permission would. */
FORWARD_ANDROID(int,           mknod,          (const char *p, mode_t m, dev_t d), (p, m, d), -1)
FORWARD_ANDROID(int,           mkfifo,         (const char *p, mode_t m), (p, m),  -1)

/* -- raw syscalls. The guest was compiled against Bionic's numbering, which is not
 * Darwin's, so forwarding the number would run a different call than the guest asked for.
 *
 * The guest uses three of them (read out of its own code):
 *
 *   122 uname     - the Linux struct is six 65-byte fields; Darwin's is five 256-byte
 *                   ones, so forwarding the pointer would write 1280 bytes into the
 *                   guest's 390-byte buffer and read the fields wrongly. The answer is
 *                   copied field by field into the guest's own shape.
 *   178 gettid    - Darwin has no gettid; what the guest wants is its thread's id, and
 *                   pthread_threadid_np is exactly that.
 *   278 getrandom  - Darwin's arc4random_buf fills any length (getentropy stops at 256).
 *
 * Anything else is still a failure, and it is reported once so it is visible: a guest
 * that depends on a syscall that is not there is a guest whose behaviour is missing. */
struct linux_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

int syscall(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    int result = -1;
    switch (n) {
        case 122: {   /* uname */
            struct linux_utsname *u = va_arg(ap, struct linux_utsname *);
            struct utsname host;
            if (u && uname(&host) == 0) {
                memset(u, 0, sizeof(*u));
                strlcpy(u->sysname, host.sysname, sizeof(u->sysname));
                strlcpy(u->nodename, host.nodename, sizeof(u->nodename));
                strlcpy(u->release, host.release, sizeof(u->release));
                strlcpy(u->version, host.version, sizeof(u->version));
                strlcpy(u->machine, host.machine, sizeof(u->machine));
                result = 0;
            }
            break;
        }
        case 178: {   /* gettid */
            uint64_t tid = 0;
            if (pthread_threadid_np(NULL, &tid) == 0) result = (int)tid;
            break;
        }
        case 278: {   /* getrandom */
            void *buf = va_arg(ap, void *);
            size_t len = va_arg(ap, size_t);
            if (buf && len) { arc4random_buf(buf, len); result = (int)len; }
            break;
        }
        default:
            shim_missing("syscall");
            errno = ENOSYS;
            result = -1;
            break;
    }
    va_end(ap);
    return result;
}

/* -- threads. The attribute, condition-variable and semaphore objects are laid out by the
 * C library that owns them, and a guest that keeps one in its own stack slot sized for
 * Android's would have a Darwin object written over it. These travel to the implementation
 * that knows Android's layout, which also normalises what it hands to the host. The mutex
 * wrappers further up do the same job for the reason given there. */
/* The attribute object is the one pthread structure whose size differs enough to matter:
 * bionic's pthread_attr_t is 56 bytes (flags, stack_base, stack_size, guard_size, policy,
 * priority, 16 reserved), Darwin's is 64 (a signature word and an opaque body). A guest
 * keeps one in its own stack slot, so letting Darwin's pthread_attr_init write through that
 * pointer puts eight bytes past the slot -- into whatever the guest placed next -- and the
 * bionic fields the guest set are gone. These functions therefore operate on the bionic
 * layout only. pthread_create() below turns that into a Darwin attribute of its own. */
struct bionic_pthread_attr {
    uint32_t flags;
    void *stack_base;
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
    char __reserved[16];
};
typedef char bionic_pthread_attr_size_check[
    sizeof(struct bionic_pthread_attr) == 56 ? 1 : -1];

#define BIONIC_PTHREAD_ATTR_FLAG_DETACHED   0x1u
#define BIONIC_PTHREAD_ATTR_FLAG_USER_STACK 0x2u
#define BIONIC_PTHREAD_STACK_MIN            16384u
#define BIONIC_PTHREAD_STACK_DEFAULT        (8u * 1024u * 1024u)
#define BIONIC_PTHREAD_CREATE_JOINABLE      0
#define BIONIC_PTHREAD_CREATE_DETACHED      1

static struct bionic_pthread_attr *bionic_attr(pthread_attr_t *a)
{
    return (struct bionic_pthread_attr *)(void *)a;
}

static const struct bionic_pthread_attr *bionic_attr_const(const pthread_attr_t *a)
{
    return (const struct bionic_pthread_attr *)(const void *)a;
}

int pthread_attr_init(pthread_attr_t *a)
{
    if (!a) return EINVAL;
    memset(a, 0, sizeof(struct bionic_pthread_attr));
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }

int pthread_attr_setdetachstate(pthread_attr_t *a, int d)
{
    if (!a || (d != BIONIC_PTHREAD_CREATE_DETACHED &&
               d != BIONIC_PTHREAD_CREATE_JOINABLE)) return EINVAL;
    if (d == BIONIC_PTHREAD_CREATE_DETACHED)
        bionic_attr(a)->flags |= BIONIC_PTHREAD_ATTR_FLAG_DETACHED;
    else
        bionic_attr(a)->flags &= ~BIONIC_PTHREAD_ATTR_FLAG_DETACHED;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *a, int *d)
{
    if (!a || !d) return EINVAL;
    *d = (bionic_attr_const(a)->flags & BIONIC_PTHREAD_ATTR_FLAG_DETACHED)
             ? BIONIC_PTHREAD_CREATE_DETACHED : BIONIC_PTHREAD_CREATE_JOINABLE;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *a, size_t s)
{
    if (!a || s < BIONIC_PTHREAD_STACK_MIN) return EINVAL;
    bionic_attr(a)->stack_size = s;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *s)
{
    if (!a || !s) return EINVAL;
    *s = bionic_attr_const(a)->stack_size
             ? bionic_attr_const(a)->stack_size : BIONIC_PTHREAD_STACK_DEFAULT;
    return 0;
}

int pthread_attr_setstack(pthread_attr_t *a, void *p, size_t s)
{
    if (!a || s < BIONIC_PTHREAD_STACK_MIN) return EINVAL;
    bionic_attr(a)->stack_base = p;
    bionic_attr(a)->stack_size = s;
    bionic_attr(a)->flags |= BIONIC_PTHREAD_ATTR_FLAG_USER_STACK;
    return 0;
}

int pthread_attr_getstack(const pthread_attr_t *a, void **p, size_t *s)
{
    if (!a || !p || !s) return EINVAL;
    *p = bionic_attr_const(a)->stack_base;
    *s = bionic_attr_const(a)->stack_size;
    return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *a, size_t s)
{
    if (!a) return EINVAL;
    bionic_attr(a)->guard_size = s;
    return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *s)
{
    if (!a || !s) return EINVAL;
    *s = bionic_attr_const(a)->guard_size;
    return 0;
}

int pthread_attr_setschedparam(pthread_attr_t *a, const struct sched_param *p)
{
    if (!a || !p) return EINVAL;
    bionic_attr(a)->sched_priority = p->sched_priority;
    return 0;
}

int pthread_attr_getschedparam(const pthread_attr_t *a, struct sched_param *p)
{
    if (!a || !p) return EINVAL;
    p->sched_priority = bionic_attr_const(a)->sched_priority;
    return 0;
}

int pthread_attr_setschedpolicy(pthread_attr_t *a, int p)
{
    if (!a) return EINVAL;
    bionic_attr(a)->sched_policy = p;
    return 0;
}

int pthread_attr_getschedpolicy(const pthread_attr_t *a, int *p)
{
    if (!a || !p) return EINVAL;
    *p = bionic_attr_const(a)->sched_policy;
    return 0;
}
FORWARD_ANDROID(int, pthread_cond_init, (pthread_cond_t *c, const pthread_condattr_t *a), (c, a), -1)
FORWARD_ANDROID(int, pthread_cond_destroy, (pthread_cond_t *c), (c), 0)
FORWARD_ANDROID(int, pthread_cond_wait, (pthread_cond_t *c, pthread_mutex_t *m), (c, m), -1)
FORWARD_ANDROID(int, pthread_cond_timedwait, (pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t), (c, m, t), -1)
FORWARD_ANDROID(int, pthread_cond_signal, (pthread_cond_t *c), (c), -1)
FORWARD_ANDROID(int, pthread_cond_broadcast, (pthread_cond_t *c), (c), -1)
FORWARD_ANDROID(int, pthread_condattr_init, (pthread_condattr_t *a), (a), -1)
FORWARD_ANDROID(int, pthread_condattr_destroy, (pthread_condattr_t *a), (a), 0)
FORWARD_ANDROID(int, pthread_condattr_setclock, (pthread_condattr_t *a, clockid_t c), (a, c), -1)
FORWARD_ANDROID(int, pthread_condattr_setpshared, (pthread_condattr_t *a, int s), (a, s), -1)
FORWARD_ANDROID(int, pthread_rwlock_init, (pthread_rwlock_t *l, const pthread_rwlockattr_t *a), (l, a), -1)
FORWARD_ANDROID(int, pthread_rwlock_destroy, (pthread_rwlock_t *l), (l), 0)
FORWARD_ANDROID(int, pthread_rwlock_rdlock, (pthread_rwlock_t *l), (l), -1)
FORWARD_ANDROID(int, pthread_rwlock_wrlock, (pthread_rwlock_t *l), (l), -1)
FORWARD_ANDROID(int, pthread_rwlock_unlock, (pthread_rwlock_t *l), (l), -1)
FORWARD_ANDROID(int, pthread_rwlock_tryrdlock, (pthread_rwlock_t *l), (l), -1)
FORWARD_ANDROID(int, pthread_rwlock_trywrlock, (pthread_rwlock_t *l), (l), -1)
FORWARD_ANDROID(int, sem_init, (sem_t *s, int p, unsigned v), (s, p, v), -1)
FORWARD_ANDROID(int, sem_destroy, (sem_t *s), (s), -1)
FORWARD_ANDROID(int, sem_wait, (sem_t *s), (s), -1)
FORWARD_ANDROID(int, sem_trywait, (sem_t *s), (s), -1)
FORWARD_ANDROID(int, sem_timedwait, (sem_t *s, const struct timespec *t), (s, t), -1)
FORWARD_ANDROID(int, sem_post, (sem_t *s), (s), -1)
FORWARD_ANDROID(int, sem_getvalue, (sem_t *s, int *v), (s, v), -1)

/* pthread_once() is here for the same reason as the condition variables, and it costs more
 * when it is wrong: bionic's once-control word is {state, count} and Darwin's is a single
 * long with a different protocol, so a guest's own once object handed to Darwin's pthread_once
 * is a request to wait for an initializer nobody is running. The thread never returns and
 * there is no fault to see -- the stack just stops at pthread_once with nothing above it.
 * A thread, a key and a name all travel the same way, for the same reason. */
/* ------------------------------------------------------------------ pthread objects

 * bionic says a zeroed pthread_mutex_t is a normal mutex and a zeroed pthread_cond_t is a
 * condition variable, because on Android that is what a static one is: the object sits in
 * .bss and nobody initialises it. Darwin disagrees -- its objects carry a signature word, and
 * locking a zeroed one fails with EINVAL. Something has to bridge the two, and every
 * operation on these objects has to agree about how.
 *
 * That "every operation" is the whole point, and getting it wrong is not a shape mismatch
 * but a use of freed memory. These objects are not a struct the guest lays out and the host
 * then reads: the host keeps its own object and puts a header in the guest's, so a condition
 * variable knows which host object belongs to it and can pair it with the right mutex. An
 * earlier version of this file normalised mutexes in place instead, for Darwin's benefit --
 * which wrote a Darwin mutex where the header was, and the first pthread_cond_timedwait
 * through the translation layer passed that to the host as the mutex to wait on. The crash
 * said nothing about mutexes.
 *
 * So there is one implementation of each, the one the translation layer already has, and
 * these wrappers only hand it over.
 */
FORWARD_ANDROID(int, pthread_mutex_init, (pthread_mutex_t *m, const pthread_mutexattr_t *a), (m, a), -1)
FORWARD_ANDROID(int, pthread_mutex_destroy, (pthread_mutex_t *m), (m), 0)
FORWARD_ANDROID(int, pthread_mutex_lock, (pthread_mutex_t *m), (m), -1)
FORWARD_ANDROID(int, pthread_mutex_trylock, (pthread_mutex_t *m), (m), -1)
FORWARD_ANDROID(int, pthread_mutex_unlock, (pthread_mutex_t *m), (m), -1)
FORWARD_ANDROID(int, pthread_mutexattr_init, (pthread_mutexattr_t *a), (a), -1)
FORWARD_ANDROID(int, pthread_mutexattr_destroy, (pthread_mutexattr_t *a), (a), 0)
FORWARD_ANDROID(int, pthread_mutexattr_settype, (pthread_mutexattr_t *a, int k), (a, k), -1)
FORWARD_ANDROID(int, pthread_mutexattr_gettype, (const pthread_mutexattr_t *a, int *k), (a, k), -1)
FORWARD_ANDROID(int, pthread_mutexattr_setpshared, (pthread_mutexattr_t *a, int p), (a, p), -1)
FORWARD_ANDROID(int, pthread_mutexattr_getpshared, (const pthread_mutexattr_t *a, int *p), (a, p), -1)
FORWARD_ANDROID(int, pthread_once, (pthread_once_t *o, void (*fn)(void)), (o, fn), -1)
FORWARD_ANDROID(int, pthread_detach, (pthread_t t), (t), -1)
FORWARD_ANDROID(int, pthread_join, (pthread_t t, void **r), (t, r), -1)
FORWARD_ANDROID(int, pthread_kill, (pthread_t t, int s), (t, s), -1)
FORWARD_ANDROID(void *, pthread_getspecific, (pthread_key_t k), (k), NULL)
FORWARD_ANDROID(int, pthread_setspecific, (pthread_key_t k, const void *v), (k, v), -1)
FORWARD_ANDROID(int, pthread_key_create, (pthread_key_t *k, void (*d)(void *)), (k, d), -1)
FORWARD_ANDROID(int, pthread_key_delete, (pthread_key_t k), (k), -1)
FORWARD_ANDROID(int, pthread_getname_np, (pthread_t t, char *n, size_t l), (t, n, l), -1)
FORWARD_ANDROID(int, pthread_setname_np, (const char *n), (n), -1)
FORWARD_ANDROID(int, pthread_getcpuclockid, (pthread_t t, clockid_t *c), (t, c), -1)
FORWARD_ANDROID(int, pthread_getschedparam, (pthread_t t, int *p, struct sched_param *sp), (t, p, sp), -1)
FORWARD_ANDROID(int, pthread_setschedparam, (pthread_t t, int p, const struct sched_param *sp), (t, p, sp), -1)
FORWARD_ANDROID(int, pthread_sigmask, (int how, const sigset_t *set, sigset_t *old), (how, set, old), -1)
FORWARD_ANDROID(int, pthread_atfork, (void (*prepare)(void), void (*parent)(void), void (*child)(void)), (prepare, parent, child), -1)

/* pthread_create is the one call here that is not a straight forward, because a thread the
 * guest starts has never been near the loader: nothing has put the register that stands for
 * the guest's thread pointer where that thread's code expects to find it, and the first
 * instruction that stores through it writes to address zero. So the guest's start routine is
 * given a trampoline that asks the host to set it up and then calls it. */
typedef void *(*aot_start_fn)(void *);
struct aot_thread_start { aot_start_fn fn; void *arg; };

static void (*host_thread_entry)(void);

void husk_shim_set_thread_entry(void (*fn)(void)) { host_thread_entry = fn; }

static void *aot_thread_trampoline(void *arg)
{
    /* The guest's start routine and its argument are read out of the cell and the cell is
     * released here: it is the only thing allocated on this path, and it does not outlive
     * the call it exists for. */
    struct aot_thread_start *st = (struct aot_thread_start *)arg;
    aot_start_fn fn = st->fn;
    void *a = st->arg;
    free(st);
    if (host_thread_entry) host_thread_entry();
    return fn ? fn(a) : NULL;
}

/* Darwin's own attribute functions, used only on the private host attribute this file
 * builds. They are looked up through the loader's host table, not through the guest's
 * imports, because the guest's pthread_attr_* now mean the bionic layout above. */
static int (*host_attr_init)(pthread_attr_t *);
static int (*host_attr_destroy)(pthread_attr_t *);
static int (*host_attr_setstacksize)(pthread_attr_t *, size_t);
static int (*host_attr_setguardsize)(pthread_attr_t *, size_t);
static int (*host_attr_setdetachstate)(pthread_attr_t *, int);

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    static int (*impl)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    if (!impl)
        impl = (int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *))
                   host_impl("pthread_create");
    if (!impl || !fn) return -1;
    struct aot_thread_start *st = (struct aot_thread_start *)malloc(sizeof *st);
    if (!st) return -1;
    st->fn = fn;
    st->arg = arg;
    if (!host_attr_init) {
        host_attr_init = (int (*)(pthread_attr_t *))host_impl("pthread_attr_init");
        host_attr_destroy = (int (*)(pthread_attr_t *))host_impl("pthread_attr_destroy");
        host_attr_setstacksize = (int (*)(pthread_attr_t *, size_t))
            host_impl("pthread_attr_setstacksize");
        host_attr_setguardsize = (int (*)(pthread_attr_t *, size_t))
            host_impl("pthread_attr_setguardsize");
        host_attr_setdetachstate = (int (*)(pthread_attr_t *, int))
            host_impl("pthread_attr_setdetachstate");
    }
    const struct bionic_pthread_attr *ba =
        (const struct bionic_pthread_attr *)(const void *)attr;
    pthread_attr_t host_attr;
    int have_host_attr = 0;
    if (host_attr_init && host_attr_init(&host_attr) == 0) {
        have_host_attr = 1;
        size_t ss = (ba && ba->stack_size) ? ba->stack_size
                                           : BIONIC_PTHREAD_STACK_DEFAULT;
        if (ss < BIONIC_PTHREAD_STACK_MIN) ss = BIONIC_PTHREAD_STACK_MIN;
        if (host_attr_setstacksize) host_attr_setstacksize(&host_attr, ss);
        if (ba && ba->guard_size && host_attr_setguardsize)
            host_attr_setguardsize(&host_attr, ba->guard_size);
        if (host_attr_setdetachstate) {
            int d = (ba && (ba->flags & BIONIC_PTHREAD_ATTR_FLAG_DETACHED))
                        ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE;
            host_attr_setdetachstate(&host_attr, d);
        }
        /* A guest-provided stack (the USER_STACK flag) is an address in the guest's
         * own space, not a Darwin stack; keep the requested size and let the host
         * allocate the stack instead of forwarding the pointer. */
    }
    int rc = impl(t, have_host_attr ? &host_attr : NULL,
                  aot_thread_trampoline, (void *)st);
    if (have_host_attr && host_attr_destroy) host_attr_destroy(&host_attr);
    if (rc) free(st);
    return rc;
}

/* -- signals. sigset_t is 128 bytes on Android and four on Darwin, and the guest allocates
 * the set and fills it itself, so a set written by one side and read by the other is not a
 * set at all. Every one of these touches one.
 *
 * The five set operations are written out rather than forwarded by the macro because Darwin
 * declares them as macros, and a macro the generator cannot see past expands before the
 * definition is ever a function. Undefining them is the other half of that: the guest needs
 * the function, and it is the one whose layout is right. */
#undef sigemptyset
#undef sigfillset
#undef sigaddset
#undef sigdelset
#undef sigismember

#define FORWARD_SIGSET(name, decl, args) \
    int name decl { \
        static int (*impl) decl; \
        if (!impl) impl = (int (*) decl)host_impl(#name); \
        return impl ? impl args : -1; }

FORWARD_ANDROID(int, sigaction, (int s, const struct sigaction *a, struct sigaction *o), (s, a, o), -1)
FORWARD_ANDROID(int, sigprocmask, (int how, const sigset_t *set, sigset_t *o), (how, set, o), -1)
FORWARD_SIGSET(sigemptyset, (sigset_t *s), (s))
FORWARD_SIGSET(sigfillset, (sigset_t *s), (s))
FORWARD_SIGSET(sigaddset, (sigset_t *s, int n), (s, n))
FORWARD_SIGSET(sigdelset, (sigset_t *s, int n), (s, n))
FORWARD_SIGSET(sigismember, (const sigset_t *s, int n), (s, n))
FORWARD_ANDROID(int, sigsuspend, (const sigset_t *s), (s), -1)
FORWARD_ANDROID(int, sigpending, (sigset_t *s), (s), -1)
FORWARD_ANDROID(int, sigwait, (const sigset_t *s, int *n), (s, n), -1)

/* -- directories. struct dirent is 280 bytes on Android and over a thousand on Darwin, with
 * the name at a different offset, so a guest reading d_name out of one the host wrote is
 * reading the length field. opendir() is already forwarded by the file hooks above; these
 * are the rest of the walk. */
FORWARD_ANDROID(struct dirent *, readdir, (DIR *d), (d), NULL)
FORWARD_ANDROID(int, closedir, (DIR *d), (d), -1)
FORWARD_ANDROID(int, dirfd, (DIR *d), (d), -1)
FORWARD_ANDROID(void, rewinddir, (DIR *d), (d), (void)0)

/* -- fstat(): the same struct stat the guest's stat() already goes through the file hooks
 * for, and libSystem's is a different arrangement of the same fields. */
FORWARD_ANDROID(int, fstat, (int fd, struct stat *s), (fd, s), -1)

/* -- strerror_r has two incompatible forms: Darwin's returns int and fills the caller's
 * buffer, Bionic's returns char * to a buffer of its own. A caller compiled against Bionic
 * reads the return value as a pointer, so the name cannot be declared here at all; the
 * export is an assembly branch onto an implementation with Bionic's shape. */
char *husk_strerror_r(int e, char *b, size_t l)
{
    static char *(*impl)(int, char *, size_t);
    if (!impl) impl = (char *(*)(int, char *, size_t))host_impl("strerror_r");
    if (!impl) { shim_snprintf(b, l, "error %d", e); return b; }
    return impl(e, b, l);
}

__asm__(".globl _strerror_r\n"
        ".p2align 2\n"
        "_strerror_r:\n"
        "        b       _husk_strerror_r\n"
        ".section __TEXT,__const\n");

/* -- a few more where the platform's answer is not the guest's. getauxval reports the host's
 * page size and vector length, which for a guest whose layout was decided by its own ELF are
 * the wrong numbers; uname's utsname is spelled the same but sized differently. */
FORWARD_ANDROID(unsigned long, getauxval, (unsigned long t), (t), 0)

/* The guest's thread block, for the thread that is running.
 *
 * `mrs Xt, tpidr_el0` is rewritten into a call here. A page inside the dylib's own data
 * would be the obvious place to put it and is wrong: the guest *writes* through the block.
 * Of the 21,799 reads in libPVZ2 alone, 1,993 are followed by a store to [tp + 0x28] -- the
 * stack cookie, which every function built with a protector saves on entry -- and the
 * libraries between them also write 0x18, 0x60, 0xc8, 0x368, 0x9a8 and 0xac8. One page in
 * the image is one page for the main thread, the GL thread and the audio thread at once,
 * and the guest is running all three.
 *
 * The layout is the one husk-tl-ld.c's ensure_tcb() builds and the game is known to run
 * against, because that is the arrangement it already works under: a self pointer at 0,
 * two words its pthread code reads as counts, and the stack cookie at 0x28. One page
 * covers every offset any of the four libraries touches.
 *
 * Per-thread storage is the fast path and a pthread key is the cleanup. The key exists
 * because a thread's TLS block is still needed when its start routine returns -- the
 * destructors registered with __cxa_thread_atexit run after that and read TLS -- so
 * freeing where the trampoline ends would hand them a page that is gone. A key with a
 * destructor is released by the runtime after those, which is the one moment it is safe.
 */
#define GUEST_TCB_BYTES  0x1000
#define GUEST_TCB_COOKIE 0xdeadbeefcafebabeull

static __thread uint64_t *g_tcb;
static pthread_key_t g_tcb_key;
static pthread_once_t g_tcb_once = PTHREAD_ONCE_INIT;

static void tcb_release(void *p) { free(p); }
static void tcb_key_init(void) { pthread_key_create(&g_tcb_key, tcb_release); }

void *husk_guest_tcb_addr(void)
{
    if (!g_tcb) {
        pthread_once(&g_tcb_once, tcb_key_init);
        uint64_t *t = (uint64_t *)calloc(1, GUEST_TCB_BYTES);
        if (!t) return NULL;             /* the guest then faults low, which says why */
        t[0] = (uint64_t)(uintptr_t)t;   /* self */
        t[1] = 1000;
        t[2] = 1000;
        t[5] = GUEST_TCB_COOKIE;         /* [tpidr_el0 + 0x28] */
        g_tcb = t;
        pthread_setspecific(g_tcb_key, t);
    }
    return g_tcb;
}

/* The reserved register's value, for the thread that is running.
 *
 * It cannot live in a converted library's data: that is one image, shared by every thread,
 * and the guest uses the register as a general one as well as a thread pointer -- a NEON
 * string loop in this game does `and x18, x17, #~15` and then indexes off it -- so a shared
 * word hands one thread's value to another, and the next instruction to read through it
 * faults on whatever was there. Here it is per-thread, which is the only place it can be.
 *
 * Keyed by an immediate in the stub's own code, because each library has its own thread
 * block: the same register in two libraries is two different pointers, and a thread runs
 * both.
 */
static __thread uint64_t g_x18;

static void *x18_slot(unsigned i)
{
    /* One word for the thread, not one per library. The translating route keeps the
     * register's value in a single thread-specific slot that every library shares, and
     * that is the behaviour to match: a value libPVZ2 writes is visible to libc++ and to
     * the mod. It also makes the library index irrelevant, which matters because the
     * converter's numbering and the runtime's are not the same.
     *
     * The word starts at zero, as the translating route's slot does. The register is a
     * general one for this game -- it writes x18 in 7,832 places and reads it in 9,470 --
     * and the thread's block is reached through `mrs tpidr_el0`, which is a different
     * thing entirely. Seeding the register with the block's address made the first read
     * of x18 in a thread see a heap pointer where the game expects zero. */
    (void)i;
    return &g_x18;
}

/* One accessor per library, so a stub's index is an immediate in its own code rather than
 * a word in the library's data. That word was reachable and writable: the guest can store
 * anywhere it can compute an address, and a library whose identity came out as 0, 1, 2 or 3
 * is the signature of a value that something had overwritten. An immediate cannot be. */
#define X18_ACCESSOR(n) \
    void *husk_guest_x18_addr_##n(void) { return x18_slot(n); }
X18_ACCESSOR(0)  X18_ACCESSOR(1)  X18_ACCESSOR(2)  X18_ACCESSOR(3)
X18_ACCESSOR(4)  X18_ACCESSOR(5)  X18_ACCESSOR(6)  X18_ACCESSOR(7)
X18_ACCESSOR(8)  X18_ACCESSOR(9)  X18_ACCESSOR(10) X18_ACCESSOR(11)
X18_ACCESSOR(12) X18_ACCESSOR(13) X18_ACCESSOR(14) X18_ACCESSOR(15)

void *husk_guest_x18_addr(unsigned i) { return x18_slot((unsigned)i); }

/* Set by the host when a thread is about to run guest code: the thread block for each
 * library, which is what the register holds between libraries. */
void husk_guest_x18_set(unsigned which, unsigned long value)
{
    *(uint64_t *)x18_slot(which) = (uint64_t)value;
}

/* ------------------------------------------------------------------- stdio streams */

/* bionic's stdin/stdout/stderr are &__sF[0..2] and sizeof(FILE) there is 152 bytes on
 * LP64. A guest reads the address out of __sF and adds its own idea of the stride, so the
 * array has to be here, at Bionic's stride, and the calls that receive one of its three
 * entries have to recognise it: libSystem's fprintf would read Darwin's FILE at an address
 * chosen from Bionic's arithmetic. The three slots are this file's own; the wrapper
 * substitutes the host's stream for the slot's address and everything else -- including a
 * stream the translation layer handed out -- is passed along unchanged. */
#define BIONIC_FILE_SIZE 152
struct bionic_FILE { unsigned char opaque[BIONIC_FILE_SIZE]; };
struct bionic_FILE __sF[3];

/* Bionic's timezone is the number of seconds west of Greenwich -- a long, and the offset
 * itself. Darwin's is a timezone_t, which is a pointer to a structure of the same name, and
 * a bind that went to libSystem would have a guest reading the low half of a pointer as its
 * offset. There are no time zones to choose between on a device that has never been told
 * what time zone it is in, so this is the answer that is right: none. */
long timezone = 0;

/* One of this file's three slots becomes the host's corresponding stream. */
static FILE *shim_stream(FILE *f)
{
    if (f == (FILE *)&__sF[0]) return stdin;
    if (f == (FILE *)&__sF[1]) return stdout;
    if (f == (FILE *)&__sF[2]) return stderr;
    return NULL;
}

/* libSystem's own three are already host streams and need no translation. This file's own
 * logging writes to them -- stderr is a macro for __stderrp here -- and taking that path
 * through the forwarder would ask the translation layer about a FILE it has never heard of. */
static FILE *host_stream(FILE *f)
{
    FILE *s = shim_stream(f);
    if (s) return s;
    if (f == stdin || f == stdout || f == stderr) return f;
    return NULL;
}

/* The stdio calls that take a stream have two destinations: one of this file's three slots,
 * which becomes the host's corresponding stream, and anything else -- a stream the
 * translation layer handed out, most often -- which goes to the implementation that knows
 * what that object is. */
static int     (*real_fputs)(const char *, FILE *);
static int     (*real_fputc)(int, FILE *);
static size_t  (*real_fwrite)(const void *, size_t, size_t, FILE *);
static int     (*real_fflush)(FILE *);
static size_t  (*real_fread)(void *, size_t, size_t, FILE *);
static int     (*real_fseek)(FILE *, long, int);
static long    (*real_ftell)(FILE *);
static char   *(*real_fgets)(char *, int, FILE *);
static int     (*real_fgetc)(FILE *);
static int     (*real_feof)(FILE *);
static int     (*real_ferror)(FILE *);
static int     (*real_fileno)(FILE *);
static int     (*real_fsetpos)(FILE *, const fpos_t *);
static int     (*android_fputs)(const char *, FILE *);
static int     (*android_fputc)(int, FILE *);
static size_t  (*android_fwrite)(const void *, size_t, size_t, FILE *);
static int     (*android_fflush)(FILE *);
static int   (*android_fclose)(FILE *);
static size_t (*android_fread)(void *, size_t, size_t, FILE *);
static int   (*android_fseek)(FILE *, long, int);
static long  (*android_ftell)(FILE *);
static char *(*android_fgets)(char *, int, FILE *);
static int   (*android_fgetc)(FILE *);
static int   (*android_feof)(FILE *);
static int   (*android_ferror)(FILE *);
static int   (*android_fileno)(FILE *);
static int   (*android_fsetpos)(FILE *, const fpos_t *);

static void stdio_ready(void)
{
    static int host_done;
    if (!host_done) {
        *(void **)(&real_fputs)   = dlsym(RTLD_NEXT, "fputs");
        *(void **)(&real_fputc)   = dlsym(RTLD_NEXT, "fputc");
        *(void **)(&real_fwrite)  = dlsym(RTLD_NEXT, "fwrite");
        *(void **)(&real_fflush)  = dlsym(RTLD_NEXT, "fflush");
        *(void **)(&real_fread)   = dlsym(RTLD_NEXT, "fread");
        *(void **)(&real_fseek)   = dlsym(RTLD_NEXT, "fseek");
        *(void **)(&real_ftell)   = dlsym(RTLD_NEXT, "ftell");
        *(void **)(&real_fgets)   = dlsym(RTLD_NEXT, "fgets");
        *(void **)(&real_fgetc)   = dlsym(RTLD_NEXT, "fgetc");
        *(void **)(&real_feof)    = dlsym(RTLD_NEXT, "feof");
        *(void **)(&real_ferror)  = dlsym(RTLD_NEXT, "ferror");
        *(void **)(&real_fileno)  = dlsym(RTLD_NEXT, "fileno");
        *(void **)(&real_fsetpos) = dlsym(RTLD_NEXT, "fsetpos");
        host_done = 1;
    }
    /* The translation layer's own implementations are not there yet when this
     * file's constructor runs: the layer loads this file and publishes its
     * table afterwards. Looking them up once, then, leaves them null for ever
     * -- and a guest's write that goes nowhere is silent: the file is created
     * and stays empty, and the game's progress is lost with no error anywhere.
     * They are looked up again until they are there. */
    if (!android_fputs)  android_fputs  = (int (*)(const char *, FILE *))host_impl("fputs");
    if (!android_fputc)  android_fputc  = (int (*)(int, FILE *))host_impl("fputc");
    if (!android_fwrite) android_fwrite = (size_t (*)(const void *, size_t, size_t, FILE *))host_impl("fwrite");
    if (!android_fflush) android_fflush = (int (*)(FILE *))host_impl("fflush");
    if (!android_fclose) android_fclose = (int (*)(FILE *))host_impl("fclose");
    if (!android_fread)  android_fread  = (size_t (*)(void *, size_t, size_t, FILE *))host_impl("fread");
    if (!android_fseek)  android_fseek  = (int (*)(FILE *, long, int))host_impl("fseek");
    if (!android_ftell)  android_ftell  = (long (*)(FILE *))host_impl("ftell");
    if (!android_fgets)  android_fgets  = (char *(*)(char *, int, FILE *))host_impl("fgets");
    if (!android_fgetc)  android_fgetc  = (int (*)(FILE *))host_impl("fgetc");
    if (!android_feof)   android_feof   = (int (*)(FILE *))host_impl("feof");
    if (!android_ferror) android_ferror = (int (*)(FILE *))host_impl("ferror");
    if (!android_fileno) android_fileno = (int (*)(FILE *))host_impl("fileno");
    if (!android_fsetpos) android_fsetpos = (int (*)(FILE *, const fpos_t *))host_impl("fsetpos");
}

/* The first call into any of these, from anywhere. Resolved once; a constructor gets it
 * done before any guest thread exists, and the call at the top of each wrapper covers a
 * first use that somehow arrives earlier. */
static void stdio_start(void) { stdio_ready(); }
__attribute__((constructor)) static void stdio_ctor(void) { stdio_start(); }

/* fprintf is one of the guest's variadic entry points, so it is defined with them
 * further down: the guest's fprintf takes its arguments the Android way, and the
 * translation layer's fprintf is the one that reads them that way. */

int fputs(const char *s, FILE *f)
{
    stdio_start();
    FILE *t = host_stream(f);
    if (t) return real_fputs(s, t);
    return android_fputs ? android_fputs(s, f) : -1;
}

int fputc(int c, FILE *f)
{
    stdio_start();
    FILE *t = host_stream(f);
    if (t) return real_fputc(c, t);
    return android_fputc ? android_fputc(c, f) : -1;
}

size_t fwrite(const void *b, size_t sz, size_t n, FILE *f)
{
    stdio_start();
    FILE *t = host_stream(f);
    if (t) return real_fwrite(b, sz, n, t);
    return android_fwrite ? android_fwrite(b, sz, n, f) : 0;
}

int fflush(FILE *f)
{
    stdio_start();
    FILE *t = host_stream(f);
    if (t) return real_fflush(t);
    return android_fflush ? android_fflush(f) : -1;
}

/* The rest of the stream calls. They are here for the same reason fwrite is:
 * without them the guest's import binds to libSystem's, which reads a Darwin
 * FILE where the guest means its own -- and, for fclose, which closes the
 * host's stdout and, worse, never tells the translation layer that a stream
 * it was writing crash-safely has finished, so the new file is never put in
 * place and the old one stays. */
/* The gzip calls that name a file. The guest's path is an Android path
 * ("/sdcard/Android/data/...") and the host's zlib has never heard of it, so
 * the call has to go through the translation layer, which maps the path. Left
 * to zlib itself it opens a file that does not exist, and the archive the game
 * downloads is written and then never read back.
 *
 * The compression calls themselves (inflate, deflate, crc32, adler32,
 * zlibVersion) are NOT here: the shim must not define a name a linked library
 * provides, and those are zlib's. Only the three that carry a path are. */
static void *(*android_gzopen)(const char *, const char *);
static int   (*android_gzread)(void *, void *, unsigned);
static int   (*android_gzclose)(void *);

void *gzopen(const char *path, const char *mode)
{
    if (!android_gzopen) android_gzopen = (void *(*)(const char *, const char *))host_impl("gzopen");
    return android_gzopen ? android_gzopen(path, mode) : NULL;
}
int gzread(void *f, void *buf, unsigned len)
{
    if (!android_gzread) android_gzread = (int (*)(void *, void *, unsigned))host_impl("gzread");
    return android_gzread ? android_gzread(f, buf, len) : -1;
}
int gzclose(void *f)
{
    if (!android_gzclose) android_gzclose = (int (*)(void *))host_impl("gzclose");
    return android_gzclose ? android_gzclose(f) : -1;
}

int fclose(FILE *f)
{
    stdio_start();
    /* The guest's own three are the host's: closing them would close the
     * process's stdout for good. */
    if (host_stream(f)) return 0;
    return android_fclose ? android_fclose(f) : -1;
}
size_t fread(void *p, size_t sz, size_t n, FILE *f)
{
    stdio_start();
    FILE *t = host_stream(f);
    if (t) return real_fread(p, sz, n, t);
    return android_fread ? android_fread(p, sz, n, f) : 0;
}
int fseek(FILE *f, long off, int whence)
{
    stdio_start();
    FILE *t = host_stream(f);
    if (t) return real_fseek(t, off, whence);
    return android_fseek ? android_fseek(f, off, whence) : -1;
}
long ftell(FILE *f)
{
    stdio_start();
    FILE *t = host_stream(f);
    if (t) return real_ftell(t);
    return android_ftell ? android_ftell(f) : -1;
}
char *fgets(char *s, int n, FILE *f)
{
    stdio_start();
    FILE *t = host_stream(f);
    if (t) return real_fgets(s, n, t);
    return android_fgets ? android_fgets(s, n, f) : NULL;
}
int fgetc(FILE *f)
{
    stdio_start();
    FILE *t = host_stream(f);
    if (t) return real_fgetc(t);
    return android_fgetc ? android_fgetc(f) : EOF;
}
int feof(FILE *f)
{
    stdio_start();
    FILE *t = host_stream(f);
    if (t) return real_feof(t);
    return android_feof ? android_feof(f) : 1;
}
int ferror(FILE *f)
{
    stdio_start();
    FILE *t = host_stream(f);
    if (t) return real_ferror(t);
    return android_ferror ? android_ferror(f) : 0;
}
int fileno(FILE *f)
{
    stdio_start();
    FILE *t = host_stream(f);
    if (t) return real_fileno(t);
    return android_fileno ? android_fileno(f) : -1;
}
int fsetpos(FILE *f, const fpos_t *pos)
{
    stdio_start();
    FILE *t = host_stream(f);
    if (t) return real_fsetpos(t, pos);
    return android_fsetpos ? android_fsetpos(f, pos) : -1;
}

/* --------------------------------------------------- the file hooks: the host's own VFS */

static void *(*host_fopen)(const char *, const char *);
static int   (*host_open)(const char *, int, ...);
static int   (*host_stat)(const char *, void *, int);
static int   (*host_access)(const char *, int);
static int   (*host_mkdir)(const char *, int);
static int   (*host_unlink)(const char *);
static void *(*host_opendir)(const char *);

void husk_shim_set_file_hooks(void *fopen_fn, void *open_fn, void *stat_fn,
                              void *access_fn, void *mkdir_fn, void *unlink_fn,
                              void *opendir_fn)
{
    host_fopen   = (void *(*)(const char *, const char *))fopen_fn;
    host_open    = (int (*)(const char *, int, ...))open_fn;
    host_stat    = (int (*)(const char *, void *, int))stat_fn;
    host_access  = (int (*)(const char *, int))access_fn;
    host_mkdir   = (int (*)(const char *, int))mkdir_fn;
    host_unlink  = (int (*)(const char *))unlink_fn;
    host_opendir = (void *(*)(const char *))opendir_fn;
}

FILE *fopen(const char *path, const char *mode)
{
    if (host_fopen) return (FILE *)host_fopen(path, mode);
    static FILE *(*real)(const char *, const char *);
    if (!real) *(void **)(&real) = dlsym(RTLD_NEXT, "fopen");
    return real(path, mode);
}

/* The shim's own open, with the mode as an ordinary argument. The guest's open is a
 * variadic tail, and Darwin's convention would put the mode on the stack where the
 * guest does not put it, so the shim keeps its own copy with the mode where the guest's
 * is: in a register. */
static int shim_open(const char *path, int flags, unsigned mode)
{
    if (!(flags & O_CREAT)) mode = 0666;
    if (host_open) return host_open(path, flags, mode);
    static int (*real)(const char *, int, ...);
    if (!real) *(void **)(&real) = dlsym(RTLD_NEXT, "open");
    return real(path, flags, mode);
}

static int shim_openat(int d, const char *p, int f, unsigned mode)
{
    fs_ready();
    return android_openat ? android_openat(d, p, f, (int)mode) : -1;
}

static int shim_fcntl(int fd, int cmd, long arg)
{
    fs_ready();
    return android_fcntl ? android_fcntl(fd, cmd, (void *)arg) : -1;
}


int stat(const char *path, struct stat *st) { return host_stat ? host_stat(path, st, 0) : 0; }
int lstat(const char *path, struct stat *st) { return host_stat ? host_stat(path, st, 1) : 0; }
int __xstat(int v, const char *path, struct stat *st) { (void)v; return stat(path, st); }
int __lxstat(int v, const char *path, struct stat *st) { (void)v; return lstat(path, st); }
int fstatat(int fd, const char *path, struct stat *st, int flags) { (void)fd; (void)flags; return stat(path, st); }

int access(const char *path, int mode)
{
    if (host_access) return host_access(path, mode);
    static int (*real)(const char *, int);
    if (!real) *(void **)(&real) = dlsym(RTLD_NEXT, "access");
    return real(path, mode);
}

int mkdir(const char *path, mode_t mode)
{
    if (host_mkdir) return host_mkdir(path, mode);
    static int (*real)(const char *, mode_t);
    if (!real) *(void **)(&real) = dlsym(RTLD_NEXT, "mkdir");
    return real(path, mode);
}

int unlink(const char *path)
{
    if (host_unlink) return host_unlink(path);
    static int (*real)(const char *);
    if (!real) *(void **)(&real) = dlsym(RTLD_NEXT, "unlink");
    return real(path);
}

DIR *opendir(const char *path)
{
    if (host_opendir) return (DIR *)host_opendir(path);
    static DIR *(*real)(const char *);
    if (!real) *(void **)(&real) = dlsym(RTLD_NEXT, "opendir");
    return real(path);
}

/* ------------------------------------------------------------------ diagnosis
 *
 * A guest abort says nothing about where it came from, and the host's own handler does
 * not always get a look in. This one prints the guest's stack before the process goes. */
static void shim_abort_handler(int sig)
{
    static volatile int inside;
    if (inside++) return;                 /* a fault inside the handler is not news */
    void *frames[24];
    int n = backtrace(frames, 24);
    husk_shim_logf("bionic-shim: signal %d, %d frames\n", sig, n);
    for (int i = 0; i < n; i++) {
        Dl_info di;
        if (dladdr(frames[i], &di) && di.dli_sname)
            husk_shim_logf("   %s %s\n", di.dli_fname ? di.dli_fname : "?", di.dli_sname);
        else
            husk_shim_logf("   %p\n", frames[i]);
    }
    _exit(128 + sig);
}

/* The guest's runtime calls abort() through its own imports, so this is where it lands. */
void abort(void)
{
    husk_shim_logf("bionic-shim: the guest called abort()\n");
    shim_abort_handler(SIGABRT);
}

/* ------------------------------------------------------------------ the guest's hook API
 *
 * A64HookFunction stores a branch into the library it is hooking. In a converted library
 * that code is signed and read-only, so the branch was written offline, into the image,
 * before it was signed (scripts/aot-convert.py, --hook-site): the site holds a branch to a
 * dispatch stub that reads the replacement's address out of a slot in the same library's
 * data and jumps to it. All this function has to do is find that stub and fill the slot.
 *
 * A site that is not one of ours -- the guest's own hooks, which its engine still asks for
 * at run time -- is recorded instead, as it always was: there is no writable text to put a
 * branch in, so the request can only be answered with the original code. */
unsigned long husk_hook_sites[512];
void *husk_hook_replacements[512];
void *husk_hook_tramp_slots[512];
int husk_hook_count;

/* The stub the branch at a baked site jumps to, or NULL when the site is not a baked one.
 *
 * Decoding the branch is the only way to find it: nothing else refers to the stub, and a
 * published table would have to be found first, which is the same problem. The magic word
 * at the far end is what tells a baked stub from a branch the guest wrote itself, and the
 * dladdr check keeps a misdecoded branch from being read as one -- a wrong address here
 * would fault inside the hook API, where the fault says nothing about why. */
static char *husk_hook_stub_of(void *site)
{
    uintptr_t pc = (uintptr_t)site;
    if (pc & 3) return NULL;                       /* an ARM64 branch is 4-byte aligned */
    /* The site is dereferenced only after dladdr has said it is in a mapped image: a
     * request for a site outside every image would fault here, inside the hook API,
     * where the fault says nothing about where the pointer came from. */
    Dl_info here, there;
    if (!dladdr(site, &here)) return NULL;
    uint32_t word = *(volatile uint32_t *)site;
    if ((word & 0xFC000000u) != 0x14000000u) return NULL;   /* not a plain `b` */
    int64_t imm = (int64_t)(word & 0x03FFFFFFu);
    if (imm & 0x02000000) imm -= 0x04000000;       /* the 26 bits are a signed word count */
    char *stub = (char *)(pc + imm * 4);
    if (!dladdr(stub, &there)) return NULL;
    if (here.dli_fbase != there.dli_fbase) return NULL;     /* not in the same library */
    if (*(volatile uint32_t *)(stub + HOOK_MAGIC_AT) != HOOK_MAGIC) return NULL;
    return stub;
}

/* The two distances the stub carries: how far from the stub the slot is, and how far
 * the trampoline is. They are 8 bytes at an offset that is not 8-aligned (the stub is
 * 8-aligned, the fields are at +20 and +28), so they are read a byte at a time rather
 * than with an unaligned load. */
static int64_t husk_hook_delta(const char *stub, int at)
{
    int64_t d;
    memcpy(&d, stub + at, sizeof d);
    return d;
}

/* Answer one hook request: fill the slot of the stub at the site, so that the baked branch
 * reaches the replacement from now on, and hand the caller the trampoline -- the site's own
 * displaced instruction and a branch back -- as the way to reach the original code. */
static int husk_hook_install(char *stub, void *replacement, void **trampoline,
                             void *site, const char *what)
{
    void **slot = (void **)(stub + husk_hook_delta(stub, HOOK_SLOT_DELTA));
    *slot = replacement;
    if (trampoline) *trampoline = stub + husk_hook_delta(stub, HOOK_TRAMP_DELTA);
    if (getenv("HUSK_HOOK_TRACE"))
        husk_shim_logf("hook: %s -> %p (site %p, slot %p, tramp %p)\n", what, replacement,
                       site, (void *)slot, trampoline ? *trampoline : NULL);
    return 0;
}

int A64HookFunction(void *site, void *replacement, void **trampoline)
{
    /* The mod's own signature, which the disassembly settles: the site is x0, the
     * replacement x1, and x2 is a pointer the caller wants the trampoline written into
     * (`str x22, [x19]` at libRFLib.so:0x1a2be0). It returns zero on success.
     *
     * The trampoline is where the caller jumps to reach the function it just hooked, and
     * the mod keeps it in a global and calls it (`ldr x8,[x8,#0x8e0]; blr x8`), so what
     * goes there has to run the original function. For a baked site that is the
     * trampoline the converter baked next to the stub; for a site that is not one of ours
     * the original is still where it was, and the site itself is what to hand back.
     */
    char *stub = husk_hook_stub_of(site);
    if (stub)
        return husk_hook_install(stub, replacement, trampoline, site, "A64HookFunction");
    if (trampoline) *trampoline = site;
    if (husk_hook_count < (int)(sizeof husk_hook_sites / sizeof *husk_hook_sites)) {
        husk_hook_sites[husk_hook_count] = (unsigned long)site;
        husk_hook_replacements[husk_hook_count] = replacement;
        husk_hook_tramp_slots[husk_hook_count] = trampoline;
        husk_hook_count++;
    }
    /* A request the bake does not answer is a request that is not applied, and the game
     * then runs with that patch missing -- silently, because the caller is told it
     * succeeded. It is logged whether or not the trace is on: a missed hook has to
     * announce itself, and the count of these lines is the measure of the bake's
     * completeness. A healthy run has none. */
    husk_shim_logf("hook: site %p is not baked; the hook to %p was NOT installed\n",
                   site, replacement);
    if (getenv("HUSK_HOOK_TRACE"))
        husk_shim_logf("hook: %s -> %p (site %p, tramp %p) [not baked]\n",
                       "A64HookFunction", replacement, site, trampoline);
    return 0;
}

int A64HookFunctionV(void *addr, void *replace, void *trampoline, unsigned long len)
{
    /* The mod's order, again from the disassembly: `sub x22, x1, x0` at
     * libRFLib.so:0x1a2174 is the branch-range check, so x0 is the site and x1 is the
     * replacement; x2 is the trampoline and x3 its length. Writing the replacement
     * through the site instead -- which is what the old argument order here did -- would
     * put eight bytes into the first instruction of a function in a read-only, signed
     * page, and the fault would say nothing about how the pointer got there.
     *
     * The request is recorded for baking; the branch itself is written offline, into the
     * converted library, because there is no writable text at run time. */
    (void)trampoline; (void)len;
    if (husk_hook_count < (int)(sizeof husk_hook_sites / sizeof *husk_hook_sites)) {
        husk_hook_sites[husk_hook_count++] = (unsigned long)addr;
        if (getenv("HUSK_HOOK_TRACE"))
            husk_shim_logf("hook: %sV -> %p (site %p)\n", "A64HookFunctionV", replace, addr);
    }
    return 0;
}

/* ------------------------------------------------------------------ syslog

 * libc++ says what went wrong through syslog before it throws. syslog and vsyslog are
 * two of the guest's variadic entry points, so they are defined with them further down:
 * the guest's syslog takes its arguments the Android way, and the translation layer's
 * is the one that reads them that way. The translation layer's vsyslog does not call
 * os_log, which is what faults on some of the arguments a hardened libc++ hands it --
 * the message is the point, and the system's copy of it is not. */

#ifdef __cplusplus
}
#endif
