/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Threads for the guest on Windows.
 *
 * The guest's pthread objects are bionic's (a mutex is 40 bytes, a condition 48, a
 * read-write lock 56, a semaphore 16) and live in the guest's memory, often
 * zero-initialised statically with no init call. Windows' slim locks fit that:
 * an SRWLOCK and a CONDITION_VARIABLE are one pointer each, and all-zero is their
 * initial state. So each object holds its Windows lock in place, no allocation:
 *
 *   mutex:   [0] bionic's type bits (recursive 0x4000, error-checking 0x8000, as the
 *            static initialisers write them), [8] SRWLOCK, [16] owner thread, [20] depth
 *   cond:    [0] CONDITION_VARIABLE
 *   rwlock:  [0] SRWLOCK, [8] the writing thread (unlock does not say which it releases)
 *   sem:     [0] count (for sem_getvalue), [8] a Windows semaphore, made by sem_init
 *
 * Threads themselves, keys and once are winpthreads' (pthread_t is a pointer-sized
 * integer there, which is what the guest's 64-bit pthread_t holds).
 */
#include "husk-tl-bionic-win.h"

#include <windows.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <stdlib.h>

#define E_BUSY 16
#define E_DEADLK 35
#define E_AGAIN 11
#define E_TIMEDOUT 110
#define E_INVAL 22
#define E_PERM 1

/* ------------------------------------------------------------------- time */

static int64_t now_realtime_ms(void)
{
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)((t - 116444736000000000ull) / 10000);
}
static DWORD ms_until(const g_timespec *abs)
{
    if (!abs) return INFINITE;
    int64_t when = abs->tv_sec * 1000 + abs->tv_nsec / 1000000;
    int64_t d = when - now_realtime_ms();
    return d <= 0 ? 0 : d > 0x7ffffffe ? 0x7ffffffe : (DWORD)d;
}

/* ------------------------------------------------------------------ mutex */

typedef struct { uint32_t type; uint32_t pad; SRWLOCK lock; uint32_t owner; uint32_t depth; uint8_t rest[16]; } g_mutex;
_Static_assert(sizeof(g_mutex) == 40, "bionic arm64 pthread_mutex_t");

static int mtype(const g_mutex *m) { return (int)((m->type >> 14) & 3); }

static int b_mutexattr_init(int64_t *a) { *a = 0; return 0; }
static int b_mutexattr_destroy(int64_t *a) { (void)a; return 0; }
static int b_mutexattr_settype(int64_t *a, int type) { if (type < 0 || type > 2) return E_INVAL; *a = (*a & ~0xfll) | type; return 0; }
static int b_mutexattr_setpshared(int64_t *a, int p) { (void)a; (void)p; return 0; }

static int b_mutex_init(g_mutex *m, const int64_t *attr)
{
    memset(m, 0, sizeof(*m));
    m->type = (uint32_t)((attr ? (*attr & 3) : 0) << 14);
    return 0;
}
static int b_mutex_destroy(g_mutex *m) { (void)m; return 0; }
static int b_mutex_lock(g_mutex *m)
{
    DWORD me = GetCurrentThreadId();
    int t = mtype(m);
    if (t != 0 && m->owner == me) {
        if (t == 1) { m->depth++; return 0; }
        return E_DEADLK;
    }
    AcquireSRWLockExclusive(&m->lock);
    m->owner = me; m->depth = 1;
    return 0;
}
static int b_mutex_trylock(g_mutex *m)
{
    DWORD me = GetCurrentThreadId();
    if (mtype(m) == 1 && m->owner == me) { m->depth++; return 0; }
    if (!TryAcquireSRWLockExclusive(&m->lock)) return E_BUSY;
    m->owner = me; m->depth = 1;
    return 0;
}
static int b_mutex_unlock(g_mutex *m)
{
    int t = mtype(m);
    if (t != 0 && m->owner != GetCurrentThreadId()) return E_PERM;
    if (t == 1 && m->depth > 1) { m->depth--; return 0; }
    m->owner = 0; m->depth = 0;
    ReleaseSRWLockExclusive(&m->lock);
    return 0;
}

/* -------------------------------------------------------------- condition */

typedef struct { CONDITION_VARIABLE cv; uint8_t rest[40]; } g_cond;
_Static_assert(sizeof(g_cond) == 48, "bionic arm64 pthread_cond_t");

