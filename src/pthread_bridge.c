/*
 * pthread_bridge.c -- bionic synchronisation objects on top of glibc's.
 *
 * The objects are smaller in bionic than in glibc on arm64
 * (mutex 40 vs 48, rwlock 56 vs 56 but laid out differently, sem 4 vs 32,
 * pthread_attr_t 56 vs 64), so letting the game hand its storage straight to
 * glibc corrupts whatever follows it.  Every object is therefore treated as a
 * handle: we keep a real glibc object on the heap and store its address inside
 * the bionic storage, which is always at least 16 bytes and whose static
 * initialisers only ever touch the first word.
 *
 * Two behaviours here are deliberate, both learned the hard way on other ports:
 *   - cond_wait waits with a ceiling and loops.  POSIX allows spurious
 *     wakeups, so this is semantically free, and it means a lost wakeup shows
 *     up as a slow frame instead of a permanent freeze.
 *   - sem_init on an already-live semaphore keeps the existing object instead
 *     of making a fresh one, so a post that arrived before the init is not
 *     dropped.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "nx_elf.h"
#include "bc.h"

#define BRIDGE_MAGIC 0x50463242u   /* "HGOB" */

/* Bionic's storage as we use it: [0]=original word, [1]=magic, [2..3]=pointer. */
typedef struct {
    uint32_t word0;
    uint32_t magic;
    void *real;
} bhandle;

static pthread_mutex_t bridge_lock = PTHREAD_MUTEX_INITIALIZER;

static void *slot(void *obj, size_t real_size, void (*init)(void *, uint32_t))
{
    bhandle *h = obj;
    if (h->magic == BRIDGE_MAGIC && h->real)
        return h->real;
    pthread_mutex_lock(&bridge_lock);
    if (!(h->magic == BRIDGE_MAGIC && h->real)) {
        void *r = calloc(1, real_size);
        if (init)
            init(r, h->word0);
        h->real = r;
        __atomic_store_n(&h->magic, BRIDGE_MAGIC, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&bridge_lock);
    return h->real;
}

/* ------------------------------------------------------------------ mutex */

/* bionic packs the type into bits 14-15 of the first word, which is how a
 * statically initialised recursive mutex reaches us. */
#define BIONIC_MUTEX_TYPE(w) (((w) >> 14) & 3)

static void mutex_init(void *r, uint32_t w)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    switch (BIONIC_MUTEX_TYPE(w)) {
    case 1: pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); break;
    case 2: pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK); break;
    default: pthread_mutexattr_settype(&a, PTHREAD_MUTEX_NORMAL); break;
    }
    pthread_mutex_init(r, &a);
    pthread_mutexattr_destroy(&a);
}

static pthread_mutex_t *M(void *o) { return slot(o, sizeof(pthread_mutex_t), mutex_init); }

static int b_mutex_init(void *o, const void *attr)
{
    bhandle *h = o;
    /* bionic's pthread_mutexattr_t is a plain int holding the same type bits. */
    h->word0 = attr ? (uint32_t)(*(const int *)attr) : 0;
    h->magic = 0;
    h->real = NULL;
    (void)M(o);
    return 0;
}
static int b_mutex_destroy(void *o)
{
    bhandle *h = o;
    if (h->magic == BRIDGE_MAGIC && h->real) {
        pthread_mutex_destroy(h->real);
        free(h->real);
        h->real = NULL;
        h->magic = 0;
    }
    return 0;
}
static int b_mutex_lock(void *o)    { return pthread_mutex_lock(M(o)); }
static int b_mutex_unlock(void *o)  { return pthread_mutex_unlock(M(o)); }
static int b_mutex_trylock(void *o) { return pthread_mutex_trylock(M(o)); }
static int b_mutex_timedlock(void *o, const struct timespec *ts)
{
    return pthread_mutex_timedlock(M(o), ts);
}

static int b_mutexattr_init(int *a) { if (a) *a = 0; return 0; }
static int b_mutexattr_destroy(int *a) { (void)a; return 0; }
static int b_mutexattr_settype(int *a, int t)
{
    if (a)
        *a = (t & 3) << 14;
    return 0;
}
static int b_mutexattr_gettype(const int *a, int *t)
{
    if (t)
        *t = a ? BIONIC_MUTEX_TYPE((uint32_t)*a) : 0;
    return 0;
}
static int b_mutexattr_setpshared(int *a, int s) { (void)a; (void)s; return 0; }

/* ------------------------------------------------------------------- cond */

typedef struct { pthread_cond_t c; int monotonic; } bcond;

