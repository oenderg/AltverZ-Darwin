/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Android's C library (bionic), for the game's code, on Windows.
 *
 * The Darwin runtime binds every libc function whose ABI matches Darwin's straight to
 * libSystem and wraps the rest. Windows matches far less: `long` is 32 bits there and
 * 64 on Android, `wchar_t` 16 bits and 32, and struct stat, dirent, tm, timespec,
 * FILE, the pthread types, jmp_buf, z_stream, errno values and open() flags all
 * differ. So this table is written for Windows from the import list of the four
 * libraries the game loads: each entry either has the same ABI on both sides (string
 * and memory functions, float math) or is a wrapper here that converts.
 *
 * Split over files by area: this one (the core: table, log, errno, paths, dl*,
 * process, environment, atexit, signals, raw system calls), -io (files, stdio,
 * directories), -thread (pthreads, semaphores) and -str (strings, wide characters,
 * locales, time, numbers, printf).
 */
#include "husk-tl-bionic-win.h"

#include <windows.h>
#include <bcrypt.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "husk-tl-ld.h"
#include "husk-tl-va.h"

/* --------------------------------------------------------------------- log */

static SRWLOCK g_log_lock = SRWLOCK_INIT;
static FILE *g_log_file;
static LARGE_INTEGER g_log_t0, g_log_freq;

/* Each start begins its own log; a restart to fit the window is the same session and appends to it. */
void tl_win_log_open_mode(const char *path, int append)
{
    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log_file) fclose(g_log_file);
    g_log_file = path ? fopen(path, append ? "a" : "w") : NULL;
    ReleaseSRWLockExclusive(&g_log_lock);
}

void tl_win_log_open(const char *path)
{
    tl_win_log_open_mode(path, 0);
}

void tl_log_line(const char *fmt, ...)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    AcquireSRWLockExclusive(&g_log_lock);
    if (!g_log_freq.QuadPart) { QueryPerformanceFrequency(&g_log_freq); g_log_t0 = now; }
    double t = (double)(now.QuadPart - g_log_t0.QuadPart) / (double)g_log_freq.QuadPart;
    char line[4096];
    int n = snprintf(line, sizeof(line), "[%7.3f] ", t);
    va_list ap; va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    fputs(line, stderr); fputc('\n', stderr); fflush(stderr);
    if (g_log_file) { fputs(line, g_log_file); fputc('\n', g_log_file); fflush(g_log_file); }
    ReleaseSRWLockExclusive(&g_log_lock);
}

void tl_note_once(const char *what)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static const char *seen[256]; static int n;
    AcquireSRWLockExclusive(&lock);
    for (int i = 0; i < n; i++) if (seen[i] == what || !strcmp(seen[i], what)) { ReleaseSRWLockExclusive(&lock); return; }
    if (n < 256) seen[n++] = what;
    ReleaseSRWLockExclusive(&lock);
    tl_log_line("bionic: %s", what);
}

/* ------------------------------------------------------------------- errno */

/* The guest's errno: Linux numbers, one per thread. __errno() hands out its address. */
static __thread int t_guest_errno;
int *tl_guest_errno_ptr(void) { return &t_guest_errno; }
void tl_set_guest_errno(int e) { t_guest_errno = e; }

/* Windows' CRT numbers 1-34 as Linux does; the rest differ. */
int tl_errno_to_guest(int e)
{
    if (e >= 0 && e <= 34) return e;
    switch (e) {
    case EDEADLK: return 35; case ENAMETOOLONG: return 36; case ENOLCK: return 37; case ENOSYS: return 38;
    case ENOTEMPTY: return 39; case EILSEQ: return 84;
#ifdef ETIMEDOUT
    case ETIMEDOUT: return 110;
#endif
#ifdef ECONNREFUSED
    case ECONNREFUSED: return 111;
#endif
#ifdef EINPROGRESS
    case EINPROGRESS: return 115;
#endif
#ifdef EWOULDBLOCK
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK: return 11;
#endif
#endif
#ifdef EOVERFLOW
    case EOVERFLOW: return 75;
#endif
#ifdef ENOTSUP
    case ENOTSUP: return 95;
#endif
    default: return e < 128 ? e : 5;   /* EIO */
    }
}
int tl_errno_from_guest(int g)
{
    if (g >= 0 && g <= 34) return g;
    switch (g) {
    case 35: return EDEADLK; case 36: return ENAMETOOLONG; case 37: return ENOLCK; case 38: return ENOSYS;
    case 39: return ENOTEMPTY; case 84: return EILSEQ;
    default: return EINVAL;
    }
}
void tl_win_set_errno_from_crt(void) { t_guest_errno = tl_errno_to_guest(errno); }
void tl_win_set_errno_from_win32(DWORD err)
{
    int e;
    switch (err) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE: e = G_ENOENT; break;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: e = G_EACCES; break;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: e = G_EEXIST; break;
    case ERROR_DIR_NOT_EMPTY: e = G_ENOTEMPTY; break;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: e = G_ENOMEM; break;
    case ERROR_DISK_FULL: e = G_ENOSPC; break;
    case ERROR_INVALID_HANDLE: e = G_EBADF; break;
    case ERROR_DIRECTORY: e = G_ENOTDIR; break;
    default: e = G_EIO; break;
    }
    t_guest_errno = e;
}

