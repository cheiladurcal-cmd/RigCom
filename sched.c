#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — src/sched.c
   Parallel N-core scheduler: pthread task pool
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/rigsched.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Worker thread function ─────────────────────────────────── */
static void* worker_fn(void *arg) {
    Scheduler *s = (Scheduler *)arg;

    while (1) {
        pthread_mutex_lock(&s->lock);

        /* Wait for work or shutdown */
        while (!s->shutdown && s->count == 0)
            pthread_cond_wait(&s->cond_work, &s->lock);

        if (s->shutdown && s->count == 0) {
            pthread_mutex_unlock(&s->lock);
            return NULL;
        }

        /* Dequeue job */
        SchedJob job = s->queue[s->head];
        s->head      = (s->head + 1) % SCHED_QUEUE_CAP;
        s->count--;
        s->active++;
        pthread_mutex_unlock(&s->lock);

        /* Execute */
        if (job.fn) job.fn(job.arg);

        /* Notify completion */
        pthread_mutex_lock(&s->lock);
        s->active--;
        if (s->active == 0 && s->count == 0)
            pthread_cond_broadcast(&s->cond_done);
        pthread_mutex_unlock(&s->lock);
    }
    return NULL;
}

/* ── Lifecycle ──────────────────────────────────────────────── */
Scheduler* sched_new(uint32_t n_threads) {
    if (n_threads == 0) n_threads = 1;

    Scheduler *s = calloc(1, sizeof(Scheduler));
    if (!s) return NULL;

    s->n_threads = n_threads;
    s->threads   = malloc(n_threads * sizeof(pthread_t));
    if (!s->threads) { free(s); return NULL; }

    pthread_mutex_init(&s->lock,      NULL);
    pthread_cond_init (&s->cond_work, NULL);
    pthread_cond_init (&s->cond_done, NULL);

    s->head = s->tail = s->count = s->active = 0;
    s->shutdown = false;

    for (uint32_t i = 0; i < n_threads; i++) {
        if (pthread_create(&s->threads[i], NULL, worker_fn, s) != 0) {
            fprintf(stderr, "[sched] Error al crear hilo %u\n", i);
            s->n_threads = i;
            break;
        }
    }
    return s;
}

void sched_free(Scheduler *s) {
    if (!s) return;

    /* Signal shutdown */
    pthread_mutex_lock(&s->lock);
    s->shutdown = true;
    pthread_cond_broadcast(&s->cond_work);
    pthread_mutex_unlock(&s->lock);

    /* Join all threads */
    for (uint32_t i = 0; i < s->n_threads; i++)
        pthread_join(s->threads[i], NULL);

    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy (&s->cond_work);
    pthread_cond_destroy (&s->cond_done);

    free(s->threads);
    free(s);
}

/* ── Submit a job ───────────────────────────────────────────── */
void sched_submit(Scheduler *s, SchedTaskFn fn, void *arg) {
    if (!s || !fn) return;

    pthread_mutex_lock(&s->lock);

    /* Block if queue is full */
    while (s->count >= SCHED_QUEUE_CAP) {
        pthread_mutex_unlock(&s->lock);
        /* Yield and retry — in production use a backoff */
        struct timespec ts = {0, 1000000}; /* 1 ms */
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&s->lock);
    }

    s->queue[s->tail].fn  = fn;
    s->queue[s->tail].arg = arg;
    s->tail   = (s->tail + 1) % SCHED_QUEUE_CAP;
    s->count++;

    pthread_cond_signal(&s->cond_work);
    pthread_mutex_unlock(&s->lock);
}

/* ── Wait for all jobs ──────────────────────────────────────── */
void sched_wait(Scheduler *s) {
    if (!s) return;

    pthread_mutex_lock(&s->lock);
    while (s->count > 0 || s->active > 0)
        pthread_cond_wait(&s->cond_done, &s->lock);
    pthread_mutex_unlock(&s->lock);
}

/* ── Map: run fn on each item in parallel ───────────────────── */
typedef struct {
    SchedMapFn  fn;
    void       *item;
    uint32_t    index;
    void       *user;
} MapArg;

static void map_wrapper(void *arg) {
    MapArg *ma = (MapArg *)arg;
    ma->fn(ma->item, ma->index, ma->user);
    free(ma);
}

void sched_map(Scheduler *s, void **items, uint32_t n,
               SchedMapFn fn, void *user) {
    if (!s || !items || !fn) return;
    for (uint32_t i = 0; i < n; i++) {
        MapArg *ma = malloc(sizeof(MapArg));
        if (!ma) continue;
        ma->fn    = fn;
        ma->item  = items[i];
        ma->index = i;
        ma->user  = user;
        sched_submit(s, map_wrapper, ma);
    }
    sched_wait(s);
}
