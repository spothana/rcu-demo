/*
 * main.c - Demonstrates the QSBR RCU emulation.
 *
 * Scenario
 * --------
 * A "config" object is shared, lock-free, between several reader
 * threads and one writer thread. The writer periodically publishes a
 * brand-new config and must free the old one -- but only once every
 * reader has stopped referencing it.
 *
 * Readers never block the writer with a lock. The writer never frees
 * memory a reader could still be touching. Correctness comes entirely
 * from quiescent-state reporting.
 *
 * Two reclamation styles are shown:
 *   - The writer thread uses the deferred-delete FIFO (rcu_qsbr_dq_*),
 *     so it just enqueues old configs and never blocks.
 *   - demo_synchronize() shows the simple blocking rcu_qsbr_synchronize.
 */
#include "rcu_qsbr.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NUM_READERS  4
#define MAX_THREADS  16          /* sized generously for the QS var */
#define RUN_SECONDS  3

/* ---- The shared, reference-counted-by-RCU data structure --------- */
struct config {
    int      version;
    int      payload[16];        /* some data a reader walks over    */
};

/* The single lock-free shared pointer. Writer swaps it atomically. */
static struct config *_Atomic g_config;

/* Globals wired up in main(). */
static struct rcu_qsbr     *g_qsv;
static struct rcu_qsbr_dq  *g_dq;
static atomic_bool          g_quit;

/* Stats, just so the demo prints something meaningful. */
static atomic_ullong g_reads;
static atomic_uint   g_frees;

/* ---- Free callback invoked by the dq FIFO ------------------------ */
static void free_config(void *ctx, void *element)
{
    (void)ctx;
    struct config *c = element;
    atomic_fetch_add_explicit(&g_frees, 1, memory_order_relaxed);
    free(c);
}

/* Allocate and fill a new config version. */
static struct config *make_config(int version)
{
    struct config *c = malloc(sizeof(*c));
    if (c == NULL) {
        perror("malloc");
        exit(1);
    }
    c->version = version;
    for (int i = 0; i < 16; i++)
        c->payload[i] = version * 100 + i;
    return c;
}

/* ---- Reader thread ----------------------------------------------- */
static void *reader_thread(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;

    rcu_qsbr_thread_register(g_qsv, id);
    rcu_qsbr_thread_online(g_qsv, id);

    unsigned long long local_reads = 0;

    while (!atomic_load_explicit(&g_quit, memory_order_acquire)) {

        /* --- critical section: reference the shared config -------- */
        struct config *c = atomic_load_explicit(&g_config,
                                                memory_order_acquire);
        /* Walk the data. Because the writer never frees a config
         * until all readers are quiescent, 'c' stays valid for the
         * whole of this section even though deletes happen. */
        volatile long sum = 0;
        for (int i = 0; i < 16; i++)
            sum += c->payload[i];
        (void)sum;
        local_reads++;
        /* --- end critical section --------------------------------- */

        /* Quiescent point: we no longer hold a reference to 'c'.
         * This is what lets the writer's grace period end. */
        rcu_qsbr_quiescent(g_qsv, id);
    }

    /* One reader demonstrates the offline path for blocking work. */
    if (id == 0) {
        rcu_qsbr_thread_offline(g_qsv, id);   /* writer skips us */
        struct timespec nap = { 0, 1000 * 1000 };
        nanosleep(&nap, NULL);                /* pretend: blocking I/O */
        rcu_qsbr_thread_online(g_qsv, id);
    }

    rcu_qsbr_thread_offline(g_qsv, id);
    rcu_qsbr_thread_unregister(g_qsv, id);

    atomic_fetch_add_explicit(&g_reads, local_reads, memory_order_relaxed);
    printf("  reader %u: %llu reads\n", id, local_reads);
    return NULL;
}

/* ---- Writer thread ----------------------------------------------- */
static void *writer_thread(void *arg)
{
    (void)arg;
    int version = 1;

    while (!atomic_load_explicit(&g_quit, memory_order_acquire)) {

        /* Step 1 - Delete: atomically publish a new config and take
         * the old pointer. New readers can only see the new one. */
        struct config *fresh = make_config(++version);
        struct config *old   =
            atomic_exchange_explicit(&g_config, fresh,
                                     memory_order_acq_rel);

        /* Step 2 - Free (deferred): hand the old config to the FIFO.
         * dq_enqueue starts a grace period and reclaims matured
         * entries. The writer never blocks here -- it is free to do
         * other useful work, which is the whole point of the design. */
        rcu_qsbr_dq_enqueue(g_dq, old);

        struct timespec nap = { 0, 5 * 1000 * 1000 };  /* 5 ms */
        nanosleep(&nap, NULL);
    }
    return NULL;
}

/* ---- A second, simpler reclamation style: synchronize ------------ */
static void demo_synchronize(void)
{
    struct config *fresh = make_config(999);
    struct config *old   = atomic_exchange(&g_config, fresh);

    /* One call: triggers the report AND blocks until every reader is
     * quiescent. No token to store, no FIFO -- free immediately. */
    rcu_qsbr_synchronize(g_qsv);
    free_config(NULL, old);
    printf("demo_synchronize: old config freed safely\n");
}

int main(void)
{
    /* ---- 1. Initialization (the "application" owns this) --------- */
    size_t sz = rcu_qsbr_get_memsize(MAX_THREADS);
    g_qsv = malloc(sz);
    if (g_qsv == NULL || rcu_qsbr_init(g_qsv, MAX_THREADS) != 0) {
        fprintf(stderr, "QS variable init failed\n");
        return 1;
    }

    struct rcu_qsbr_dq_params dqp = {
        .v             = g_qsv,
        .size          = 256,
        .trigger_limit = 32,     /* reclaim once 32 entries pile up */
        .max_reclaim   = 16,
        .free_fn       = free_config,
        .free_ctx      = NULL,
    };
    g_dq = rcu_qsbr_dq_create(&dqp);
    if (g_dq == NULL) {
        fprintf(stderr, "dq FIFO create failed\n");
        return 1;
    }

    atomic_store(&g_config, make_config(1));

    printf("RCU QSBR demo: %d readers + 1 writer, %d s\n\n",
           NUM_READERS, RUN_SECONDS);

    /* ---- 2. Launch reader + writer threads ----------------------- */
    pthread_t readers[NUM_READERS], writer;
    for (uint32_t i = 0; i < NUM_READERS; i++)
        pthread_create(&readers[i], NULL, reader_thread,
                       (void *)(uintptr_t)i);
    pthread_create(&writer, NULL, writer_thread, NULL);

    sleep(RUN_SECONDS);
    atomic_store_explicit(&g_quit, true, memory_order_release);

    pthread_join(writer, NULL);
    for (uint32_t i = 0; i < NUM_READERS; i++)
        pthread_join(readers[i], NULL);

    /* ---- 3. Drain remaining deferred frees ----------------------- */
    uint32_t freed = 0, pending = 0;
    rcu_qsbr_dq_reclaim(g_dq, &freed, &pending);

    /* ---- 4. Shutdown: dq_delete drains the rest, frees the FIFO -- */
    rcu_qsbr_dq_delete(g_dq);

    /* Show the simple synchronize() style too. */
    demo_synchronize();

    printf("\nTotals: %llu reads, %u configs freed (no use-after-free)\n",
           atomic_load(&g_reads), atomic_load(&g_frees));

    /* Free the final live config and the QS variable itself. */
    free(atomic_load(&g_config));
    free(g_qsv);
    return 0;
}