static int *b___errno(void) { return &t_guest_errno; }

/* ------------------------------------------------------------- data folder */

static char g_data_dir[1024];
void tl_set_data_dir(const char *dir)
{
    snprintf(g_data_dir, sizeof(g_data_dir), "%s", dir ? dir : "");
    for (char *p = g_data_dir; *p; p++) if (*p == '\\') *p = '/';
    size_t n = strlen(g_data_dir);
    while (n > 3 && g_data_dir[n - 1] == '/') g_data_dir[--n] = 0;
    tl_win_mkdirs(g_data_dir);
}
const char *tl_data_dir(void) { return g_data_dir[0] ? g_data_dir : "."; }

/* The game's saves are the one part of Android's storage that belongs with the
 * player's settings rather than with the installed data: the host names a
 * folder for /sdcard/Android/data (the app's files folder, where the game keeps
 * its saves), while the OBB and the large caches stay in the data folder. An
 * empty or missing folder keeps the old one-folder layout. */
static char g_saves_dir[1024];
void tl_win_set_saves_dir(const char *dir)
{
    snprintf(g_saves_dir, sizeof(g_saves_dir), "%s", dir ? dir : "");
    for (char *p = g_saves_dir; *p; p++) if (*p == '\\') *p = '/';
    size_t n = strlen(g_saves_dir);
    while (n > 3 && g_saves_dir[n - 1] == '/') g_saves_dir[--n] = 0;
    if (g_saves_dir[0]) {
        tl_win_mkdirs(g_saves_dir);
        tl_log_line("bionic: the saves folder is %s", g_saves_dir);
    }
}

void tl_win_mkdirs(const char *path)
{
    char t[1100];
    snprintf(t, sizeof(t), "%s", path);
    for (char *p = t; *p; p++) {
        if ((*p == '/' || *p == '\\') && p > t && p[-1] != ':') { char c = *p; *p = 0; CreateDirectoryA(t, NULL); *p = c; }
    }
    CreateDirectoryA(t, NULL);
}

/*
 * A guest path, as a Windows one. Android's storage is folders in the data folder:
 * /sdcard (and /storage/emulated/0, its real name) is <data>/sdcard, the app's own
 * folder /data/data/<package> (or /data/user/0/<package>) is <data> itself. A path
 * the guest builds from those is absolute and starts with '/'; anything else under
 * '/' has no Windows meaning and is put under <data>/root so it cannot escape.
 * A relative path is left alone (the current directory is the data folder).
 */
const char *tl_path_resolve(const char *path, char *buf, size_t n)
{
    if (!path || path[0] != '/') return path;
    /* Android's kernel collapses repeated slashes: "//sdcard/x" is "/sdcard/x" */
    char norm[1100];
    { size_t j = 0; for (const char *p = path; *p && j + 1 < sizeof(norm); p++) { if (p[0] == '/' && p[1] == '/') continue; norm[j++] = *p; } norm[j] = 0; }
    const char *p = norm;
    static const struct { const char *prefix; const char *to; } maps[] = {
        { "/sdcard", "/sdcard" }, { "/storage/emulated/0", "/sdcard" }, { "/storage/self/primary", "/sdcard" },
        { "/mnt/sdcard", "/sdcard" },
    };
    for (size_t i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
        size_t l = strlen(maps[i].prefix);
        if (!strncmp(p, maps[i].prefix, l) && (p[l] == 0 || p[l] == '/')) {
            const char *rest = p + l;
            const char *base = tl_data_dir();
            if (g_saves_dir[0] && !strncmp(rest, "/Android/data", 13) && (rest[13] == 0 || rest[13] == '/'))
                base = g_saves_dir;
            snprintf(buf, n, "%s%s%s", base, maps[i].to, rest);
            return buf;
        }
    }
    static const char *const app_roots[] = { "/data/user/0/", "/data/data/" };
    for (int i = 0; i < 2; i++) {
        size_t l = strlen(app_roots[i]);
        if (strncmp(p, app_roots[i], l) || !p[l] || p[l] == '/') continue;
        const char *rest = strchr(p + l, '/');
        snprintf(buf, n, "%s%s", tl_data_dir(), rest ? rest : "");
        return buf;
    }
    if (!strncmp(p, "/data/local/tmp", 15) && (p[15] == 0 || p[15] == '/')) { snprintf(buf, n, "%s/cache/local-tmp%s", tl_data_dir(), p + 15); return buf; }
    /* A path that is already in the data folder (the runtime passes Windows paths to itself through guest calls): unchanged. */
    snprintf(buf, n, "%s/root%s", tl_data_dir(), p);
    return buf;
}

