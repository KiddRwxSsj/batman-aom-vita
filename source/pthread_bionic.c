#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <psp2/kernel/threadmgr.h>

#include "util.h"
#include "loader.h"

#define BIONIC_ETIMEDOUT 110
#define MIN_STACK (512 * 1024)
#define DEFAULT_STACK (1024 * 1024)

typedef struct {
    uint32_t flags;
    void *stack_base;
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
} bionic_attr;

#ifdef DEBUG_SHOTS
// leak hunt: every live mutex/cond by the .so address that made it, dumped with the fps line
#define TRACK_SLOTS 16384
static struct { int *obj; uint32_t caller; } track[TRACK_SLOTS];
static int track_lock;
static volatile int sema_live, track_full;

static void track_add(int *obj, void *ra) {
    uint32_t caller = (uint32_t)ra - ue3_mod.load_addr;
    unsigned h = ((uint32_t)obj >> 2) % TRACK_SLOTS;
    while (__sync_lock_test_and_set(&track_lock, 1));
    for (int i = 0; i < TRACK_SLOTS; i++, h = (h + 1) % TRACK_SLOTS) {
        if ((uintptr_t)track[h].obj <= 1 || track[h].obj == obj) {
            track[h].obj = obj;
            track[h].caller = caller;
            break;
        }
        if (i == TRACK_SLOTS - 1)
            track_full++;
    }
    __sync_lock_release(&track_lock);
}

static void track_del(int *obj) {
    unsigned h = ((uint32_t)obj >> 2) % TRACK_SLOTS;
    while (__sync_lock_test_and_set(&track_lock, 1));
    for (int i = 0; i < TRACK_SLOTS; i++, h = (h + 1) % TRACK_SLOTS) {
        if (track[h].obj == obj) {
            track[h].obj = (int *)1; // tombstone
            break;
        }
    }
    __sync_lock_release(&track_lock);
}

int __real_sceKernelCreateSema(const char *, unsigned, int, int, void *);
int __real_sceKernelDeleteSema(int);
int __wrap_sceKernelCreateSema(const char *name, unsigned attr, int init, int max, void *opt) {
    int ret = __real_sceKernelCreateSema(name, attr, init, max, opt);
    if (ret >= 0)
        __sync_add_and_fetch(&sema_live, 1);
    return ret;
}
int __wrap_sceKernelDeleteSema(int id) {
    int ret = __real_sceKernelDeleteSema(id);
    if (ret >= 0)
        __sync_sub_and_fetch(&sema_live, 1);
    return ret;
}

void pthread_track_dump(void) {
    static struct { uint32_t caller; int n; } top[64];
    int ntop = 0, live = 0;
    while (__sync_lock_test_and_set(&track_lock, 1));
    for (int i = 0; i < TRACK_SLOTS; i++) {
        if ((uintptr_t)track[i].obj <= 1)
            continue;
        live++;
        int j;
        for (j = 0; j < ntop && top[j].caller != track[i].caller; j++);
        if (j == ntop && ntop < 64)
            top[ntop++] = (typeof(top[0])){ track[i].caller, 0 };
        if (j < ntop)
            top[j].n++;
    }
    __sync_lock_release(&track_lock);
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "sync: %d sema, %d live objs%s:", sema_live, live, track_full ? " (table full)" : "");
    for (int k = 0; k < 6; k++) {
        int best = -1;
        for (int j = 0; j < ntop; j++)
            if (top[j].n && (best < 0 || top[j].n > top[best].n))
                best = j;
        if (best < 0)
            break;
        len += snprintf(buf + len, sizeof(buf) - len, " %x=%d", (unsigned)top[best].caller, top[best].n);
        top[best].n = 0;
    }
    LOG("%s", buf);
}
#define TRACK(o) track_add((int *)(o), __builtin_return_address(0))
#define UNTRACK(o) track_del((int *)(o))
#else
#define TRACK(o)
#define UNTRACK(o)
#endif

// mutexes and conds of our own. pte backs every mutex with a kernel semaphore and every cond with
// three, and scaleform keeps thousands of them alive in some menus (4800 seen on the boosts screen):
// the vita's per-process kernel object limit (~2000) ran out and the async loader crashed on a NULL
// event (options menu, back to the menu after a fight). here a mutex or cond is a small heap struct
// with a spinlock-protected waiter queue, and only threads that actually have to block own a kernel
// semaphore (one per thread, reused for every wait)