static void cond_init_cb(void *r, uint32_t w)
{
    bcond *b = r;
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    /* bionic encodes CLOCK_MONOTONIC in bit 0 of the first word. */
    b->monotonic = (w & 1) == 0;
    pthread_condattr_setclock(&a, b->monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME);
    pthread_cond_init(&b->c, &a);
    pthread_condattr_destroy(&a);
}

static bcond *C(void *o) { return slot(o, sizeof(bcond), cond_init_cb); }

static int b_cond_init(void *o, const void *attr)
{
    bhandle *h = o;
    h->word0 = attr ? (uint32_t)(*(const int *)attr) : 0;
    h->magic = 0;
    h->real = NULL;
    (void)C(o);
    return 0;
}
static int b_cond_destroy(void *o)
{
    bhandle *h = o;
    if (h->magic == BRIDGE_MAGIC && h->real) {
        pthread_cond_destroy(h->real);
        free(h->real);
        h->real = NULL;
        h->magic = 0;
    }
    return 0;
}
static int b_cond_signal(void *o)    { return pthread_cond_signal(&C(o)->c); }
static int b_cond_broadcast(void *o) { return pthread_cond_broadcast(&C(o)->c); }

/* 50 ms ceiling: long enough not to spin, short enough that a lost wakeup is a
 * hitch rather than a hang.  The caller re-checks its predicate either way. */
static int b_cond_wait(void *o, void *m)
{
    bcond *b = C(o);
    struct timespec ts;
    clock_gettime(b->monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
    ts.tv_nsec += 50000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_nsec -= 1000000000L;
        ts.tv_sec++;
    }
    int r = pthread_cond_timedwait(&b->c, M(m), &ts);
    return r == ETIMEDOUT ? 0 : r;
}

static int b_cond_timedwait(void *o, void *m, const struct timespec *ts)
{
    return pthread_cond_timedwait(&C(o)->c, M(m), ts);
}

static int b_condattr_init(int *a) { if (a) *a = 0; return 0; }
static int b_condattr_destroy(int *a) { (void)a; return 0; }
static int b_condattr_setclock(int *a, int clk)
{
    if (a)
        *a = (clk == CLOCK_REALTIME) ? 1 : 0;
    return 0;
}
static int b_condattr_setpshared(int *a, int s) { (void)a; (void)s; return 0; }

/* ----------------------------------------------------------------- rwlock */

static void rw_init_cb(void *r, uint32_t w) { (void)w; pthread_rwlock_init(r, NULL); }
static pthread_rwlock_t *R(void *o) { return slot(o, sizeof(pthread_rwlock_t), rw_init_cb); }

static int b_rwlock_init(void *o, const void *a)
{
    bhandle *h = o;
    (void)a;
    h->word0 = 0;
    h->magic = 0;
    h->real = NULL;
    (void)R(o);
    return 0;
}
static int b_rwlock_destroy(void *o)
{
    bhandle *h = o;
    if (h->magic == BRIDGE_MAGIC && h->real) {
        pthread_rwlock_destroy(h->real);
        free(h->real);
        h->real = NULL;
        h->magic = 0;
    }
    return 0;
}
static int b_rwlock_rdlock(void *o)    { return pthread_rwlock_rdlock(R(o)); }
static int b_rwlock_wrlock(void *o)    { return pthread_rwlock_wrlock(R(o)); }
static int b_rwlock_unlock(void *o)    { return pthread_rwlock_unlock(R(o)); }
static int b_rwlock_tryrdlock(void *o) { return pthread_rwlock_tryrdlock(R(o)); }
static int b_rwlock_trywrlock(void *o) { return pthread_rwlock_trywrlock(R(o)); }

/* -------------------------------------------------------------- semaphore */

/* bionic's sem_t is a single int, so there is no room for a handle: keep a
 * small table keyed by the address the game gave us. */
#define SEM_MAX 512
static struct { void *key; sem_t *s; } sems[SEM_MAX];
static int sem_n;

static sem_t *S(void *o, unsigned initial, int creating)
{
    pthread_mutex_lock(&bridge_lock);
    for (int i = 0; i < sem_n; i++)
        if (sems[i].key == o) {
            sem_t *s = sems[i].s;
            if (creating) {
                /* Do not drop posts that arrived before this init. */
                int cur = 0;
                sem_getvalue(s, &cur);
                for (unsigned k = (unsigned)cur; k < initial; k++)
                    sem_post(s);
            }
            pthread_mutex_unlock(&bridge_lock);
            return s;
        }
    sem_t *s = calloc(1, sizeof *s);
    sem_init(s, 0, initial);
    if (sem_n < SEM_MAX) {
        sems[sem_n].key = o;
        sems[sem_n].s = s;
        sem_n++;
    } else {
        nx_log("semaphore table full");
    }
    pthread_mutex_unlock(&bridge_lock);
    return s;
}

