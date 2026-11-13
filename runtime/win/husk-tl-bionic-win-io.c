/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Files for the guest on Windows: file descriptors (the CRT's), stdio (the CRT's
 * FILE, which the guest only ever holds a pointer to), directories, stat, the file
 * system's size, memory maps and the guest's zlib.
 *
 * Every path goes through tl_path_resolve. Files are always opened in binary mode:
 * Android has no text mode, and Windows' would rewrite line ends.
 */
#include "pvz2-profile.h"
#include "husk-tl-bionic-win.h"

#include <windows.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utime.h>
#include <sys/mman.h>

#include "husk-tl-va.h"

#define Z_PREFIX
#include "zlib-guest/zlib.h"

#define MAPPED(p) char p##_buf[1100]; const char *p##_m = tl_path_resolve(p, p##_buf, sizeof(p##_buf))
#define SET_ERRNO() tl_win_set_errno_from_crt()
int *tl_guest_errno_ptr(void);
#define GERR(e) (*tl_guest_errno_ptr() = (e))

/* ------------------------------------------------- made-up /proc files */

size_t tl_ld_maps(char *buf, size_t n, const char *libdir);

/* The contents of a /proc (or /sys) file the guest reads, or NULL for a real path. The mod
 * finds the game's base address in /proc/self/maps; engines size themselves by cpuinfo. */
static const char *synth_content(const char *path, char *buf, size_t n)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    if (!strcmp(path, "/proc/self/maps") || (!strncmp(path, "/proc/", 6) && strstr(path, "/maps") && !strcmp(strstr(path, "/maps"), "/maps"))) {
        tl_ld_maps(buf, n, "/data/app/~~husk==/" PVZ2_PROFILE_PKG "-1/lib/arm64");
        return buf;
    }
    if (!strcmp(path, "/proc/cpuinfo")) {
        buf[0] = 0;
        for (DWORD i = 0; i < si.dwNumberOfProcessors; i++) {
            size_t l = strlen(buf);
            snprintf(buf + l, n - l, "processor\t: %lu\nBogoMIPS\t: 48.00\nFeatures\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp\n"
                     "CPU implementer\t: 0x61\nCPU architecture: 8\nCPU variant\t: 0x0\nCPU part\t: 0x0\nCPU revision\t: 0\n\n", (unsigned long)i);
        }
        return buf;
    }
    if (!strcmp(path, "/proc/meminfo")) {
        MEMORYSTATUSEX m = { sizeof(m) }; GlobalMemoryStatusEx(&m);
        snprintf(buf, n, "MemTotal:       %llu kB\nMemFree:        %llu kB\nMemAvailable:   %llu kB\nBuffers:           10000 kB\nCached:          2000000 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n",
                 m.ullTotalPhys >> 10, m.ullAvailPhys >> 10, m.ullAvailPhys >> 10);
        return buf;
    }
    if (!strncmp(path, "/sys/devices/system/cpu/", 24) && (strstr(path, "/present") || strstr(path, "/possible") || strstr(path, "/online"))) {
        snprintf(buf, n, "0-%lu\n", (unsigned long)si.dwNumberOfProcessors - 1);
        return buf;
    }
    if (!strcmp(path, "/proc/self/status")) {
        snprintf(buf, n, "Name:\tapp_process64\nState:\tR (running)\nTgid:\t%lu\nPid:\t%lu\nPPid:\t1\nUid:\t10001\t10001\t10001\t10001\nTracerPid:\t0\nThreads:\t8\nVmRSS:\t  300000 kB\n",
                 GetCurrentProcessId(), GetCurrentProcessId());
        return buf;
    }
    return NULL;
}

/* A descriptor reading `content`: a temporary file that Windows deletes when it is closed. */
static int synth_fd(const char *content)
{
    char dir[1100], name[1200];
    snprintf(dir, sizeof(dir), "%s/cache", tl_data_dir());
    tl_win_mkdirs(dir);
    static volatile LONG seq;
    snprintf(name, sizeof(name), "%s/proc-%lu-%ld.txt", dir, GetCurrentProcessId(), InterlockedIncrement(&seq));
    HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD w = 0; WriteFile(h, content, (DWORD)strlen(content), &w, NULL);
    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    return _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY);
}
/* /dev/urandom and /dev/random: a descriptor on NUL that read() fills from the system's random source. */
#include <bcrypt.h>
static volatile LONG g_random_fd[16];
static bool is_random_fd(int fd) { for (int i = 0; i < 16; i++) if (g_random_fd[i] == fd + 1) return true; return false; }
static int dev_open(const char *path)
{
    if (!strcmp(path, "/dev/null")) return _open("NUL", _O_RDWR | _O_BINARY);
    if (strcmp(path, "/dev/urandom") && strcmp(path, "/dev/random")) return -1;
    int fd = _open("NUL", _O_RDONLY | _O_BINARY);
    if (fd < 0) return -1;
    for (int i = 0; i < 16; i++) if (InterlockedCompareExchange(&g_random_fd[i], fd + 1, 0) == 0) break;
    return fd;
}
static void forget_fd(int fd) { for (int i = 0; i < 16; i++) InterlockedCompareExchange(&g_random_fd[i], 0, fd + 1); }