/* ----------------------------------------------------------------- dl*** */

typedef struct { int unused; } sysh;
static sysh g_sys_handle[16];
static char g_sys_names[16][48];
static int g_nsys;
static __thread char t_dlerr[200];
static __thread int t_dlerr_set;
static void dl_fail(const char *fmt, const char *arg) { snprintf(t_dlerr, sizeof(t_dlerr), fmt, arg); t_dlerr_set = 1; }

static void *b_dlopen(const char *path, int flags)
{
    if (!path) return &g_sys_handle[0];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (flags & 4) {   /* RTLD_NOLOAD */
        tl_lib *L = tl_ld_find_lib(base);
        if (!L) dl_fail("dlopen failed: library \"%s\" is not loaded", path);
        return L;
    }
    if (tl_bionic_is_system_lib(base)) {
        for (int i = 0; i < g_nsys; i++) if (!strcmp(g_sys_names[i], base)) return &g_sys_handle[i + 1];
        if (g_nsys < 15) { snprintf(g_sys_names[g_nsys], 48, "%s", base); g_nsys++; return &g_sys_handle[g_nsys]; }
        return &g_sys_handle[0];
    }
    tl_lib *L = tl_ld_find_lib(base);
    if (!L) L = tl_ld_load(base);
    if (getenv("TL_DL_TRACE")) tl_log_line("dl: dlopen(%s) -> %s", path, L ? "ok" : "not found");
    if (!L) { dl_fail("dlopen failed: library \"%s\" not found", path); return NULL; }
    tl_ld_init(L);
    return L;
}

static void *b_dlsym(void *handle, const char *name)
{
    if (!name) return NULL;
    void *r = NULL;
    if (handle == (void *)-1L) {
        r = tl_bionic_find(name);
        if (!r) r = tl_ld_sym(NULL, name);
    } else if (handle == NULL) {
        r = tl_ld_sym(NULL, name);
        if (!r) r = tl_bionic_find(name);
    } else if ((sysh *)handle >= g_sys_handle && (sysh *)handle < g_sys_handle + 16) {
        r = tl_bionic_find(name);
    } else {
        r = tl_ld_sym_deep((tl_lib *)handle, name, NULL);
    }
    if (getenv("TL_DL_TRACE")) tl_log_line("dl: dlsym(%s) -> %s", name, r ? "found" : "missing");
    if (!r) dl_fail("undefined symbol: %s", name);
    return r;
}
static int b_dlclose(void *h) { (void)h; return 0; }
static char *b_dlerror(void) { if (!t_dlerr_set) return NULL; t_dlerr_set = 0; return t_dlerr; }

typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } guest_dl_info;
static int b_dladdr(const void *addr, guest_dl_info *info)
{
    const char *ln = NULL; const void *sa = NULL;
    const char *sym = tl_ld_symbol_at(addr, &ln, &sa);
    if (!ln) return 0;
    info->dli_fname = ln;
    info->dli_fbase = tl_ld_lib_base(tl_ld_lib_of(addr));
    info->dli_sname = sym;
    info->dli_saddr = (void *)sa;
    return 1;
}

typedef struct { uint64_t addr; const char *name; const void *phdr; uint16_t phnum; uint64_t adds, subs; size_t tls_modid; void *tls_data; } guest_phdr_info;
typedef int (*guest_phdr_cb)(guest_phdr_info *, size_t, void *);
struct phdr_ctx { guest_phdr_cb cb; void *data; };
static int phdr_trampoline(uintptr_t bias, const char *name, const void *phdr, unsigned phnum, void *user)
{
    struct phdr_ctx *c = user;
    guest_phdr_info i = { bias, name, phdr, (uint16_t)phnum, 0, 0, 0, NULL };
    return c->cb(&i, sizeof(i), c->data);
}
static int b_dl_iterate_phdr(guest_phdr_cb cb, void *data) { struct phdr_ctx c = { cb, data }; return tl_ld_iterate(phdr_trampoline, &c); }