static int b_sem_init(void *o, int pshared, unsigned v) { (void)pshared; S(o, v, 1); return 0; }
static int b_sem_destroy(void *o) { (void)o; return 0; }   /* keep the mapping alive */
static int b_sem_post(void *o)    { return sem_post(S(o, 0, 0)); }
static int b_sem_wait(void *o)
{
    sem_t *s = S(o, 0, 0);
    int r;
    do { r = sem_wait(s); } while (r == -1 && errno == EINTR);
    return r;
}
static int b_sem_trywait(void *o) { return sem_trywait(S(o, 0, 0)); }
static int b_sem_timedwait(void *o, const struct timespec *ts)
{
    return sem_timedwait(S(o, 0, 0), ts);
}
static int b_sem_getvalue(void *o, int *v) { return sem_getvalue(S(o, 0, 0), v); }

/* -------------------------------------------------------------------- attr */

/* bionic pthread_attr_t: { uint32 flags; void *stack_base; size_t stack_size;
 *                          size_t guard_size; int32 sched_policy;
 *                          int32 sched_priority; } == 40 bytes used of 56. */
typedef struct {
    uint32_t flags;
    void *stack_base;
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
} bionic_attr;

#define BIONIC_ATTR_DETACHED 1

static int b_attr_init(void *a)
{
    bionic_attr *b = a;
    memset(b, 0, sizeof *b);
    b->stack_size = 1024 * 1024;
    b->guard_size = (size_t)getpagesize();
    return 0;
}
static int b_attr_destroy(void *a) { (void)a; return 0; }
static int b_attr_setstacksize(void *a, size_t s) { ((bionic_attr *)a)->stack_size = s; return 0; }
static int b_attr_getstacksize(void *a, size_t *s) { *s = ((bionic_attr *)a)->stack_size; return 0; }
static int b_attr_setguardsize(void *a, size_t s) { ((bionic_attr *)a)->guard_size = s; return 0; }
static int b_attr_setdetachstate(void *a, int st)
{
    bionic_attr *b = a;
    if (st)
        b->flags |= BIONIC_ATTR_DETACHED;
    else
        b->flags &= ~BIONIC_ATTR_DETACHED;
    return 0;
}
static int b_attr_getdetachstate(void *a, int *st)
{
    *st = (((bionic_attr *)a)->flags & BIONIC_ATTR_DETACHED) ? 1 : 0;
    return 0;
}
static int b_attr_setschedparam(void *a, const void *p)
{
    ((bionic_attr *)a)->sched_priority = *(const int *)p;
    return 0;
}
static int b_attr_setschedpolicy(void *a, int p) { ((bionic_attr *)a)->sched_policy = p; return 0; }
static int b_attr_getstack(void *a, void **base, size_t *sz)
{
    bionic_attr *b = a;
    *base = b->stack_base;
    *sz = b->stack_size;
    return 0;
}

static int b_getattr_np(pthread_t t, void *a)
{
    /* Boehm-style GCs and Unity's Baselib read the stack bounds from here. */
    bionic_attr *b = a;
    pthread_attr_t g;
    b_attr_init(b);
    if (pthread_getattr_np(t, &g) == 0) {
        void *base = NULL;
        size_t sz = 0;
        pthread_attr_getstack(&g, &base, &sz);
        b->stack_base = base;
        b->stack_size = sz;
        pthread_attr_destroy(&g);
    }
    return 0;
}

static int b_create(pthread_t *th, const void *attr, void *(*fn)(void *), void *arg)
{
    pthread_attr_t g;
    pthread_attr_init(&g);
    size_t requested_stack = 0;
    uint32_t requested_flags = 0;
    if (attr) {
        const bionic_attr *b = attr;
        requested_stack = b->stack_size;
        requested_flags = b->flags;
        if (b->stack_size >= 16384)
            pthread_attr_setstacksize(&g, b->stack_size);
        if (b->flags & BIONIC_ATTR_DETACHED)
            pthread_attr_setdetachstate(&g, PTHREAD_CREATE_DETACHED);
    }
    int r = pthread_create(th, &g, fn, arg);
    if (getenv("BC_THREADLOG"))
        nx_log("pthread_create attr=%p stack=%zu flags=%#x fn=%p arg=%p"
               " -> %d thread=%p",
               attr, requested_stack, requested_flags, (void *)fn, arg, r,
               (void *)(uintptr_t)(r == 0 && th ? *th : 0));
    pthread_attr_destroy(&g);
    return r;
}

