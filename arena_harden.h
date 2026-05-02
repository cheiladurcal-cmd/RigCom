/* ============================================================
   RigCom v8.0 — include/arena_harden.h
   Arena Memory Hardening: guard pages + overflow detection
   Activar con -DRIG_ARENA_HARDEN en modo debug
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef ARENA_HARDEN_H
#define ARENA_HARDEN_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* ── Canary value para detección de desbordamiento ────────── */
#define ARENA_CANARY     0xDEADBEEFCAFE1618ULL
#define ARENA_GUARD_SIZE 4096   /* tamaño de guard page (1 página)  */

/* ── Stats de hardening ───────────────────────────────────── */
typedef struct {
    uint64_t total_allocs;
    uint64_t total_bytes;
    uint64_t guard_hits;      /* veces que el canary detectó corrupción */
    uint64_t peak_usage;
} ArenaHardenStats;

/* ── API pública ──────────────────────────────────────────── */

/* Instala guard page PROT_NONE después del bloque */
bool  arena_install_guard(void *block_end, size_t guard_size);

/* Escribe canary en los últimos 8 bytes antes del límite */
void  arena_write_canary(void *block_end, size_t block_cap);

/* Verifica canary — retorna false si fue corrompido */
bool  arena_check_canary(const void *block_end, size_t block_cap);

/* Verifica todos los bloques de un arena y emite warnings */
int   arena_harden_check_all(void *arena_opaque);

/* Reporta stats por stderr */
void  arena_harden_report(const ArenaHardenStats *s);

/* Singleton global de stats */
extern ArenaHardenStats g_arena_stats;

#endif /* ARENA_HARDEN_H */