static int synth_open(const char *path)
{
    if (!strncmp(path, "/dev/", 5)) return dev_open(path);
    if (strncmp(path, "/proc/", 6) && strncmp(path, "/sys/", 5)) return -1;
    static __thread char content[65536];
    const char *c = synth_content(path, content, sizeof(content));
    return c ? synth_fd(c) : -1;
}

/* ------------------------------------------------------------- the FILEs */

/* bionic's stdin, stdout and stderr are &__sF[0..2], 152 bytes apart. The guest takes their
 * addresses; every stdio entry turns one back into the CRT's own stream. */
#define G_FILE_SIZE 152
static uint8_t g_sF[3 * G_FILE_SIZE];
static FILE *gf(void *f)
{
    if ((uint8_t *)f >= g_sF && (uint8_t *)f < g_sF + sizeof(g_sF)) {
        size_t i = (size_t)((uint8_t *)f - g_sF) / G_FILE_SIZE;
        return i == 0 ? stdin : i == 1 ? stdout : stderr;
    }
    return f;
}

static void mode_binary(const char *mode, char *out, size_t n)
{
    size_t j = 0;
    for (const char *m = mode; *m && j + 2 < n; m++) if (*m != 'e' && *m != 'b' && *m != 't') out[j++] = *m;   /* 'e' is O_CLOEXEC */
    out[j++] = 'b'; out[j] = 0;
}

static void *b_fopen(const char *path, const char *mode)
{
    if (path && (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random"))) {
        /* a stream: a file of random bytes, which is as random to whoever reads it */
        static __thread char rnd[65537];
        BCryptGenRandom(NULL, (PUCHAR)rnd, 65536, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        for (int i = 0; i < 65536; i++) if (!rnd[i]) rnd[i] = 1;   /* synth_fd writes a C string */
        rnd[65536] = 0;
        int sfd = synth_fd(rnd);
        return sfd >= 0 ? _fdopen(sfd, "rb") : NULL;
    }
    if (path) {
        int sfd = synth_open(path);
        if (sfd >= 0) return _fdopen(sfd, "rb");
    }
    MAPPED(path);
    char m[16]; mode_binary(mode, m, sizeof(m));
    FILE *f = fopen(path_m, m);
    if (!f) SET_ERRNO();
    if (getenv("TL_FILE_TRACE")) tl_log_line("file: fopen(%s -> %s, %s) -> %s", path, path_m, mode, f ? "ok" : strerror(errno));
    return f;
}
static void *b_fdopen(int fd, const char *mode) { char m[16]; mode_binary(mode, m, sizeof(m)); FILE *f = _fdopen(fd, m); if (!f) SET_ERRNO(); return f; }
static int b_fclose(void *f) { FILE *h = gf(f); if (h == stdin || h == stdout || h == stderr) return 0; return fclose(h); }
static size_t b_fread(void *p, size_t s, size_t n, void *f) { return fread(p, s, n, gf(f)); }
static size_t b_fwrite(const void *p, size_t s, size_t n, void *f) { return fwrite(p, s, n, gf(f)); }
static int b_fflush(void *f) { return fflush(f ? gf(f) : NULL); }
static char *b_fgets(char *s, int n, void *f) { return fgets(s, n, gf(f)); }
static int b_fputs(const char *s, void *f) { return fputs(s, gf(f)); }
static int b_fputc(int c, void *f) { return fputc(c, gf(f)); }
static int b_fgetc(void *f) { return fgetc(gf(f)); }
static int b_ungetc(int c, void *f) { return ungetc(c, gf(f)); }
static int b_feof(void *f) { return feof(gf(f)); }
static int b_ferror(void *f) { return ferror(gf(f)); }
static void b_clearerr(void *f) { clearerr(gf(f)); }
static int b_fileno(void *f) { return _fileno(gf(f)); }
static int b_fseek(void *f, int64_t off, int whence) { return _fseeki64(gf(f), off, whence); }
static int64_t b_ftell(void *f) { return _ftelli64(gf(f)); }
static int b_fsetpos(void *f, const int64_t *pos) { return _fseeki64(gf(f), *pos, SEEK_SET); }
static void b_setbuf(void *f, char *buf) { setvbuf(gf(f), buf, buf ? _IOFBF : _IONBF, BUFSIZ); }
static int b_setvbuf(void *f, char *buf, int mode, size_t n)
{
    /* bionic: _IOFBF 0, _IOLBF 1, _IONBF 2; Windows: _IOFBF 0, _IOLBF 0x40, _IONBF 4 */
    int wm = mode == 2 ? _IONBF : mode == 1 ? _IOLBF : _IOFBF;
    if (wm != _IONBF && n < 2) n = 2;
    return setvbuf(gf(f), buf, wm, n);
}
static int b_fwide(void *f, int mode) { (void)f; return mode; }
static int b_putchar(int c) { return fputc(c, stdout); }
static int b_puts(const char *s) { return puts(s); }
static void b_perror(const char *s) { tl_log_line("perror: %s: errno %d", s ? s : "", *tl_guest_errno_ptr()); }

/* printf to a stream: formatted by the guest-ABI formatter, then written. */
static int write_formatted(FILE *f, const char *fmt, tl_va_list *ap)
{
    char small[1024];
    int n = tl_format(small, sizeof(small), fmt, ap);
    if (n < (int)sizeof(small)) return (int)fwrite(small, 1, (size_t)n, f);
    return n;   /* tl_format consumed the arguments; a second pass needs a copy taken first */
}
static int write_formatted_copy(FILE *f, const char *fmt, tl_va_list *ap)
{
    tl_va_list copy = *ap;
    char small[1024];
    int n = tl_format(small, sizeof(small), fmt, ap);
    if (n < (int)sizeof(small)) return (int)fwrite(small, 1, (size_t)n, f);
    char *big = malloc((size_t)n + 1);
    tl_format(big, (size_t)n + 1, fmt, &copy);
    fwrite(big, 1, (size_t)n, f);
    free(big);
    return n;
}
int tl_va_printf_impl(tl_va_frame *fr);
int tl_va_printf_impl(tl_va_frame *fr) { tl_va_list ap; tl_va_start(fr, 1, 0, &ap); return write_formatted_copy(stdout, (const char *)fr->gp[0], &ap); }
int tl_va_fprintf_impl(tl_va_frame *fr);
int tl_va_fprintf_impl(tl_va_frame *fr) { tl_va_list ap; tl_va_start(fr, 2, 0, &ap); return write_formatted_copy(gf((void *)fr->gp[0]), (const char *)fr->gp[1], &ap); }
TL_VA_STUB(tl_va_printf, tl_va_printf_impl);
TL_VA_STUB(tl_va_fprintf, tl_va_fprintf_impl);
extern void tl_va_printf(void), tl_va_fprintf(void);
static int b_vfprintf(void *f, const char *fmt, tl_va_list *ap) { return write_formatted_copy(gf(f), fmt, ap); }
static int b_vprintf(const char *fmt, tl_va_list *ap) { return write_formatted_copy(stdout, fmt, ap); }
static int (*unused_wf)(FILE *, const char *, tl_va_list *) = write_formatted;

/* fscanf: up to 32 pointer arguments, with the same %l -> %ll widening the string scanner does. */
static void widen_scan_format(const char *fmt, char *out, size_t n)
{
    size_t j = 0;
    for (const char *f = fmt; *f && j + 3 < n; f++) {
        out[j++] = *f;
        if (*f != '%') continue;
        if (f[1] == '%') { out[j++] = *++f; continue; }
        while (f[1] && strchr("*0123456789", f[1]) && j + 3 < n) out[j++] = *++f;
        if (f[1] == 'l' && f[2] != 'l' && f[2] && strchr("diouxXn", f[2])) { out[j++] = 'l'; out[j++] = 'l'; f++; }
    }
    out[j] = 0;
}
int tl_va_fscanf_impl(tl_va_frame *fr);
int tl_va_fscanf_impl(tl_va_frame *fr)
{
    tl_va_list ap; tl_va_start(fr, 2, 0, &ap);
    void *a[32];
    for (int i = 0; i < 32; i++) a[i] = (void *)(uintptr_t)tl_va_arg_u64(&ap);
    char wf[1024]; widen_scan_format((const char *)fr->gp[1], wf, sizeof(wf));
    return fscanf(gf((void *)fr->gp[0]), wf, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15],
                  a[16], a[17], a[18], a[19], a[20], a[21], a[22], a[23], a[24], a[25], a[26], a[27], a[28], a[29], a[30], a[31]);
}
TL_VA_STUB(tl_va_fscanf, tl_va_fscanf_impl);
extern void tl_va_fscanf(void);

/* ------------------------------------------------------------ descriptors */

/* Linux's open() flags, as the CRT's. */
static int win_oflags(int g)
{
    int w = _O_BINARY;
    switch (g & 3) { case 1: w |= _O_WRONLY; break; case 2: w |= _O_RDWR; break; default: w |= _O_RDONLY; break; }
    if (g & 0100) w |= _O_CREAT;
    if (g & 0200) w |= _O_EXCL;
    if (g & 01000) w |= _O_TRUNC;
    if (g & 02000) w |= _O_APPEND;
    return w;
}
static int is_dir(const char *p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }

int tl_va_open_impl(tl_va_frame *fr);
int tl_va_open_impl(tl_va_frame *fr)
{
    const char *path = (const char *)fr->gp[0];
    int flags = (int)fr->gp[1], mode = (int)fr->gp[2];
    { int sfd = path ? synth_open(path) : -1; if (sfd >= 0) return sfd; }
    MAPPED(path);
    if (is_dir(path_m)) {
        /* A directory: Windows' CRT cannot open one. Something to fsync or fstat; NUL stands in. */
        int fd = _open("NUL", _O_RDONLY | _O_BINARY);
        if (getenv("TL_FILE_TRACE")) tl_log_line("file: open(%s) is a directory -> %d", path, fd);
        return fd;
    }
    int fd = _open(path_m, win_oflags(flags), (mode & 0200) ? (_S_IREAD | _S_IWRITE) : _S_IREAD | _S_IWRITE);
    if (fd < 0) SET_ERRNO();
    if (getenv("TL_FILE_TRACE")) tl_log_line("file: open(%s -> %s, %#o) -> %d", path, path_m, flags, fd);
    return fd;
}
TL_VA_STUB(tl_va_open, tl_va_open_impl);
extern void tl_va_open(void);
static int b___open_2(const char *path, int flags)
{
    { int sfd = path ? synth_open(path) : -1; if (sfd >= 0) return sfd; }
    MAPPED(path);
    int fd = _open(path_m, win_oflags(flags), _S_IREAD | _S_IWRITE);
    if (fd < 0) SET_ERRNO();
    return fd;
}
static int b_close(int fd) { forget_fd(fd); int r = _close(fd); if (r) SET_ERRNO(); return r; }
static int64_t b_read(int fd, void *buf, size_t n) {
    if (is_random_fd(fd)) { BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG); return (int64_t)n; } int r = _read(fd, buf, n > 0x7fffffff ? 0x7fffffff : (unsigned)n); if (r < 0) SET_ERRNO(); return r; }