static int b_setname_np(pthread_t t, const char *n)
{
    char b[16];
    snprintf(b, sizeof b, "%s", n ? n : "");
    int r = pthread_setname_np(t, b);
    if (getenv("BC_THREADLOG"))
        nx_log("pthread_setname_np thread=%p requested=\"%s\" applied=\"%s\""
               " -> %d",
               (void *)(uintptr_t)t, n ? n : "", b, r);
    return r;
}

/* ---------------------------------------------------------------- publish */

static const nx_import tab[] = {
    { "pthread_mutex_init",            (void *)b_mutex_init },
    { "pthread_mutex_destroy",         (void *)b_mutex_destroy },
    { "pthread_mutex_lock",            (void *)b_mutex_lock },
    { "pthread_mutex_unlock",          (void *)b_mutex_unlock },
    { "pthread_mutex_trylock",         (void *)b_mutex_trylock },
    { "pthread_mutex_timedlock",       (void *)b_mutex_timedlock },
    { "pthread_mutexattr_init",        (void *)b_mutexattr_init },
    { "pthread_mutexattr_destroy",     (void *)b_mutexattr_destroy },
    { "pthread_mutexattr_settype",     (void *)b_mutexattr_settype },
    { "pthread_mutexattr_gettype",     (void *)b_mutexattr_gettype },
    { "pthread_mutexattr_setpshared",  (void *)b_mutexattr_setpshared },
    { "pthread_cond_init",             (void *)b_cond_init },
    { "pthread_cond_destroy",          (void *)b_cond_destroy },
    { "pthread_cond_wait",             (void *)b_cond_wait },
    { "pthread_cond_timedwait",        (void *)b_cond_timedwait },
    { "pthread_cond_signal",           (void *)b_cond_signal },
    { "pthread_cond_broadcast",        (void *)b_cond_broadcast },
    { "pthread_condattr_init",         (void *)b_condattr_init },
    { "pthread_condattr_destroy",      (void *)b_condattr_destroy },
    { "pthread_condattr_setclock",     (void *)b_condattr_setclock },
    { "pthread_condattr_setpshared",   (void *)b_condattr_setpshared },
    { "pthread_rwlock_init",           (void *)b_rwlock_init },
    { "pthread_rwlock_destroy",        (void *)b_rwlock_destroy },
    { "pthread_rwlock_rdlock",         (void *)b_rwlock_rdlock },
    { "pthread_rwlock_wrlock",         (void *)b_rwlock_wrlock },
    { "pthread_rwlock_unlock",         (void *)b_rwlock_unlock },
    { "pthread_rwlock_tryrdlock",      (void *)b_rwlock_tryrdlock },
    { "pthread_rwlock_trywrlock",      (void *)b_rwlock_trywrlock },
    { "sem_init",                      (void *)b_sem_init },
    { "sem_destroy",                   (void *)b_sem_destroy },
    { "sem_post",                      (void *)b_sem_post },
    { "sem_wait",                      (void *)b_sem_wait },
    { "sem_trywait",                   (void *)b_sem_trywait },
    { "sem_timedwait",                 (void *)b_sem_timedwait },
    { "sem_getvalue",                  (void *)b_sem_getvalue },
    /* bionic and glibc both use a 4-byte pthread_once_t initialised to 0. */
    { "pthread_once",                  (void *)pthread_once },
    { "pthread_attr_init",             (void *)b_attr_init },
    { "pthread_attr_destroy",          (void *)b_attr_destroy },
    { "pthread_attr_setstacksize",     (void *)b_attr_setstacksize },
    { "pthread_attr_getstacksize",     (void *)b_attr_getstacksize },
    { "pthread_attr_setguardsize",     (void *)b_attr_setguardsize },
    { "pthread_attr_setdetachstate",   (void *)b_attr_setdetachstate },
    { "pthread_attr_getdetachstate",   (void *)b_attr_getdetachstate },
    { "pthread_attr_setschedparam",    (void *)b_attr_setschedparam },
    { "pthread_attr_setschedpolicy",   (void *)b_attr_setschedpolicy },
    { "pthread_attr_getstack",         (void *)b_attr_getstack },
    { "pthread_getattr_np",            (void *)b_getattr_np },
    { "pthread_create",                (void *)b_create },
    { "pthread_setname_np",            (void *)b_setname_np },
};

const nx_import *bc_pthread_table(size_t *n)
{
    *n = sizeof tab / sizeof *tab;
    return tab;
}
