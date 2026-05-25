/*
 * rcu_qsbr.h - A self-contained QSBR (Quiescent State Based Reclamation)
 *              RCU implementation that emulates the DPDK rte_rcu_qsbr API.
 *
 * No DPDK dependency. Uses only C11 <stdatomic.h> and POSIX threads.
 *
 * The design mirrors the DPDK RCU library:
 *   - A "QS variable" holds one per-thread counter slot per reader.
 *   - Readers periodically publish a quiescent-state counter.
 *   - A writer calls qsbr_start() to get a token, then qsbr_check()
 *     to learn when every reader has passed that token.
 *   - A deferred-delete FIFO (dq) automates the delete -> free split.
 */
#ifndef RCU_QSBR_H
#define RCU_QSBR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* QS variable                                                         */
/* ------------------------------------------------------------------ */

struct rcu_qsbr;          /* opaque: the shared QS variable */

/* Sentinel thread id, mirrors RTE_QSBR_THRID_INVALID. */
#define RCU_THRID_INVALID UINT32_MAX

/* Bytes needed to hold a QS variable sized for `max_threads` readers. */
size_t rcu_qsbr_get_memsize(uint32_t max_threads);

/* Initialize a caller-allocated region (of rcu_qsbr_get_memsize bytes). */
int rcu_qsbr_init(struct rcu_qsbr *v, uint32_t max_threads);

/* Reader lifecycle. thread_id must be in [0, max_threads). */
void rcu_qsbr_thread_register(struct rcu_qsbr *v, uint32_t thread_id);
void rcu_qsbr_thread_unregister(struct rcu_qsbr *v, uint32_t thread_id);
void rcu_qsbr_thread_online(struct rcu_qsbr *v, uint32_t thread_id);
void rcu_qsbr_thread_offline(struct rcu_qsbr *v, uint32_t thread_id);

/* Reader reports it currently holds no reference to shared memory. */
void rcu_qsbr_quiescent(struct rcu_qsbr *v, uint32_t thread_id);

/* Writer side. start() returns a token; check() tests it. */
uint64_t rcu_qsbr_start(struct rcu_qsbr *v);
int      rcu_qsbr_check(struct rcu_qsbr *v, uint64_t token, bool wait);

/* start() + blocking check() combined. */
void rcu_qsbr_synchronize(struct rcu_qsbr *v);

/* ------------------------------------------------------------------ */
/* Deferred-delete FIFO (resource reclamation framework)                */
/* ------------------------------------------------------------------ */

struct rcu_qsbr_dq;       /* opaque: the deferred-delete FIFO */

/* Called by the library to release a matured element. */
typedef void (*rcu_free_fn)(void *ctx, void *element);

struct rcu_qsbr_dq_params {
    struct rcu_qsbr *v;             /* the shared QS variable        */
    uint32_t         size;          /* FIFO capacity (elements)      */
    uint32_t         trigger_limit; /* reclaim when this many queued */
    uint32_t         max_reclaim;   /* max elements freed per pass   */
    rcu_free_fn      free_fn;       /* element free callback         */
    void            *free_ctx;      /* opaque ctx passed to free_fn  */
};

struct rcu_qsbr_dq *rcu_qsbr_dq_create(const struct rcu_qsbr_dq_params *p);

/* Enqueue a deleted element pointer; starts a grace period for it.
 * Reclaims matured entries opportunistically; reclaims forcibly if full. */
int rcu_qsbr_dq_enqueue(struct rcu_qsbr_dq *dq, void *element);

/* Force a reclamation pass. Returns count freed; out params optional. */
uint32_t rcu_qsbr_dq_reclaim(struct rcu_qsbr_dq *dq,
                             uint32_t *freed, uint32_t *pending);

/* Drain everything still pending and destroy the FIFO. */
void rcu_qsbr_dq_delete(struct rcu_qsbr_dq *dq);

#endif /* RCU_QSBR_H */
