#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — include/sched.h
   Parallel N-core scheduler (pthread task pool)
   ============================================================ */
#ifndef SCHED_H
#include <pthread.h>
#define SCHED_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

typedef void (*SchedTaskFn)(void *arg);

typedef struct {
    SchedTaskFn fn;
    void       *arg;
} SchedJob;

#define SCHED_QUEUE_CAP 1024

typedef struct {
    SchedJob         queue[SCHED_QUEUE_CAP];
    uint32_t         head, tail, count;
    pthread_mutex_t  lock;
    pthread_cond_t   cond_work;
    pthread_cond_t   cond_done;
    pthread_t       *threads;
    uint32_t         n_threads;
    uint32_t         active;     /* jobs in flight */
    bool             shutdown;
} Scheduler;

/* ── Lifecycle ──────────────────────────────────────────────── */
Scheduler* sched_new  (uint32_t n_threads);
void       sched_free (Scheduler *s);

/* ── Submit a job (non-blocking) ────────────────────────────── */
void sched_submit(Scheduler *s, SchedTaskFn fn, void *arg);

/* ── Wait until all submitted jobs complete ─────────────────── */
void sched_wait(Scheduler *s);

/* ── Helper: run fn on all n items in parallel ──────────────── */
typedef void (*SchedMapFn)(void *item, uint32_t index, void *user);
void sched_map(Scheduler *s, void **items, uint32_t n,
               SchedMapFn fn, void *user);

#endif /* SCHED_H */