static int b_condattr_init(int64_t *a) { *a = 0; return 0; }
static int b_condattr_destroy(int64_t *a) { (void)a; return 0; }
static int b_cond_init(g_cond *c, const int64_t *a) { (void)a; memset(c, 0, sizeof(*c)); return 0; }
static int b_cond_destroy(g_cond *c) { (void)c; return 0; }
static int b_cond_signal(g_cond *c) { WakeConditionVariable(&c->cv); return 0; }
static int b_cond_broadcast(g_cond *c) { WakeAllConditionVariable(&c->cv); return 0; }
static int cond_wait(g_cond *c, g_mutex *m, DWORD ms)
{
    uint32_t depth = m->depth;
    m->owner = 0; m->depth = 0;
    BOOL ok = SleepConditionVariableSRW(&c->cv, &m->lock, ms, 0);
    m->owner = GetCurrentThreadId(); m->depth = depth;
    return ok ? 0 : (GetLastError() == ERROR_TIMEOUT ? E_TIMEDOUT : E_INVAL);
}
static int b_cond_wait(g_cond *c, g_mutex *m) { return cond_wait(c, m, INFINITE); }
static int b_cond_timedwait(g_cond *c, g_mutex *m, const g_timespec *abs) { return cond_wait(c, m, ms_until(abs)); }

/* ----------------------------------------------------------------- rwlock */

typedef struct { SRWLOCK lock; uint32_t writer; uint8_t rest[44]; } g_rwlock;
_Static_assert(sizeof(g_rwlock) == 56, "bionic arm64 pthread_rwlock_t");
static int b_rwlock_init(g_rwlock *l, const void *a) { (void)a; memset(l, 0, sizeof(*l)); return 0; }
static int b_rwlock_destroy(g_rwlock *l) { (void)l; return 0; }
static int b_rwlock_rdlock(g_rwlock *l) { AcquireSRWLockShared(&l->lock); return 0; }
static int b_rwlock_wrlock(g_rwlock *l) { AcquireSRWLockExclusive(&l->lock); l->writer = GetCurrentThreadId(); return 0; }
static int b_rwlock_unlock(g_rwlock *l)
{
    if (l->writer == GetCurrentThreadId()) { l->writer = 0; ReleaseSRWLockExclusive(&l->lock); }
    else ReleaseSRWLockShared(&l->lock);
    return 0;
}

/* ------------------------------------------------------------------- once */

static int b_once(volatile int32_t *once, void (*fn)(void))
{
    /* 0: not run, 1: running, 2: done */
    if (InterlockedCompareExchange((volatile LONG *)once, 1, 0) == 0) {
        fn();
        InterlockedExchange((volatile LONG *)once, 2);
        WakeByAddressAll((void *)once);
        return 0;
    }
    int32_t v;
    while ((v = *once) != 2) { WaitOnAddress((volatile void *)once, &v, 4, 10); }
    return 0;
}

/* ------------------------------------------------------------------ threads */

typedef struct { uint32_t flags; uint32_t pad; void *stack_base; uint64_t stack_size, guard_size; int32_t policy, priority; uint8_t reserved[16]; } g_attr;
_Static_assert(sizeof(g_attr) == 56, "bionic arm64 pthread_attr_t");

static int b_attr_init(g_attr *a) { memset(a, 0, sizeof(*a)); a->stack_size = 1024 * 1024; a->guard_size = 4096; return 0; }
static int b_attr_destroy(g_attr *a) { (void)a; return 0; }
static int b_attr_setdetachstate(g_attr *a, int d) { if (d) a->flags |= 1; else a->flags &= ~1u; return 0; }
static int b_attr_setstacksize(g_attr *a, uint64_t n) { a->stack_size = n; return 0; }
static int b_attr_setstack(g_attr *a, void *base, uint64_t n) { a->stack_base = base; a->stack_size = n; return 0; }
static int b_attr_getstack(const g_attr *a, void **base, uint64_t *n) { *base = a->stack_base; *n = a->stack_size; return 0; }
typedef struct { int sched_priority; } g_sched_param;
static int b_attr_setschedparam(g_attr *a, const g_sched_param *p) { a->priority = p->sched_priority; return 0; }
static int b_attr_getschedparam(const g_attr *a, g_sched_param *p) { p->sched_priority = a->priority; return 0; }
static int b_attr_setschedpolicy(g_attr *a, int p) { a->policy = p; return 0; }