static int64_t b_write(int fd, const void *buf, size_t n)
{
    if (fd == 1 || fd == 2) { fwrite(buf, 1, n, fd == 1 ? stdout : stderr); return (int64_t)n; }
    int r = _write(fd, buf, n > 0x7fffffff ? 0x7fffffff : (unsigned)n);
    if (r < 0) SET_ERRNO();
    return r;
}
static int64_t b_lseek(int fd, int64_t off, int whence) { int64_t r = _lseeki64(fd, off, whence); if (r < 0) SET_ERRNO(); return r; }
static int b_dup2(int a, int b) { int r = _dup2(a, b); return r < 0 ? (SET_ERRNO(), -1) : b; }
static int b_fsync(int fd) { return _commit(fd); }
static int b_ftruncate(int fd, int64_t len) { int r = _chsize_s(fd, len); if (r) { GERR(tl_errno_to_guest(r)); return -1; } return 0; }
static int b_truncate(const char *path, int64_t len)
{
    MAPPED(path);
    int fd = _open(path_m, _O_RDWR | _O_BINARY);
    if (fd < 0) { SET_ERRNO(); return -1; }
    int r = b_ftruncate(fd, len);
    _close(fd);
    return r;
}
/* fcntl: descriptor flags mean nothing here; F_GETFL answers the access mode. */
int64_t tl_va_fcntl_impl(tl_va_frame *fr);
int64_t tl_va_fcntl_impl(tl_va_frame *fr)
{
    int cmd = (int)fr->gp[1];
    switch (cmd) {
    case 0: { int r = _dup((int)fr->gp[0]); return r; }   /* F_DUPFD */
    case 1: case 2: case 4: return 0;                     /* F_GETFD, F_SETFD, F_SETFL */
    case 3: return 2;                                     /* F_GETFL: O_RDWR */
    default: return 0;
    }
}
TL_VA_STUB(tl_va_fcntl, tl_va_fcntl_impl);
extern void tl_va_fcntl(void);
int tl_va_ioctl_impl(tl_va_frame *fr);
int tl_va_ioctl_impl(tl_va_frame *fr) { (void)fr; GERR(25 /* ENOTTY */); return -1; }
TL_VA_STUB(tl_va_ioctl, tl_va_ioctl_impl);
extern void tl_va_ioctl(void);
static int b_pipe(int fds[2]) { int r = _pipe(fds, 65536, _O_BINARY); if (r) SET_ERRNO(); return r; }
static int b_tcgetattr(int fd, void *t) { (void)fd; (void)t; GERR(25); return -1; }
static int b_poll(void *fds, uint64_t n, int timeout) { (void)fds; (void)n; if (timeout > 0) Sleep((DWORD)timeout); return 0; }
static int b_select(int n, void *r, void *w, void *e, g_timeval *tv)
{
    (void)n; (void)r; (void)w; (void)e;
    if (tv) Sleep((DWORD)(tv->tv_sec * 1000 + tv->tv_usec / 1000));
    return 0;
}
static int64_t b_sendfile(int out, int in, int64_t *off, size_t n)
{
    char buf[65536]; int64_t done = 0;
    if (off) _lseeki64(in, *off, SEEK_SET);
    while ((size_t)done < n) {
        int k = _read(in, buf, (unsigned)((n - (size_t)done) < sizeof(buf) ? (n - (size_t)done) : sizeof(buf)));
        if (k <= 0) break;
        _write(out, buf, (unsigned)k);
        done += k;
    }
    if (off) *off += done;
    return done;
}