bool tl_bionic_is_system_lib(const char *soname)
{
    static const char *const sys[] = {
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libEGL.so", "libGLESv1_CM.so",
        "libGLESv2.so", "libGLESv3.so", "libz.so", "libmediandk.so", "libOpenSLES.so", "libaaudio.so",
        "libvulkan.so", "libnativewindow.so", "libjnigraphics.so", "libstdc++.so", NULL };
    for (int i = 0; sys[i]; i++) if (!strcmp(sys[i], soname)) return true;
    return false;
}

/* ----------------------------------------------------------------- logging */

static const char *prio_name(int p) { static const char *n[] = { "?", "?", "V", "D", "I", "W", "E", "F", "S" }; return p >= 0 && p < 9 ? n[p] : "?"; }
static int b___android_log_write(int prio, const char *tag, const char *msg)
{
    tl_log_line("android[%s/%s]: %s", prio_name(prio), tag ? tag : "", msg ? msg : "");
    return 1;
}
int tl_va_android_log_print_impl(tl_va_frame *f);
int tl_va_android_log_print_impl(tl_va_frame *f)
{
    tl_va_list ap; tl_va_start(f, 3, 0, &ap);
    int prio = (int)f->gp[0]; const char *tag = (const char *)f->gp[1], *fmt = (const char *)f->gp[2];
    char buf[2048];
    tl_format(buf, sizeof(buf), fmt, &ap);
    return b___android_log_write(prio, tag, buf);
}
TL_VA_STUB(tl_va_android_log_print, tl_va_android_log_print_impl);
extern void tl_va_android_log_print(void);
static void b_android_set_abort_message(const char *m) { tl_log_line("bionic: abort message: %s", m ? m : ""); }

/* syslog: the same log */
static void b_openlog(const char *id, int opt, int fac) { (void)id; (void)opt; (void)fac; }
static void b_closelog(void) {}
int tl_va_syslog_impl(tl_va_frame *f);
int tl_va_syslog_impl(tl_va_frame *f)
{
    tl_va_list ap; tl_va_start(f, 2, 0, &ap);
    char buf[2048]; tl_format(buf, sizeof(buf), (const char *)f->gp[1], &ap);
    tl_log_line("syslog[%d]: %s", (int)f->gp[0], buf);
    return 0;
}
TL_VA_STUB(tl_va_syslog, tl_va_syslog_impl);
extern void tl_va_syslog(void);

/* ----------------------------------------------------------------- process */

void (*tl_guest_exit_hook)(int status);

static void b_abort(void)
{
    tl_log_line("bionic: the game called abort()");
    if (tl_guest_exit_hook) tl_guest_exit_hook(134);
    TerminateProcess(GetCurrentProcess(), 134);
}
static void b_exit(int st)
{
    tl_log_line("bionic: the game called exit(%d)", st);
    if (tl_guest_exit_hook) tl_guest_exit_hook(st);
    ExitProcess((UINT)st);
}
static void b__exit(int st) { tl_log_line("bionic: the game called _exit(%d)", st); TerminateProcess(GetCurrentProcess(), (UINT)st); }
static void b___stack_chk_fail(void) { tl_log_line("bionic: stack protector tripped (return address %p)", __builtin_return_address(0)); b_abort(); }

static int b_getpid(void) { return (int)GetCurrentProcessId(); }
static int b_getppid(void) { return 1; }
static uint32_t b_geteuid(void) { return 10001; }
static int b_kill(int pid, int sig) { (void)pid; if (sig == 0) return 0; tl_log_line("bionic: kill(%d, %d) ignored", pid, sig); return 0; }
static int b_fail_enosys(void) { t_guest_errno = G_ENOSYS; return -1; }
static int b_fork(void) { tl_note_once("fork() is not available on Windows"); t_guest_errno = G_ENOSYS; return -1; }
static int b_waitpid(int pid, int *st, int opt) { (void)pid; (void)st; (void)opt; t_guest_errno = 10; /* ECHILD */ return -1; }
static int b_system(const char *cmd) { tl_log_line("bionic: system(%s) refused", cmd ? cmd : "NULL"); return cmd ? -1 : 0; }
static int64_t b_ptrace(int req, int pid, void *a, void *d) { (void)req; (void)pid; (void)a; (void)d; t_guest_errno = G_EPERM; return -1; }
static int b_prctl(int op, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    if (op == 15 /* PR_SET_NAME */ && a2) {
        /* the thread's name, for a debugger */
        wchar_t w[64]; MultiByteToWideChar(CP_UTF8, 0, (const char *)(uintptr_t)a2, -1, w, 64);
        SetThreadDescription(GetCurrentThread(), w);
    }
    return 0;
}

/* ------------------------------------------------------------- environment */