/* The calling thread's stack, as pthread_getattr_np reports it (the guest's own stack checks and libc++ read it). */
static int b_getattr_np(uint64_t thread, g_attr *a)
{
    (void)thread;
    b_attr_init(a);
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    a->stack_base = (void *)lo;
    a->stack_size = hi - lo;
    return 0;
}

typedef struct { void *(*fn)(void *); void *arg; } start_ctx;
static void *thread_start(void *p)
{
    start_ctx c = *(start_ctx *)p;
    free(p);
    return c.fn(c.arg);
}
static int b_create(uint64_t *out, const g_attr *ga, void *(*fn)(void *), void *arg)
{
    pthread_attr_t a;
    pthread_attr_init(&a);
    uint64_t ss = ga && ga->stack_size ? ga->stack_size : 1024 * 1024;
    if (ss < 256 * 1024) ss = 256 * 1024;   /* the guest's sizes are for Android; Windows code it calls needs room too */
    pthread_attr_setstacksize(&a, (size_t)ss);
    if (ga && (ga->flags & 1)) pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    start_ctx *c = malloc(sizeof(*c));
    c->fn = fn; c->arg = arg;
    pthread_t t;
    int r = pthread_create(&t, &a, thread_start, c);
    pthread_attr_destroy(&a);
    if (r) { free(c); return E_AGAIN; }
    *out = (uint64_t)t;
    return 0;
}
static int b_join(uint64_t t, void **ret) { return pthread_join((pthread_t)t, ret); }
static int b_detach(uint64_t t) { return pthread_detach((pthread_t)t); }
static uint64_t b_self(void) { return (uint64_t)pthread_self(); }
static int b_equal(uint64_t a, uint64_t b) { return a == b; }
static void b_exit(void *r) { pthread_exit(r); }
static int b_getschedparam(uint64_t t, int *policy, g_sched_param *p) { (void)t; *policy = 0; p->sched_priority = 0; return 0; }
static int b_setschedparam(uint64_t t, int policy, const g_sched_param *p) { (void)t; (void)policy; (void)p; return 0; }
static int b_sched_prio_max(int p) { (void)p; return 99; }
static int b_sched_prio_min(int p) { (void)p; return 0; }
static int b_sched_yield(void) { SwitchToThread(); return 0; }

static int b_key_create(int32_t *k, void (*d)(void *)) { pthread_key_t key; int r = pthread_key_create(&key, d); *k = (int32_t)key; return r; }
static int b_key_delete(int32_t k) { return pthread_key_delete((pthread_key_t)k); }
static void *b_getspecific(int32_t k) { return pthread_getspecific((pthread_key_t)k); }
static int b_setspecific(int32_t k, const void *v) { return pthread_setspecific((pthread_key_t)k, v); }

/* -------------------------------------------------------------- semaphores */

typedef struct { volatile LONG count; uint32_t pad; HANDLE h; } g_sem;
_Static_assert(sizeof(g_sem) == 16, "bionic arm64 sem_t");
int *tl_guest_errno_ptr(void);
static int b_sem_init(g_sem *s, int pshared, unsigned v)
{
    (void)pshared;
    s->count = (LONG)v;
    s->h = CreateSemaphoreA(NULL, (LONG)v, 0x7fffffff, NULL);
    return s->h ? 0 : (*tl_guest_errno_ptr() = 12, -1);
}
static int b_sem_destroy(g_sem *s) { if (s->h) CloseHandle(s->h); s->h = NULL; return 0; }
static int b_sem_post(g_sem *s) { InterlockedIncrement(&s->count); ReleaseSemaphore(s->h, 1, NULL); return 0; }
static int sem_wait_ms(g_sem *s, DWORD ms)
{
    DWORD r = WaitForSingleObject(s->h, ms);
    if (r == WAIT_OBJECT_0) { InterlockedDecrement(&s->count); return 0; }
    *tl_guest_errno_ptr() = ms == 0 ? E_AGAIN : E_TIMEDOUT;
    return -1;
}
static int b_sem_wait(g_sem *s) { return sem_wait_ms(s, INFINITE); }
static int b_sem_trywait(g_sem *s) { return sem_wait_ms(s, 0); }
static int b_sem_timedwait(g_sem *s, const g_timespec *abs) { return sem_wait_ms(s, ms_until(abs)); }
static int b_sem_getvalue(g_sem *s, int *v) { *v = (int)s->count; return 0; }