typedef struct waiter {
    SceUID thid, sema;
    struct waiter *next;
} waiter;

typedef struct {
    volatile int lk;
    int locked, recursive, count;
    SceUID owner;
    waiter *head, *tail;
} umutex;

typedef struct {
    volatile int lk;
    waiter *head, *tail;
} ucond;

#define MAX_WAITERS 256
static waiter waiters[MAX_WAITERS];
static volatile int waiters_lk;

static inline void spin_lock(volatile int *lk) {
    int n = 0;
    while (__sync_lock_test_and_set(lk, 1)) {
        // the holder may be a lower priority thread on this core, let it run
        if (++n > 64)
            sceKernelDelayThread(1);
    }
}

static inline void spin_unlock(volatile int *lk) {
    __sync_lock_release(lk);
}

// the calling thread's waiter, with a semaphore made the first time it has to block.
// slots of threads that are gone get reused
static waiter *self_waiter(void) {
    SceUID me = sceKernelGetThreadId();
    waiter *free_slot = NULL;
    spin_lock(&waiters_lk);
    for (int i = 0; i < MAX_WAITERS; i++) {
        if (waiters[i].thid == me) {
            spin_unlock(&waiters_lk);
            return &waiters[i];
        }
        if (!free_slot && !waiters[i].thid)
            free_slot = &waiters[i];
    }
    if (!free_slot) {
        for (int i = 0; i < MAX_WAITERS && !free_slot; i++) {
            SceKernelThreadInfo info = { sizeof(info) };
            if (sceKernelGetThreadInfo(waiters[i].thid, &info) < 0)
                free_slot = &waiters[i];
        }
    }
    if (!free_slot) {
        spin_unlock(&waiters_lk);
        fatal_error("Out of thread wait slots.");
    }
    free_slot->thid = me;
    free_slot->next = NULL;
    if (!free_slot->sema)
        free_slot->sema = sceKernelCreateSema("bionic_wait", 0, 0, 1, NULL);
    spin_unlock(&waiters_lk);
    return free_slot;
}

static void enqueue(waiter **head, waiter **tail, waiter *w) {
    w->next = NULL;
    if (*tail)
        (*tail)->next = w;
    else
        *head = w;
    *tail = w;
}

static int dequeue_this(waiter **head, waiter **tail, waiter *w) {
    waiter *prev = NULL;
    for (waiter *it = *head; it; prev = it, it = it->next) {
        if (it != w)
            continue;
        if (prev)
            prev->next = it->next;
        else
            *head = it->next;
        if (*tail == it)
            *tail = prev;
        return 1;
    }
    return 0;
}

// bionic's static initializers: 0 normal, 0x4000 recursive, 0x8000 errorcheck (treated as normal)
static umutex *get_mutex(int *m) {
    int v = *m;
    if (v != 0 && v != 0x4000 && v != 0x8000)
        return (umutex *)v;
    umutex *mx = calloc(1, sizeof(*mx));
    mx->recursive = v == 0x4000;
    if (!__sync_bool_compare_and_swap(m, v, (int)mx)) {
        free(mx);
        return (umutex *)*m;
    }
    return mx;
}

static ucond *get_cond(int *c) {
    int v = *c;
    if (v)
        return (ucond *)v;
    ucond *cv = calloc(1, sizeof(*cv));
    if (!__sync_bool_compare_and_swap(c, 0, (int)cv)) {
        free(cv);
        return (ucond *)*c;
    }
    return cv;
}

static int mutex_lock(umutex *mx) {
    SceUID me = sceKernelGetThreadId();
    spin_lock(&mx->lk);
    if (!mx->locked) {
        mx->locked = 1;
        mx->owner = me;
        mx->count = 1;
        spin_unlock(&mx->lk);
        return 0;
    }
    if (mx->recursive && mx->owner == me) {
        mx->count++;
        spin_unlock(&mx->lk);
        return 0;
    }
    waiter *w = self_waiter();
    enqueue(&mx->head, &mx->tail, w);
    spin_unlock(&mx->lk);
    // the unlocker hands the mutex over before it signals
    sceKernelWaitSema(w->sema, 1, NULL);
    return 0;
}