/* --------------------------------------------------------------------- stat */

typedef struct {
    uint64_t st_dev, st_ino;
    uint32_t st_mode, st_nlink, st_uid, st_gid;
    uint64_t st_rdev, pad1;
    int64_t st_size;
    int32_t st_blksize, pad2;
    int64_t st_blocks;
    g_timespec st_atim, st_mtim, st_ctim;
    uint32_t unused4, unused5;
} g_stat;
_Static_assert(sizeof(g_stat) == 128, "bionic arm64 struct stat");

static void to_gstat(const struct _stat64 *s, g_stat *g)
{
    memset(g, 0, sizeof(*g));
    g->st_dev = (uint64_t)s->st_dev;
    g->st_ino = (uint64_t)s->st_ino;
    g->st_mode = (uint32_t)s->st_mode | ((s->st_mode & _S_IFDIR) ? 0755 : 0644);
    g->st_nlink = 1;
    g->st_uid = g->st_gid = 10001;
    g->st_size = s->st_size;
    g->st_blksize = 4096;
    g->st_blocks = (s->st_size + 511) / 512;
    g->st_atim.tv_sec = s->st_atime; g->st_mtim.tv_sec = s->st_mtime; g->st_ctim.tv_sec = s->st_ctime;
}
static int b_stat(const char *path, g_stat *g)
{
    MAPPED(path);
    struct _stat64 s;
    /* Windows' stat refuses a trailing slash on a directory */
    char t[1100]; snprintf(t, sizeof(t), "%s", path_m);
    size_t l = strlen(t); while (l > 3 && (t[l - 1] == '/' || t[l - 1] == '\\')) t[--l] = 0;
    if (_stat64(t, &s)) { SET_ERRNO(); if (getenv("TL_FILE_TRACE")) tl_log_line("file: stat(%s -> %s) failed", path, t); return -1; }
    to_gstat(&s, g);
    return 0;
}
static int b_fstat(int fd, g_stat *g)
{
    struct _stat64 s;
    if (_fstat64(fd, &s)) { SET_ERRNO(); return -1; }
    to_gstat(&s, g);
    return 0;
}

/* statfs and statvfs: the data folder's drive */
typedef struct { uint64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree; int32_t f_fsid[2]; uint64_t f_namelen, f_frsize, f_flags, f_spare[4]; } g_statfs;
typedef struct { uint64_t f_bsize, f_frsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax; uint32_t reserved[6]; } g_statvfs;
_Static_assert(sizeof(g_statfs) == 120, "bionic arm64 struct statfs");
_Static_assert(sizeof(g_statvfs) == 112, "bionic arm64 struct statvfs");
static void disk(const char *path, uint64_t *total, uint64_t *avail, uint64_t *free_)
{
    ULARGE_INTEGER a = {0}, t = {0}, f = {0};
    char root[1100]; snprintf(root, sizeof(root), "%s", path);
    if (!GetDiskFreeSpaceExA(root, &a, &t, &f)) GetDiskFreeSpaceExA(tl_data_dir(), &a, &t, &f);
    *total = t.QuadPart; *avail = a.QuadPart; *free_ = f.QuadPart;
}
static int b_statfs(const char *path, g_statfs *g)
{
    MAPPED(path);
    uint64_t t, a, f; disk(path_m, &t, &a, &f);
    memset(g, 0, sizeof(*g));
    g->f_type = 0xEF53; g->f_bsize = g->f_frsize = 4096;
    g->f_blocks = t / 4096; g->f_bfree = f / 4096; g->f_bavail = a / 4096;
    g->f_files = 1000000; g->f_ffree = 900000; g->f_namelen = 255;
    return 0;
}
static int b_statvfs(const char *path, g_statvfs *g)
{
    MAPPED(path);
    uint64_t t, a, f; disk(path_m, &t, &a, &f);
    memset(g, 0, sizeof(*g));
    g->f_bsize = g->f_frsize = 4096;
    g->f_blocks = t / 4096; g->f_bfree = f / 4096; g->f_bavail = a / 4096;
    g->f_files = 1000000; g->f_ffree = g->f_favail = 900000; g->f_namemax = 255;
    return 0;
}

/* ------------------------------------------------------- paths and folders */

static int b_access(const char *path, int mode)
{
    MAPPED(path);
    /* X_OK (1) has no meaning on Windows: existence stands in */
    int r = _access(path_m, mode & 6);
    if (r) SET_ERRNO();
    if (getenv("TL_FILE_TRACE")) tl_log_line("file: access(%s -> %s, %d) -> %d", path, path_m, mode, r);
    return r;
}
static int b_mkdir(const char *path, uint32_t mode)
{
    (void)mode;
    MAPPED(path);
    int r = _mkdir(path_m);
    if (r) SET_ERRNO();
    return r;
}
static int b_rmdir(const char *path) { MAPPED(path); int r = _rmdir(path_m); if (r) SET_ERRNO(); return r; }
static int b_unlink(const char *path) { MAPPED(path); int r = _unlink(path_m); if (r) SET_ERRNO(); return r; }
static int b_remove(const char *path) { MAPPED(path); int r = is_dir(path_m) ? _rmdir(path_m) : _unlink(path_m); if (r) SET_ERRNO(); return r; }
static int b_rename(const char *from, const char *to)
{
    MAPPED(from); MAPPED(to);
    /* POSIX rename replaces the target; Windows' does not unless asked */
    if (!MoveFileExA(from_m, to_m, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) { tl_win_set_errno_from_win32(GetLastError()); return -1; }
    return 0;
}
static int b_chdir(const char *path) { MAPPED(path); int r = _chdir(path_m); if (r) SET_ERRNO(); return r; }
static char *b_getcwd(char *buf, size_t n)
{
    char t[1100];
    if (!_getcwd(t, sizeof(t))) { SET_ERRNO(); return NULL; }
    for (char *p = t; *p; p++) if (*p == '\\') *p = '/';
    if (!buf) return tl_strdup(t);
    if (strlen(t) + 1 > n) { GERR(G_ERANGE); return NULL; }
    strcpy(buf, t);
    return buf;
}
static char *b_realpath(const char *path, char *out)
{
    MAPPED(path);
    char t[1100];
    if (!GetFullPathNameA(path_m, sizeof(t), t, NULL)) { tl_win_set_errno_from_win32(GetLastError()); return NULL; }
    if (GetFileAttributesA(t) == INVALID_FILE_ATTRIBUTES) { GERR(G_ENOENT); return NULL; }
    for (char *p = t; *p; p++) if (*p == '\\') *p = '/';
    if (!out) return tl_strdup(t);
    snprintf(out, 4096, "%s", t);
    return out;
}
static int64_t b_readlink(const char *path, char *buf, size_t n)
{
    if (!strcmp(path, "/proc/self/exe")) {
        const char *e = "/system/bin/app_process64";
        size_t l = strlen(e); if (l > n) l = n;
        memcpy(buf, e, l);
        return (int64_t)l;
    }
    GERR(G_EINVAL);
    return -1;
}
static int b_chmod(const char *path, uint32_t mode) { (void)path; (void)mode; return 0; }
static int b_fchmod(int fd, uint32_t mode) { (void)fd; (void)mode; return 0; }
static int b_fchmodat(int dfd, const char *path, uint32_t mode, int fl) { (void)dfd; (void)path; (void)mode; (void)fl; return 0; }
static int b_lchown(const char *p, uint32_t u, uint32_t g) { (void)p; (void)u; (void)g; return 0; }
static int b_link(const char *a, const char *b) { MAPPED(a); MAPPED(b); return CopyFileA(a_m, b_m, TRUE) ? 0 : (tl_win_set_errno_from_win32(GetLastError()), -1); }
static int b_symlink(const char *a, const char *b) { (void)a; (void)b; GERR(G_EPERM); return -1; }
static int b_mkfifo(const char *p, uint32_t m) { (void)p; (void)m; GERR(G_EPERM); return -1; }
static int b_mknod(const char *p, uint32_t m, uint64_t d) { (void)p; (void)m; (void)d; GERR(G_EPERM); return -1; }
static int64_t b_pathconf(const char *p, int name) { (void)p; return name == 3 /* _PC_NAME_MAX */ ? 255 : 4096; }
typedef struct { int64_t actime, modtime; } g_utimbuf;
static int b_utime(const char *path, const g_utimbuf *t)
{
    MAPPED(path);
    struct __utimbuf64 u;
    if (t) { u.actime = t->actime; u.modtime = t->modtime; }
    int r = _utime64(path_m, t ? &u : NULL);
    if (r) SET_ERRNO();
    return r;
}
static int b_utimensat(int dfd, const char *path, const g_timespec *ts, int flags)
{
    (void)dfd; (void)flags;
    if (!path) return 0;
    g_utimbuf u = { ts ? ts[0].tv_sec : 0, ts ? ts[1].tv_sec : 0 };
    return b_utime(path, ts ? &u : NULL);
}

static char *b_basename(const char *p)
{
    static __thread char out[1100];
    if (!p || !*p) return ".";
    snprintf(out, sizeof(out), "%s", p);
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;
    char *s = strrchr(out, '/');
    return s && s[1] ? s + 1 : out;
}
static char *b_dirname(const char *p)
{
    static __thread char out[1100];
    if (!p || !*p) return ".";
    snprintf(out, sizeof(out), "%s", p);
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;
    char *s = strrchr(out, '/');
    if (!s) return ".";
    if (s == out) { out[1] = 0; return out; }
    *s = 0;
    return out;
}

/* ------------------------------------------------------------- directories */

typedef struct { uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256]; } g_dirent;
typedef struct { HANDLE h; WIN32_FIND_DATAA fd; int first, done; g_dirent ent; int64_t pos; } g_dir;
static void *b_opendir(const char *path)
{
    MAPPED(path);
    char pat[1110]; snprintf(pat, sizeof(pat), "%s/*", path_m);
    g_dir *d = calloc(1, sizeof(*d));
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) {
        tl_win_set_errno_from_win32(GetLastError());
        free(d);
        if (getenv("TL_FILE_TRACE")) tl_log_line("file: opendir(%s -> %s) failed", path, path_m);
        return NULL;
    }
    d->first = 1;
    return d;
}
static g_dirent *b_readdir(void *dp)
{
    g_dir *d = dp;
    if (!d || d->done) return NULL;
    if (!d->first && !FindNextFileA(d->h, &d->fd)) { d->done = 1; return NULL; }
    d->first = 0;
    memset(&d->ent, 0, sizeof(d->ent));
    d->ent.d_ino = (uint64_t)++d->pos;
    d->ent.d_off = d->pos;
    d->ent.d_reclen = sizeof(d->ent);
    d->ent.d_type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8;
    snprintf(d->ent.d_name, sizeof(d->ent.d_name), "%s", d->fd.cFileName);
    return &d->ent;
}
static int b_closedir(void *dp) { g_dir *d = dp; if (!d) return -1; FindClose(d->h); free(d); return 0; }

