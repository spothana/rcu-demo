/*
 * rcu_port.h - Minimal portability shim so the demo builds with both
 *              POSIX threads (Linux/macOS/MinGW) and MSVC on Windows.
 *
 * Only the handful of primitives the demo needs are wrapped:
 *   - rcu_thread_t / rcu_thread_create / rcu_thread_join
 *   - rcu_sleep_ms      (whole-millisecond sleep)
 *   - rcu_sleep_us      (sub-millisecond sleep, used by the backoff)
 *   - rcu_yield         (hint the scheduler)
 *
 * The C11 <stdatomic.h> code itself is portable and is NOT wrapped.
 */
#ifndef RCU_PORT_H
#define RCU_PORT_H

#include <stdlib.h>   /* malloc, free          */
#include <stdint.h>   /* uintptr_t             */

/* ================================================================== */
#if defined(_WIN32)
/* -------- Windows / MSVC: native Win32 threads -------------------- */

#include <windows.h>
#include <process.h>

typedef HANDLE rcu_thread_t;
typedef unsigned (__stdcall *rcu_thread_fn)(void *);

/* Wrap a (void *(*)(void *)) entry point into the __stdcall form
 * that _beginthreadex expects. We store the user function + arg in a
 * small heap struct and trampoline through rcu__win_trampoline. */
struct rcu__win_arg {
    void *(*fn)(void *);
    void  *arg;
};

static unsigned __stdcall rcu__win_trampoline(void *p)
{
    struct rcu__win_arg a = *(struct rcu__win_arg *)p;
    free(p);
    a.fn(a.arg);
    return 0;
}

static inline int rcu_thread_create(rcu_thread_t *t,
                                    void *(*fn)(void *), void *arg)
{
    struct rcu__win_arg *a = (struct rcu__win_arg *)malloc(sizeof(*a));
    if (a == NULL)
        return -1;
    a->fn  = fn;
    a->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, rcu__win_trampoline, a, 0, NULL);
    if (h == 0) {
        free(a);
        return -1;
    }
    *t = (rcu_thread_t)h;
    return 0;
}

static inline void rcu_thread_join(rcu_thread_t t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static inline void rcu_sleep_ms(unsigned ms) { Sleep(ms); }

static inline void rcu_sleep_us(unsigned us)
{
    /* Windows Sleep() granularity is coarse; round sub-ms up to 1 ms. */
    Sleep(us < 1000 ? 1 : us / 1000);
}

static inline void rcu_yield(void) { SwitchToThread(); }

/* ================================================================== */
#else
/* -------- POSIX: Linux, macOS, MinGW with pthreads ---------------- */

#include <pthread.h>
#include <sched.h>
#include <time.h>

typedef pthread_t rcu_thread_t;

static inline int rcu_thread_create(rcu_thread_t *t,
                                    void *(*fn)(void *), void *arg)
{
    return pthread_create(t, NULL, fn, arg);
}

static inline void rcu_thread_join(rcu_thread_t t)
{
    pthread_join(t, NULL);
}

static inline void rcu_sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static inline void rcu_sleep_us(unsigned us)
{
    struct timespec ts = { us / 1000000, (long)(us % 1000000) * 1000L };
    nanosleep(&ts, NULL);
}

static inline void rcu_yield(void) { sched_yield(); }

#endif
/* ================================================================== */

#endif /* RCU_PORT_H */