static int b_setenv(const char *k, const char *v, int overwrite)
{
    if (!overwrite && getenv(k)) return 0;
    return _putenv_s(k, v ? v : "") ? (t_guest_errno = G_EINVAL, -1) : 0;
}
static int b_unsetenv(const char *k) { return _putenv_s(k, "") ? -1 : 0; }

static int b_gethostname(char *buf, size_t n) { snprintf(buf, n, "localhost"); return 0; }

typedef struct { char *pw_name, *pw_passwd; uint32_t pw_uid, pw_gid; char *pw_gecos, *pw_dir, *pw_shell; } guest_passwd;
static guest_passwd g_pwd = { "u0_a1", NULL, 10001, 10001, NULL, "/data", "/system/bin/sh" };
static void *b_getpwuid(uint32_t uid) { (void)uid; return &g_pwd; }
static void *b_getpwnam(const char *n) { (void)n; return &g_pwd; }
static int b_getpwuid_r(uint32_t uid, void *pwd, char *buf, size_t n, void **result) { (void)uid; (void)buf; (void)n; memcpy(pwd, &g_pwd, sizeof(g_pwd)); *result = pwd; return 0; }
static void *b_getgr(void) { return NULL; }

/* ---------------------------------------------------------------- atexit */

/* The guest's exit-time destructors: kept and never run (Android never runs them for an app either: it is killed). */
static int b___cxa_atexit(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }
static void b___cxa_finalize(void *dso) { (void)dso; }
static int b___register_atfork(void (*a)(void), void (*b)(void), void (*c)(void), void *dso) { (void)a; (void)b; (void)c; (void)dso; return 0; }

/* thread_local destructors: run when the thread ends, newest first, through a pthread key's destructor. */
#include <pthread.h>
typedef struct tdtor { void (*fn)(void *); void *arg; struct tdtor *next; } tdtor;
static pthread_key_t g_tdtor_key;
static pthread_once_t g_tdtor_once = PTHREAD_ONCE_INIT;
static void tdtor_run(void *p)
{
    tdtor *d = p;
    while (d) { tdtor *n = d->next; d->fn(d->arg); free(d); d = n; }
}
static void tdtor_init(void) { pthread_key_create(&g_tdtor_key, tdtor_run); }
static int b___cxa_thread_atexit_impl(void (*fn)(void *), void *arg, void *dso)
{
    (void)dso;
    pthread_once(&g_tdtor_once, tdtor_init);
    tdtor *d = malloc(sizeof(*d));
    d->fn = fn; d->arg = arg; d->next = pthread_getspecific(g_tdtor_key);
    pthread_setspecific(g_tdtor_key, d);
    return 0;
}

/* ---------------------------------------------------------------- signals */

/* Nothing raises a POSIX signal on Windows: handlers are kept so the guest can ask for them back, and never run. */
typedef struct { void *handler; uint64_t mask; int flags; void *restorer; } guest_sigaction;   /* arm64 bionic: sa_handler, sa_mask, sa_flags, sa_restorer */
static guest_sigaction g_sigactions[65];
static int b_sigaction(int sig, const guest_sigaction *act, guest_sigaction *old)
{
    if (sig <= 0 || sig > 64) { t_guest_errno = G_EINVAL; return -1; }
    if (old) *old = g_sigactions[sig];
    if (act) g_sigactions[sig] = *act;
    return 0;
}
static void *b_signal(int sig, void *h)
{
    if (sig <= 0 || sig > 64) return (void *)-1;
    void *o = g_sigactions[sig].handler;
    g_sigactions[sig].handler = h;
    return o;
}
static int b_sigfillset(uint64_t *s) { *s = ~0ull; return 0; }
static int b_sigdelset(uint64_t *s, int sig) { if (sig > 0 && sig <= 64) *s &= ~(1ull << (sig - 1)); return 0; }
static int b_sigprocmask(int how, const uint64_t *set, uint64_t *old) { (void)how; (void)set; if (old) *old = 0; return 0; }
static int b_raise(int sig)
{
    tl_log_line("bionic: raise(%d)", sig);
    if (sig == 6 /* SIGABRT */) b_abort();
    return 0;
}

/* ---------------------------------------------------------- system calls */