/* ------------------------------------------------------------------- table */

const tl_bionic_entry tl_tab_win_thread[] = {
    TL_WRAP("pthread_mutexattr_init", b_mutexattr_init), TL_WRAP("pthread_mutexattr_destroy", b_mutexattr_destroy),
    TL_WRAP("pthread_mutexattr_settype", b_mutexattr_settype), TL_WRAP("pthread_mutexattr_setpshared", b_mutexattr_setpshared),
    TL_WRAP("pthread_mutex_init", b_mutex_init), TL_WRAP("pthread_mutex_destroy", b_mutex_destroy),
    TL_WRAP("pthread_mutex_lock", b_mutex_lock), TL_WRAP("pthread_mutex_trylock", b_mutex_trylock),
    TL_WRAP("pthread_mutex_unlock", b_mutex_unlock),
    TL_WRAP("pthread_condattr_init", b_condattr_init), TL_WRAP("pthread_condattr_destroy", b_condattr_destroy),
    TL_WRAP("pthread_cond_init", b_cond_init), TL_WRAP("pthread_cond_destroy", b_cond_destroy),
    TL_WRAP("pthread_cond_signal", b_cond_signal), TL_WRAP("pthread_cond_broadcast", b_cond_broadcast),
    TL_WRAP("pthread_cond_wait", b_cond_wait), TL_WRAP("pthread_cond_timedwait", b_cond_timedwait),
    TL_WRAP("pthread_rwlock_init", b_rwlock_init), TL_WRAP("pthread_rwlock_destroy", b_rwlock_destroy),
    TL_WRAP("pthread_rwlock_rdlock", b_rwlock_rdlock), TL_WRAP("pthread_rwlock_wrlock", b_rwlock_wrlock),
    TL_WRAP("pthread_rwlock_unlock", b_rwlock_unlock),
    TL_WRAP("pthread_once", b_once),
    TL_WRAP("pthread_attr_init", b_attr_init), TL_WRAP("pthread_attr_destroy", b_attr_destroy),
    TL_WRAP("pthread_attr_setdetachstate", b_attr_setdetachstate), TL_WRAP("pthread_attr_setstacksize", b_attr_setstacksize),
    TL_WRAP("pthread_attr_setstack", b_attr_setstack), TL_WRAP("pthread_attr_getstack", b_attr_getstack),
    TL_WRAP("pthread_attr_setschedparam", b_attr_setschedparam), TL_WRAP("pthread_attr_getschedparam", b_attr_getschedparam),
    TL_WRAP("pthread_attr_setschedpolicy", b_attr_setschedpolicy), TL_WRAP("pthread_getattr_np", b_getattr_np),
    TL_WRAP("pthread_create", b_create), TL_WRAP("pthread_join", b_join), TL_WRAP("pthread_detach", b_detach),
    TL_WRAP("pthread_self", b_self), TL_WRAP("pthread_equal", b_equal), TL_WRAP("pthread_exit", b_exit),
    TL_WRAP("pthread_getschedparam", b_getschedparam), TL_WRAP("pthread_setschedparam", b_setschedparam),
    TL_WRAP("sched_get_priority_max", b_sched_prio_max), TL_WRAP("sched_get_priority_min", b_sched_prio_min),
    TL_WRAP("sched_yield", b_sched_yield),
    TL_WRAP("pthread_key_create", b_key_create), TL_WRAP("pthread_key_delete", b_key_delete),
    TL_WRAP("pthread_getspecific", b_getspecific), TL_WRAP("pthread_setspecific", b_setspecific),
    TL_WRAP("sem_init", b_sem_init), TL_WRAP("sem_destroy", b_sem_destroy), TL_WRAP("sem_post", b_sem_post),
    TL_WRAP("sem_wait", b_sem_wait), TL_WRAP("sem_trywait", b_sem_trywait), TL_WRAP("sem_timedwait", b_sem_timedwait),
    TL_WRAP("sem_getvalue", b_sem_getvalue),
    TL_END
};
