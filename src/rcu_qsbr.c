/*
 * rcu_qsbr.c - QSBR RCU implementation. See rcu_qsbr.h for the API.
 *
 * How it works
 * ------------
 * Each registered reader owns a cache-line-padded counter slot.
 *   - When ONLINE, the reader's slot holds a monotonically increasing
 *     "quiescent counter": every call to rcu_qsbr_quiescent() copies
 *     the variable's current global token into the slot.
 *   - When OFFLINE, the slot holds a special value (CNT_OFFLINE) so the
 *     writer treats the reader as permanently quiescent and skips it.
 *
 * rcu_qsbr_start() bumps the global token and returns the new value T.
 * A reader has "passed" token T once its slot counter >= T.
 * rcu_qsbr_check(T) succeeds when every registered, online reader has
 * a slot counter >= T.  At that point no reader can still hold a
 * reference taken before start() was called -> safe to free.
 */
#include "rcu_qsbr.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <time.h>

#define RCU_CACHE_LINE 64

/* Slot counter sentinels. Real counters are ordinary increasing ints. */
#define CNT_UNREGISTERED 0u
#define CNT_OFFLINE      1u
#define CNT_FIRST        2u   /* first usable quiescent-counter value */

/* One reader's state, padded to its own cache line to avoid false
 * sharing between readers hammering their counters. */
struct reader_slot {
    _Atomic uint64_t cnt;     /* CNT_* sentinel, or a quiescent counter */
    _Atomic uint32_t registered;
    char _pad[RCU_CACHE_LINE - sizeof(uint64_t) - sizeof(uint32_t)];
};

struct rcu_qsbr {
    _Atomic uint64_t token;   /* global grace-period counter         */
    uint32_t max_threads;
    char _pad[RCU_CACHE_LINE - sizeof(uint64_t) - sizeof(uint32_t)];
    struct reader_slot slots[];  /* flexible array, one per thread   */
};

/* ------------------------------------------------------------------ */
/* QS variable                                                         */
/* ------------------------------------------------------------------ */

size_t rcu_qsbr_get_memsize(uint32_t max_threads)
{
    return sizeof(struct rcu_qsbr)
         + (size_t)max_threads * sizeof(struct reader_slot);
}

int rcu_qsbr_init(struct rcu_qsbr *v, uint32_t max_threads)
{
    if (v == NULL || max_threads == 0)
        return -1;

    memset(v, 0, rcu_qsbr_get_memsize(max_threads));
    v->max_threads = max_threads;
    atomic_store_explicit(&v->token, CNT_FIRST, memory_order_relaxed);

    for (uint32_t i = 0; i < max_threads; i++) {
        atomic_store_explicit(&v->slots[i].cnt, CNT_UNREGISTERED,
                              memory_order_relaxed);
        atomic_store_explicit(&v->slots[i].registered, 0,
                              memory_order_relaxed);
    }
    return 0;
}

void rcu_qsbr_thread_register(struct rcu_qsbr *v, uint32_t id)
{
    if (id >= v->max_threads)
        return;
    /* Registered but not yet reporting: start offline. */
    atomic_store_explicit(&v->slots[id].cnt, CNT_OFFLINE,
                          memory_order_relaxed);
    atomic_store_explicit(&v->slots[id].registered, 1,
                          memory_order_release);
}

void rcu_qsbr_thread_unregister(struct rcu_qsbr *v, uint32_t id)
{
    if (id >= v->max_threads)
        return;
    atomic_store_explicit(&v->slots[id].registered, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&v->slots[id].cnt, CNT_UNREGISTERED,
                          memory_order_release);
}

void rcu_qsbr_thread_online(struct rcu_qsbr *v, uint32_t id)
{
    if (id >= v->max_threads)
        return;
    /* Adopt the current token so the writer does not wait on a stale
     * counter from before we went offline. */
    uint64_t t = atomic_load_explicit(&v->token, memory_order_acquire);
    atomic_store_explicit(&v->slots[id].cnt, t, memory_order_release);
}

void rcu_qsbr_thread_offline(struct rcu_qsbr *v, uint32_t id)
{
    if (id >= v->max_threads)
        return;
    /* Mark quiescent forever until back online: the writer skips us. */
    atomic_store_explicit(&v->slots[id].cnt, CNT_OFFLINE,
                          memory_order_release);
}

void rcu_qsbr_quiescent(struct rcu_qsbr *v, uint32_t id)
{
    if (id >= v->max_threads)
        return;
    /* Publish the latest token: "I hold no reference as of now." */
    uint64_t t = atomic_load_explicit(&v->token, memory_order_acquire);
    atomic_store_explicit(&v->slots[id].cnt, t, memory_order_release);
}

uint64_t rcu_qsbr_start(struct rcu_qsbr *v)
{
    /* Hand out a fresh token. fetch_add returns the previous value,
     * so the caller's token is old+1. Multiple writers each get a
     * distinct, lock-free token. */
    return atomic_fetch_add_explicit(&v->token, 1,
                                     memory_order_acq_rel) + 1;
}