/* Linux arm64 system call numbers the game's code makes directly (syscall() or a raw svc). */
long long tl_linux_syscall(long long a0, long long a1, long long a2, long long a3, long long a4, long long a5, long long nr)
{
    (void)a3; (void)a4; (void)a5;
    switch (nr) {
    case 178: return (long long)GetCurrentThreadId();                        /* gettid */
    case 172: return (long long)GetCurrentProcessId();                       /* getpid */
    case 278:                                                            /* getrandom */
        BCryptGenRandom(NULL, (PUCHAR)(uintptr_t)a0, (ULONG)a1, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        return a1;
    case 124: SwitchToThread(); return 0;                               /* sched_yield */
    case 98: {                                                           /* futex: WAIT (0) and WAKE (1), private or not */
        int op = (int)a1 & 127;
        if (op == 0) { int v = (int)a2; WaitOnAddress((volatile void *)(uintptr_t)a0, &v, 4, a3 ? 10 : INFINITE); return 0; }
        if (op == 1) { if (a2 == 1) WakeByAddressSingle((void *)(uintptr_t)a0); else WakeByAddressAll((void *)(uintptr_t)a0); return 0; }
        break;
    }
    default: break;
    }
    tl_log_line("bionic: Linux system call %lld (%#llx, %#llx, %#llx) not available", nr, a0, a1, a2);
    return -G_ENOSYS;
}
/* syscall() returns long: the stub's x0 is the whole 64-bit value the impl returns, so the impl returns long. */
long long tl_va_syscall_impl_l(tl_va_frame *f);
long long tl_va_syscall_impl_l(tl_va_frame *f)
{
    long long r = tl_linux_syscall((long long)f->gp[1], (long long)f->gp[2], (long long)f->gp[3], (long long)f->gp[4], (long long)f->gp[5], (long long)f->gp[6], (long long)f->gp[0]);
    if (r < 0 && r > -4096) { t_guest_errno = (int)-r; return -1; }
    return r;
}
TL_VA_STUB(tl_va_syscall, tl_va_syscall_impl_l);
extern void tl_va_syscall(void);

/* ------------------------------------------------------- system queries */

/* getauxval: AT_HWCAP (16) and AT_HWCAP2 (26) as an Apple M-series core reports them through Android's
 * kernel: fp asimd aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc dcpop
 * sha3 asimddp sha512 asimdfhm dit uscat ilrcpc flagm ssbs sb paca pacg; AT_PAGESZ (6) 4096. */
uint64_t tl_bionic_getauxval(uint64_t type);
uint64_t tl_bionic_getauxval(uint64_t type)
{
    switch (type) {
    case 16: return 0xEFFFFBull;
    case 26: return 0x2;
    case 6: return 4096;
    case 17: return 100;   /* AT_CLKTCK */
    default: return 0;
    }
}
static uint64_t b_getauxval(uint64_t type) { return tl_bionic_getauxval(type); }

/* sysconf: bionic's numbers for the names */
static int64_t b_sysconf(int name)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    switch (name) {
    case 0x0027: return 4096;                                  /* _SC_PAGESIZE / _SC_PAGE_SIZE */
    case 0x0028: return 4096;
    case 0x0060: case 0x0061: return si.dwNumberOfProcessors;  /* _SC_NPROCESSORS_CONF / _ONLN */
    case 0x0006: return 100;                                   /* _SC_CLK_TCK */
    case 0x0062: { MEMORYSTATUSEX m = { sizeof(m) }; GlobalMemoryStatusEx(&m); return (int64_t)(m.ullTotalPhys / 4096); }   /* _SC_PHYS_PAGES */
    case 0x0063: { MEMORYSTATUSEX m = { sizeof(m) }; GlobalMemoryStatusEx(&m); return (int64_t)(m.ullAvailPhys / 4096); }   /* _SC_AVPHYS_PAGES */
    case 0x0007: return 1024;                                  /* _SC_OPEN_MAX */
    case 0x0021: return 256;                                   /* _SC_GETPW_R_SIZE_MAX-ish */
    default: tl_log_line("bionic: sysconf(%#x) unknown, -1", name); t_guest_errno = G_EINVAL; return -1;
    }
}

static int b_getentropy(void *buf, size_t n) { return BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1; }

/* rand: bionic's RAND_MAX is 2^31-1 and the game's code divides by that constant, so Windows' 15-bit rand() will not do. */
static uint64_t g_rand_state = 1;
static int b_rand(void) { g_rand_state = g_rand_state * 6364136223846793005ull + 1442695040888963407ull; return (int)((g_rand_state >> 33) & 0x7fffffff); }
static void b_srand(unsigned s) { g_rand_state = s; }

/* ------------------------------------------------------ system properties */

