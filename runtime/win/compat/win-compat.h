/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Included first in every runtime file built for Windows (-include).
 *
 * The runtime is written against Darwin's POSIX. mingw-w64 (with winpthreads)
 * provides most of it; what it lacks, or what Windows does differently enough to
 * matter, is declared here and implemented in tl-win-compat.c.
 *
 * Memory the guest can free: Android code frees with free() what it got from
 * memalign/posix_memalign, and Windows' CRT cannot free an aligned block that
 * way. So every allocation the runtime or the guest makes goes through tl_malloc
 * and friends, which keep a header in front of each block; tl_free also takes a
 * plain CRT block (one a CRT function returned), told apart by the header's magic.
 */
#ifndef TL_WIN_COMPAT_H
#define TL_WIN_COMPAT_H
#ifdef _WIN32

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#ifdef __cplusplus
extern "C" {
#endif

void *tl_malloc(size_t n);
void *tl_calloc(size_t n, size_t m);
void *tl_realloc(void *p, size_t n);
void  tl_free(void *p);
int   tl_posix_memalign(void **out, size_t align, size_t n);
void *tl_memalign(size_t align, size_t n);
size_t tl_malloc_usable_size(const void *p);
char *tl_strdup(const char *s);
char *tl_strndup(const char *s, size_t n);
void *memmem(const void *h, size_t hl, const void *n, size_t nl);
void  tl_win_flush_icache(void *addr, size_t len);
/* Darwin's: the calling thread's stack top (its highest address) and size. */
void *tl_win_stack_top(void);
size_t tl_win_stack_size(void);
#define pthread_get_stackaddr_np(t) ((void)(t), tl_win_stack_top())
#define pthread_get_stacksize_np(t) ((void)(t), tl_win_stack_size())

/* Darwin calls the runtime makes, in Windows terms (tl-win-compat.c). pthread.h comes first so
 * that the one-argument pthread_setname_np below does not clash with winpthreads' declaration. */
#include <pthread.h>
#include <time.h>
void tl_win_arc4random_buf(void *buf, size_t n);
uint32_t tl_win_arc4random(void);
int tl_win_setname(const char *name);
#define arc4random_buf tl_win_arc4random_buf
#define arc4random tl_win_arc4random
#define arc4random_uniform(n) (tl_win_arc4random() % (n))
#define pthread_threadid_np(t, out) ((void)(t), *(out) = (uint64_t)tl_win_thread_id(), 0)
unsigned long tl_win_thread_id(void);
#undef pthread_setname_np
#define pthread_setname_np(n) tl_win_setname(n)
#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

/* POSIX mkdir takes a mode; Windows' has none. */
#include <io.h>
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#include <fcntl.h>
#define pipe(fds) _pipe((fds), 65536, _O_BINARY)
#define setenv(k, v, o) ((o) || !getenv(k) ? _putenv_s((k), (v)) : 0)
#define unsetenv(k) _putenv_s((k), "")
long long tl_win_pwrite(int fd, const void *buf, size_t n, long long off);
long long tl_win_pread(int fd, void *buf, size_t n, long long off);
#define pwrite tl_win_pwrite

/* Signals: Windows raises none. A SIGSEGV, SIGBUS or SIGILL handler is still honoured for the thread that
 * installed it: husk-tl-crash-win.c turns that thread's access violation (or misaligned access, or illegal
 * instruction) into a call of the handler, which is how the driver's fault guard around calls into the
 * game works. sigsetjmp saves registers only: Windows' own longjmp unwinds through frame tables, and the
 * game's code between the two has none. */
#include <signal.h>
#include <setjmp.h>
typedef struct { int si_signo, si_code; void *si_addr; } siginfo_t;
struct sigaction { union { void (*sa_handler)(int); void (*sa_sigaction)(int, siginfo_t *, void *); }; unsigned long long sa_mask; int sa_flags; };
#define SA_SIGINFO 4
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#ifndef SIGBUS
#define SIGBUS 7
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGUSR1
#define SIGUSR1 10
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
int tl_win_sigaction(int s, const struct sigaction *a, struct sigaction *o);
#define sigaction(s, a, o) tl_win_sigaction((s), (a), (o))
#define sigemptyset(m) (*(m) = 0, 0)
#define sigfillset(m) (*(m) = ~0ull, 0)
#define sigaddset(m, s) (*(m) |= 1ull << ((s) - 1), 0)
typedef unsigned long long sigset_t;
typedef unsigned long long sigjmp_buf[24];
int tl_win_setjmp(void *buf) __attribute__((returns_twice));
__attribute__((noreturn)) void tl_win_longjmp(void *buf, int v);
#define sigsetjmp(b, s) tl_win_setjmp(b)
#define siglongjmp(b, v) tl_win_longjmp((b), (v))
#define lstat stat
#define S_ISLNK(m) 0
#define localtime_r(t, out) (localtime_s((out), (t)) ? NULL : (out))
#define gmtime_r(t, out) (gmtime_s((out), (t)) ? NULL : (out))
int link(const char *from, const char *to);
#define fsync(fd) _commit(fd)
#include <stdio.h>
long long tl_win_getline(char **line, size_t *cap, FILE *f);
#define getline tl_win_getline
struct dirent;
int scandir(const char *dir, struct dirent ***list, int (*filter)(const struct dirent *), int (*cmp)(const struct dirent **, const struct dirent **));
int alphasort(const struct dirent **a, const struct dirent **b);
#define pread tl_win_pread

#ifndef TL_WIN_COMPAT_IMPL
#define malloc tl_malloc
#define calloc tl_calloc
#define realloc tl_realloc
#define free tl_free
#define posix_memalign tl_posix_memalign
#define memalign tl_memalign
#define strdup tl_strdup
#define strndup tl_strndup
#endif

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif
