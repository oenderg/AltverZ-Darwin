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