/* What a phone answers for its properties (the Darwin runtime's table, husk-tl-bionic.c). */
static const struct { const char *k, *v; } k_props[] = {
    { "ro.build.version.sdk", "34" }, { "ro.build.version.release", "14" },
    { "ro.build.version.release_or_codename", "14" }, { "ro.build.version.codename", "REL" },
    { "ro.build.version.incremental", "11000000" }, { "ro.build.version.security_patch", "2024-05-05" },
    { "ro.build.id", "UP1A.231105.001" }, { "ro.build.display.id", "UP1A.231105.001" },
    { "ro.build.type", "user" }, { "ro.build.tags", "release-keys" },
    { "ro.build.fingerprint", "google/shiba/shiba:14/UP1A.231105.001/11000000:user/release-keys" },
    { "ro.product.manufacturer", "Google" }, { "ro.product.brand", "google" },
    { "ro.product.model", "Pixel 8" }, { "ro.product.name", "shiba" }, { "ro.product.device", "shiba" },
    { "ro.product.board", "shiba" }, { "ro.product.cpu.abi", "arm64-v8a" },
    { "ro.product.cpu.abilist", "arm64-v8a" }, { "ro.hardware", "shiba" }, { "ro.board.platform", "gs301" },
    { "ro.soc.manufacturer", "Google" }, { "ro.soc.model", "Tensor G3" },
    { "ro.opengles.version", "196610" }, { "ro.kernel.qemu", "0" }, { "ro.debuggable", "0" },
    { "ro.secure", "1" }, { "ro.zygote", "zygote64" }, { "ro.config.low_ram", "false" },
    { "ro.sf.lcd_density", "420" }, { "ro.hwui.use_vulkan", "" }, { "persist.sys.locale", "en-US" },
    { "persist.sys.timezone", "UTC" }, { "gsm.operator.iso-country", "us" }, { "ro.carrier", "unknown" },
    { "ro.com.google.clientidbase", "android-google" }, { "debug.hwui.renderer", "" },
    { "ro.vendor.build.fingerprint", "google/shiba/shiba:14/UP1A.231105.001/11000000:user/release-keys" },
    { "ro.system.build.version.sdk", "34" }, { "ro.product.first_api_level", "34" },
};

const char *tl_sysprop(const char *name)
{
    for (size_t i = 0; i < sizeof(k_props) / sizeof(k_props[0]); i++) if (!strcmp(k_props[i].k, name)) return k_props[i].v;
    return NULL;
}
static int b___system_property_get(const char *name, char *value)
{
    const char *v = name ? tl_sysprop(name) : NULL;
    if (!value) return 0;
    if (!v) { value[0] = 0; return 0; }
    snprintf(value, 92, "%s", v);
    return (int)strlen(value);
}
int tl_property_get(const char *name, char *value) { return b___system_property_get(name, value); }
size_t tl_property_list(char *buf, size_t n)
{
    size_t o = 0; buf[0] = 0;
    for (size_t i = 0; i < sizeof(k_props) / sizeof(k_props[0]) && o + 1 < n; i++)
        o += (size_t)snprintf(buf + o, n - o, "[%s]: [%s]\n", k_props[i].k, k_props[i].v);
    return o < n ? o : n - 1;
}
static const void *b___system_property_find(const char *name)
{
    for (size_t i = 0; i < sizeof(k_props) / sizeof(k_props[0]); i++) if (!strcmp(k_props[i].k, name)) return &k_props[i];
    return NULL;
}
static int b___system_property_read(const void *pi, char *name, char *value)
{
    if (!pi) return 0;
    const struct { const char *k, *v; } *p = pi;
    if (name) snprintf(name, 32, "%s", p->k);
    if (value) snprintf(value, 92, "%s", p->v);
    return (int)strlen(p->v);
}
void tl_thread_altstack(void) {}
void tl_http_install(void) {}

/* ------------------------------------------------------------------ table */