static int mutex_unlock(umutex *mx) {
    spin_lock(&mx->lk);
    if (mx->recursive && --mx->count > 0) {
        spin_unlock(&mx->lk);
        return 0;
    }
    waiter *w = mx->head;
    if (w) {
        mx->head = w->next;
        if (!mx->head)
            mx->tail = NULL;
        mx->owner = w->thid;
        mx->count = 1;
    } else {
        mx->locked = 0;
        mx->owner = 0;
    }
    spin_unlock(&mx->lk);
    if (w)
        sceKernelSignalSema(w->sema, 1);
    return 0;
}

int pthread_mutex_init_b(int *m, const pthread_mutexattr_t *attr) {
    TRACK(m);
    int type = PTHREAD_MUTEX_NORMAL;
    if (attr)
        pthread_mutexattr_gettype((pthread_mutexattr_t *)attr, &type);
    *m = type == PTHREAD_MUTEX_RECURSIVE ? 0x4000 : 0;
    return 0;
}

int pthread_mutex_destroy_b(int *m) {
    UNTRACK(m);
    int v = *m;
    if (v != 0 && v != 0x4000 && v != 0x8000)
        free((void *)v);
    *m = 0;
    return 0;
}

int pthread_mutex_lock_b(int *m) {
    if (*m == 0)
        TRACK(m);
    return mutex_lock(get_mutex(m));
}

int pthread_mutex_unlock_b(int *m) {
    return mutex_unlock(get_mutex(m));
}

int pthread_mutex_trylock_b(int *m) {
    umutex *mx = get_mutex(m);
    SceUID me = sceKernelGetThreadId();
    int ret = EBUSY;
    spin_lock(&mx->lk);
    if (!mx->locked) {
        mx->locked = 1;
        mx->owner = me;
        mx->count = 1;
        ret = 0;
    } else if (mx->recursive && mx->owner == me) {
        mx->count++;
        ret = 0;
    }
    spin_unlock(&mx->lk);
    return ret;
}

int pthread_cond_init_b(int *c, const pthread_condattr_t *attr) {
    TRACK(c);
    *c = 0;
    return 0;
}

int pthread_cond_destroy_b(int *c) {
    UNTRACK(c);
    if (*c)
        free((void *)*c);
    *c = 0;
    return 0;
}

// timeout_us < 0 waits forever
static int cond_wait(int *c, int *m, int64_t timeout_us) {
    ucond *cv = get_cond(c);
    umutex *mx = get_mutex(m);
    waiter *w = self_waiter();
    spin_lock(&cv->lk);
    enqueue(&cv->head, &cv->tail, w);
    spin_unlock(&cv->lk);
    mutex_unlock(mx);

    int ret = 0;
    SceUInt t = timeout_us < 0 ? 0 : (timeout_us > 0x7fffffff ? 0x7fffffff : (SceUInt)timeout_us);
    if (sceKernelWaitSema(w->sema, 1, timeout_us < 0 ? NULL : &t) < 0) {
        spin_lock(&cv->lk);
        int still_queued = dequeue_this(&cv->head, &cv->tail, w);
        spin_unlock(&cv->lk);
        if (still_queued)
            ret = BIONIC_ETIMEDOUT;
        else
            // signalled right as the wait ran out, take that signal so it doesn't leak into the next wait
            sceKernelWaitSema(w->sema, 1, NULL);
    }
    mutex_lock(mx);
    return ret;
}

int pthread_cond_wait_b(int *c, int *m) {
    return cond_wait(c, m, -1);
}

int pthread_cond_timedwait_b(int *c, int *m, const struct timespec *abstime) {
    struct timeval now;
    gettimeofday(&now, NULL);
    int64_t us = (int64_t)(abstime->tv_sec - now.tv_sec) * 1000000 + (abstime->tv_nsec / 1000 - now.tv_usec);
    return cond_wait(c, m, us < 0 ? 0 : us);
}

// a cond nobody waited on yet has no waiters to wake
int pthread_cond_signal_b(int *c) {
    if (!*c)
        return 0;
    ucond *cv = (ucond *)*c;
    spin_lock(&cv->lk);
    waiter *w = cv->head;
    if (w) {
        cv->head = w->next;
        if (!cv->head)
            cv->tail = NULL;
    }
    spin_unlock(&cv->lk);
    if (w)
        sceKernelSignalSema(w->sema, 1);
    return 0;
}