/* -------------------------------------------------------------------- mmap */

static void *b_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off)
{
    void *p = mmap(addr, len, prot, flags, fd, off);
    if (p == MAP_FAILED) SET_ERRNO();
    return p;
}
static int b_munmap(void *a, size_t n) { return munmap(a, n); }
/* Inside the loader's region (the game's code, data and the mod's trampolines) memory stays
 * readable, writable and executable whatever the guest asks: the mod writes branches into
 * the game's code and runs trampolines it builds in its own .bss, and one region keeps both
 * possible. A change there only needs the instruction cache told. */
bool tl_xmem_contains(const void *p);
static int b_mprotect(void *a, size_t n, int prot)
{
    if (tl_xmem_contains(a)) { tl_win_flush_icache(a, n); return 0; }
    int r = mprotect(a, n, prot);
    if (r) SET_ERRNO();
    return r;
}
static int b_madvise(void *a, size_t n, int adv) { (void)a; (void)n; (void)adv; return 0; }
static int b_mlock(const void *a, size_t n) { (void)a; (void)n; return 0; }

/* -------------------------------------------------------------------- zlib */

/* gz files take a path: mapped like any other. The rest of zlib is the guest build (uLong is 64 bits, as on Android). */
static void *b_gzopen(const char *path, const char *mode) { MAPPED(path); return z_gzopen(path_m, mode); }

/* ------------------------------------------------------------------- table */