/* Has every registered, online reader reached `token`? */
static bool all_passed(struct rcu_qsbr *v, uint64_t token)
{
    for (uint32_t i = 0; i < v->max_threads; i++) {
        if (!atomic_load_explicit(&v->slots[i].registered,
                                  memory_order_acquire))
            continue;                 /* slot not in use */

        uint64_t c = atomic_load_explicit(&v->slots[i].cnt,
                                          memory_order_acquire);
        if (c == CNT_OFFLINE)
            continue;                 /* offline = treated quiescent */
        if (c < token)
            return false;             /* still inside an old section */
    }
    return true;
}

int rcu_qsbr_check(struct rcu_qsbr *v, uint64_t token, bool wait)
{
    if (all_passed(v, token))
        return 1;
    if (!wait)
        return 0;

    /* Block-poll with a tiny backoff until the grace period ends. */
    const struct timespec nap = { 0, 100 * 1000 };  /* 100 us */
    while (!all_passed(v, token)) {
        sched_yield();
        nanosleep(&nap, NULL);
    }
    return 1;
}

void rcu_qsbr_synchronize(struct rcu_qsbr *v)
{
    rcu_qsbr_check(v, rcu_qsbr_start(v), true);
}

/* ------------------------------------------------------------------ */
/* Deferred-delete FIFO                                                 */
/* ------------------------------------------------------------------ */

/* One queued deletion: the element plus the grace-period token that
 * must elapse before the element can be freed. */
struct dq_entry {
    void    *element;
    uint64_t token;
};

struct rcu_qsbr_dq {
    struct rcu_qsbr *v;
    struct dq_entry *ring;
    uint32_t cap;             /* ring capacity                 */
    uint32_t head;            /* enqueue index                 */
    uint32_t tail;            /* reclaim index                 */
    uint32_t count;           /* pending entries               */
    uint32_t trigger_limit;
    uint32_t max_reclaim;
    rcu_free_fn free_fn;
    void       *free_ctx;
};

struct rcu_qsbr_dq *rcu_qsbr_dq_create(const struct rcu_qsbr_dq_params *p)
{
    if (p == NULL || p->v == NULL || p->size == 0 || p->free_fn == NULL)
        return NULL;

    struct rcu_qsbr_dq *dq = calloc(1, sizeof(*dq));
    if (dq == NULL)
        return NULL;

    dq->ring = calloc(p->size, sizeof(struct dq_entry));
    if (dq->ring == NULL) {
        free(dq);
        return NULL;
    }
    dq->v             = p->v;
    dq->cap           = p->size;
    dq->trigger_limit = p->trigger_limit ? p->trigger_limit : p->size;
    dq->max_reclaim   = p->max_reclaim ? p->max_reclaim : p->size;
    dq->free_fn       = p->free_fn;
    dq->free_ctx      = p->free_ctx;
    return dq;
}

/* Free up to `budget` entries whose grace period has elapsed.
 * Entries are reclaimed strictly in FIFO order; the first one whose
 * token has not been reached stops the pass (later ones can't be
 * older). Non-blocking: uses rcu_qsbr_check(..., wait=false). */
static uint32_t dq_reclaim_pass(struct rcu_qsbr_dq *dq, uint32_t budget)
{
    uint32_t freed = 0;
    while (dq->count > 0 && freed < budget) {
        struct dq_entry *e = &dq->ring[dq->tail];
        if (!rcu_qsbr_check(dq->v, e->token, false))
            break;                       /* not matured yet */
        dq->free_fn(dq->free_ctx, e->element);
        dq->tail = (dq->tail + 1) % dq->cap;
        dq->count--;
        freed++;
    }
    return freed;
}

int rcu_qsbr_dq_enqueue(struct rcu_qsbr_dq *dq, void *element)
{
    /* Keep the FIFO from growing without bound. */
    if (dq->count >= dq->trigger_limit)
        dq_reclaim_pass(dq, dq->max_reclaim);

    /* Still full: force a blocking reclaim of the oldest entry so the
     * caller never fails just because readers were briefly slow. */
    if (dq->count >= dq->cap) {
        struct dq_entry *e = &dq->ring[dq->tail];
        rcu_qsbr_check(dq->v, e->token, true);
        dq->free_fn(dq->free_ctx, e->element);
        dq->tail = (dq->tail + 1) % dq->cap;
        dq->count--;
    }

    /* Record the element with a fresh grace-period token. */
    dq->ring[dq->head].element = element;
    dq->ring[dq->head].token   = rcu_qsbr_start(dq->v);
    dq->head  = (dq->head + 1) % dq->cap;
    dq->count++;
    return 0;
}

uint32_t rcu_qsbr_dq_reclaim(struct rcu_qsbr_dq *dq,
                             uint32_t *freed, uint32_t *pending)
{
    uint32_t f = dq_reclaim_pass(dq, dq->max_reclaim);
    if (freed)   *freed   = f;
    if (pending) *pending = dq->count;
    return f;
}

void rcu_qsbr_dq_delete(struct rcu_qsbr_dq *dq)
{
    if (dq == NULL)
        return;
    /* Drain everything left: block until each entry's period ends. */
    while (dq->count > 0) {
        struct dq_entry *e = &dq->ring[dq->tail];
        rcu_qsbr_check(dq->v, e->token, true);
        dq->free_fn(dq->free_ctx, e->element);
        dq->tail = (dq->tail + 1) % dq->cap;
        dq->count--;
    }
    free(dq->ring);
    free(dq);
}