const tl_bionic_entry tl_tab_win_core[] = {
    TL_WRAP("__errno", b___errno),
    TL_WRAP("__system_property_get", b___system_property_get), TL_WRAP("__system_property_find", b___system_property_find),
    TL_WRAP("__system_property_read", b___system_property_read),
    TL_WRAP("dlopen", b_dlopen), TL_WRAP("dlsym", b_dlsym), TL_WRAP("dlclose", b_dlclose), TL_WRAP("dlerror", b_dlerror),
    TL_WRAP("dladdr", b_dladdr), TL_WRAP("dl_iterate_phdr", b_dl_iterate_phdr),
    TL_WRAP("__android_log_write", b___android_log_write), TL_WRAP("__android_log_print", tl_va_android_log_print),
    TL_WRAP("android_set_abort_message", b_android_set_abort_message),
    TL_WRAP("openlog", b_openlog), TL_WRAP("closelog", b_closelog), TL_WRAP("syslog", tl_va_syslog),
    TL_WRAP("abort", b_abort), TL_WRAP("exit", b_exit), TL_WRAP("_exit", b__exit), TL_WRAP("__stack_chk_fail", b___stack_chk_fail),
    TL_WRAP("getpid", b_getpid), TL_WRAP("getppid", b_getppid), TL_WRAP("geteuid", b_geteuid), TL_WRAP("kill", b_kill),
    TL_WRAP("fork", b_fork), TL_WRAP("execl", b_fail_enosys), TL_WRAP("execv", b_fail_enosys), TL_WRAP("waitpid", b_waitpid),
    TL_WRAP("system", b_system), TL_WRAP("ptrace", b_ptrace), TL_WRAP("prctl", b_prctl),
    TL_WRAP("getenv", getenv), TL_WRAP("setenv", b_setenv), TL_WRAP("unsetenv", b_unsetenv),
    TL_WRAP("gethostname", b_gethostname),
    TL_WRAP("getpwuid", b_getpwuid), TL_WRAP("getpwnam", b_getpwnam), TL_WRAP("getpwuid_r", b_getpwuid_r),
    TL_WRAP("getgrgid", b_getgr), TL_WRAP("getgrnam", b_getgr),
    TL_WRAP("__cxa_atexit", b___cxa_atexit), TL_WRAP("__cxa_finalize", b___cxa_finalize),
    TL_WRAP("__cxa_thread_atexit_impl", b___cxa_thread_atexit_impl),
    TL_WRAP("__register_atfork", b___register_atfork), TL_WRAP("pthread_atfork", b___register_atfork),
    TL_WRAP("sigaction", b_sigaction), TL_WRAP("signal", b_signal), TL_WRAP("sigfillset", b_sigfillset),
    TL_WRAP("sigdelset", b_sigdelset), TL_WRAP("sigprocmask", b_sigprocmask), TL_WRAP("raise", b_raise),
    TL_WRAP("syscall", tl_va_syscall), TL_WRAP("getauxval", b_getauxval), TL_WRAP("sysconf", b_sysconf),
    TL_WRAP("getentropy", b_getentropy), TL_WRAP("rand", b_rand), TL_WRAP("srand", b_srand),
    TL_END
};

/* Every table, searched in order. */
extern const tl_bionic_entry tl_tab_win_io[], tl_tab_win_thread[], tl_tab_win_str[], tl_tab_win_math[];
/* OpenSL ES (husk-tl-opensles.c), in the game but not in the start-up test: weak, and skipped when absent */
extern const tl_bionic_entry tl_tab_opensles[] __attribute__((weak));
static const tl_bionic_entry *k_tables[] = { tl_tab_win_core, tl_tab_win_io, tl_tab_win_thread, tl_tab_win_str, tl_tab_win_math, NULL };

typedef struct { const char *name; void *addr; } slot;
static slot *g_slots;
static size_t g_nslots;
static INIT_ONCE g_build_once = INIT_ONCE_STATIC_INIT;
static uint64_t fnv(const char *s) { uint64_t h = 1469598103934665603ull; while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ull; } return h; }

static BOOL CALLBACK build(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    k_tables[sizeof(k_tables) / sizeof(k_tables[0]) - 1] = tl_tab_opensles;
    size_t total = 0;
    for (size_t t = 0; t < sizeof(k_tables) / sizeof(k_tables[0]); t++) if (k_tables[t]) for (const tl_bionic_entry *e = k_tables[t]; e->name; e++) total++;
    g_nslots = 1;
    while (g_nslots < total * 2) g_nslots <<= 1;
    g_slots = calloc(g_nslots, sizeof(slot));
    for (size_t t = 0; t < sizeof(k_tables) / sizeof(k_tables[0]); t++) {
        if (!k_tables[t]) continue;
        for (const tl_bionic_entry *e = k_tables[t]; e->name; e++) {
            size_t i = fnv(e->name) & (g_nslots - 1);
            while (g_slots[i].name) {
                if (!strcmp(g_slots[i].name, e->name)) { tl_log_line("bionic: duplicate table entry %s", e->name); break; }
                i = (i + 1) & (g_nslots - 1);
            }
            g_slots[i].name = e->name;
            g_slots[i].addr = e->addr;
        }
    }
    return TRUE;
}

void *tl_egl_resolve(const char *name) __attribute__((weak));
void *tl_bionic_find(const char *name)
{
    InitOnceExecuteOnce(&g_build_once, build, NULL, NULL);
    size_t i = fnv(name) & (g_nslots - 1);
    while (g_slots[i].name) {
        if (!strcmp(g_slots[i].name, name)) return g_slots[i].addr;
        i = (i + 1) & (g_nslots - 1);
    }
    if (tl_egl_resolve && ((name[0] == 'g' && name[1] == 'l' && name[2] >= 'A' && name[2] <= 'Z') || !strncmp(name, "egl", 3)))
        return tl_egl_resolve(name);
    return NULL;
}