const tl_bionic_entry tl_tab_win_io[] = {
    TL_DATA("__sF", g_sF),
    TL_WRAP("fopen", b_fopen), TL_WRAP("fdopen", b_fdopen), TL_WRAP("fclose", b_fclose), TL_WRAP("fread", b_fread),
    TL_WRAP("fwrite", b_fwrite), TL_WRAP("fflush", b_fflush), TL_WRAP("fgets", b_fgets), TL_WRAP("fputs", b_fputs),
    TL_WRAP("fputc", b_fputc), TL_WRAP("fgetc", b_fgetc), TL_WRAP("getc", b_fgetc), TL_WRAP("ungetc", b_ungetc),
    TL_WRAP("feof", b_feof), TL_WRAP("ferror", b_ferror), TL_WRAP("clearerr", b_clearerr), TL_WRAP("fileno", b_fileno),
    TL_WRAP("fseek", b_fseek), TL_WRAP("fseeko", b_fseek), TL_WRAP("ftell", b_ftell), TL_WRAP("ftello", b_ftell),
    TL_WRAP("fsetpos", b_fsetpos), TL_WRAP("setbuf", b_setbuf), TL_WRAP("setvbuf", b_setvbuf), TL_WRAP("fwide", b_fwide),
    TL_WRAP("putchar", b_putchar), TL_WRAP("puts", b_puts), TL_WRAP("perror", b_perror),
    TL_WRAP("printf", tl_va_printf), TL_WRAP("fprintf", tl_va_fprintf), TL_WRAP("vfprintf", b_vfprintf), TL_WRAP("vprintf", b_vprintf),
    TL_WRAP("fscanf", tl_va_fscanf),
    TL_WRAP("open", tl_va_open), TL_WRAP("__open_2", b___open_2), TL_WRAP("close", b_close), TL_WRAP("read", b_read),
    TL_WRAP("write", b_write), TL_WRAP("lseek", b_lseek), TL_WRAP("dup2", b_dup2), TL_WRAP("fsync", b_fsync),
    TL_WRAP("ftruncate", b_ftruncate), TL_WRAP("truncate", b_truncate), TL_WRAP("fcntl", tl_va_fcntl), TL_WRAP("ioctl", tl_va_ioctl),
    TL_WRAP("pipe", b_pipe), TL_WRAP("tcgetattr", b_tcgetattr), TL_WRAP("tcsetattr", b_tcgetattr), TL_WRAP("poll", b_poll),
    TL_WRAP("select", b_select), TL_WRAP("sendfile", b_sendfile),
    TL_WRAP("stat", b_stat), TL_WRAP("lstat", b_stat), TL_WRAP("fstat", b_fstat), TL_WRAP("statfs", b_statfs), TL_WRAP("statvfs", b_statvfs),
    TL_WRAP("access", b_access), TL_WRAP("mkdir", b_mkdir), TL_WRAP("rmdir", b_rmdir), TL_WRAP("unlink", b_unlink),
    TL_WRAP("remove", b_remove), TL_WRAP("rename", b_rename), TL_WRAP("chdir", b_chdir), TL_WRAP("getcwd", b_getcwd),
    TL_WRAP("realpath", b_realpath), TL_WRAP("readlink", b_readlink), TL_WRAP("chmod", b_chmod), TL_WRAP("fchmod", b_fchmod),
    TL_WRAP("fchmodat", b_fchmodat), TL_WRAP("lchown", b_lchown), TL_WRAP("link", b_link), TL_WRAP("symlink", b_symlink),
    TL_WRAP("mkfifo", b_mkfifo), TL_WRAP("mknod", b_mknod), TL_WRAP("pathconf", b_pathconf), TL_WRAP("utime", b_utime),
    TL_WRAP("utimensat", b_utimensat), TL_WRAP("basename", b_basename), TL_WRAP("dirname", b_dirname),
    TL_WRAP("opendir", b_opendir), TL_WRAP("readdir", b_readdir), TL_WRAP("closedir", b_closedir),
    TL_WRAP("mmap", b_mmap), TL_WRAP("munmap", b_munmap), TL_WRAP("mprotect", b_mprotect), TL_WRAP("madvise", b_madvise), TL_WRAP("mlock", b_mlock),
    TL_WRAP("gzopen", b_gzopen), TL_WRAP("gzread", z_gzread), TL_WRAP("gzclose", z_gzclose),
    TL_WRAP("adler32", z_adler32), TL_WRAP("crc32", z_crc32), TL_WRAP("deflate", z_deflate), TL_WRAP("deflateEnd", z_deflateEnd),
    TL_WRAP("deflateInit2_", z_deflateInit2_), TL_WRAP("deflateInit_", z_deflateInit_), TL_WRAP("deflateReset", z_deflateReset),
    TL_WRAP("inflate", z_inflate), TL_WRAP("inflateEnd", z_inflateEnd), TL_WRAP("inflateInit2_", z_inflateInit2_),
    TL_WRAP("inflateInit_", z_inflateInit_), TL_WRAP("inflateReset", z_inflateReset), TL_WRAP("zlibVersion", z_zlibVersion),
    TL_WRAP("compress", z_compress), TL_WRAP("compressBound", z_compressBound), TL_WRAP("uncompress", z_uncompress),
    TL_END
};