int pthread_cond_broadcast_b(int *c) {
    if (!*c)
        return 0;
    ucond *cv = (ucond *)*c;
    spin_lock(&cv->lk);
    waiter *w = cv->head;
    cv->head = cv->tail = NULL;
    spin_unlock(&cv->lk);
    while (w) {
        // read next before waking, the woken thread can queue itself somewhere else right away
        waiter *next = w->next;
        sceKernelSignalSema(w->sema, 1);
        w = next;
    }
    return 0;
}

int pthread_attr_init_b(bionic_attr *a) {
    memset(a, 0, sizeof(*a));
    a->stack_size = DEFAULT_STACK;
    return 0;
}

int pthread_attr_setstacksize_b(bionic_attr *a, size_t size) {
    a->stack_size = size;
    return 0;
}

int pthread_attr_setdetachstate_b(bionic_attr *a, int state) {
    a->flags = (a->flags & ~1) | (state ? 1 : 0);
    return 0;
}

int pthread_attr_setschedparam_b(bionic_attr *a, const int *param) {
    a->sched_priority = *param;
    return 0;
}

// bionic's pthread_once_t is a single int (0 = not run), pte's is 16 bytes
int pthread_once_b(volatile int *once, void (*fn)(void)) {
    if (*once == 2)
        return 0;
    if (__sync_bool_compare_and_swap(once, 0, 1)) {
        fn();
        __sync_synchronize();
        *once = 2;
        return 0;
    }
    while (*once != 2)
        sched_yield();
    return 0;
}

#ifdef DEBUG_SHOTS
#include <stdio.h>
#include <stdlib.h>
#include <psp2/kernel/threadmgr.h>

typedef struct {
    void *(*fn)(void *);
    void *arg;
} start_info;

// engine threads by entry offset in the .so, the fps line shows how busy each one is
#define CPU_SLOTS 32
static struct { SceUID id; uint32_t entry; uint64_t last; } cpu_thr[CPU_SLOTS];

static void cpu_track(SceUID id, uint32_t entry) {
    for (int i = 0; i < CPU_SLOTS; i++)
        if (__sync_bool_compare_and_swap(&cpu_thr[i].id, 0, id)) {
            cpu_thr[i].entry = entry;
            cpu_thr[i].last = 0;
            return;
        }
}

static void *cpu_start(void *p) {
    start_info s = *(start_info *)p;
    free(p);
    cpu_track(sceKernelGetThreadId(), (uint32_t)((uintptr_t)s.fn - ue3_mod.load_addr));
    return s.fn(s.arg);
}

// percent of the last period each live thread spent running, main thread first
void cpu_usage_dump(uint64_t period_us) {
    char line[256];
    int o = 0;
    SceKernelThreadInfo info;
    // what the engine decided about threads (GNumHardwareThreads, GUseThreadedRendering, GIsThreadedRendering)
    o += snprintf(line, sizeof(line), "hwthreads %d threaded %d/%d | ", *(int *)(ue3_mod.load_addr + 0x2229a54),
                  *(int *)(ue3_mod.load_addr + 0x22dbad4), *(int *)(ue3_mod.load_addr + 0x22dbad0));
    for (int i = 0; i < CPU_SLOTS && o < (int)sizeof(line) - 24; i++) {
        if (!cpu_thr[i].id)
            continue;
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(cpu_thr[i].id, &info) < 0) {
            cpu_thr[i].id = 0;
            continue;
        }
        int pct = cpu_thr[i].last ? (int)((info.runClocks - cpu_thr[i].last) * 100 / period_us) : -1;
        cpu_thr[i].last = info.runClocks;
        if (pct >= 3)
            o += snprintf(line + o, sizeof(line) - o, "%x=%d%% ", (unsigned)cpu_thr[i].entry, pct);
    }
    LOG("cpu: %s", line);
}
#endif

int pthread_create_b(pthread_t *t, const bionic_attr *a, void *(*fn)(void *), void *arg) {
    size_t stack = a ? a->stack_size : DEFAULT_STACK;
    if (stack < MIN_STACK)
        stack = MIN_STACK;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    if (a && (a->flags & 1))
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#ifdef DEBUG_SHOTS
    start_info *si = malloc(sizeof(*si));
    si->fn = fn;
    si->arg = arg;
    int ret = pthread_create(t, &attr, cpu_start, si);
#else
    int ret = pthread_create(t, &attr, fn, arg);
#endif
    pthread_attr_destroy(&attr);
    LOG("pthread_create %x stack %x -> %d", (unsigned)((uintptr_t)fn - ue3_mod.load_addr), stack, ret);
    return ret;
}
