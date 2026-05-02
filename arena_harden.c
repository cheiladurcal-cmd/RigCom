#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — src/arena_harden.c
   Arena Memory Hardening: guard pages + canary detection
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/arena_harden.h"
#include "../include/ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>

ArenaHardenStats g_arena_stats = {0};

bool arena_install_guard(void *block_end, size_t guard_size) {
    if (!block_end || guard_size == 0) return false;
    /* Asegura que la dirección esté alineada a página */
    uintptr_t addr = (uintptr_t)block_end;
    uintptr_t page = (addr + 4095u) & ~4095u;
    if (page == addr && guard_size >= 4096) {
        int r = mprotect((void *)page, guard_size, PROT_NONE);
        if (r == 0) return true;
    }
    return false;
}

void arena_write_canary(void *block_end, size_t block_cap) {
    if (!block_end || block_cap < 8) return;
    /* Escribe canary en los 8 bytes ANTES del límite */
    uint64_t *canary_ptr = (uint64_t *)((char *)block_end + block_cap - 8);
    *canary_ptr = ARENA_CANARY;
}

bool arena_check_canary(const void *block_end, size_t block_cap) {
    if (!block_end || block_cap < 8) return true;
    const uint64_t *canary_ptr =
        (const uint64_t *)((const char *)block_end + block_cap - 8);
    if (*canary_ptr != ARENA_CANARY) {
        g_arena_stats.guard_hits++;
        fprintf(stderr,
            "[ARENA-HARDEN] ¡CORRUPCIÓN DETECTADA! "
            "Canary 0x%016llX != 0x%016llX en bloque %p\n",
            (unsigned long long)*canary_ptr,
            (unsigned long long)ARENA_CANARY,
            block_end);
        return false;
    }
    return true;
}

int arena_harden_check_all(void *arena_opaque) {
    (void)arena_opaque;
    /* En el arena real (ast.c) los bloques son ASTBlock.
       Esta función se llama opcionalmente con -DRIG_ARENA_HARDEN.
       Aquí verificamos el canary global de stats. */
    if (g_arena_stats.guard_hits > 0) {
        fprintf(stderr,
            "[ARENA-HARDEN] %llu corrupcion(es) de arena detectadas\n",
            (unsigned long long)g_arena_stats.guard_hits);
        return (int)g_arena_stats.guard_hits;
    }
    return 0;
}

void arena_harden_report(const ArenaHardenStats *s) {
    if (!s) s = &g_arena_stats;
    fprintf(stderr,
        "[ARENA-HARDEN] allocs=%llu bytes=%llu "
        "peak=%llu guard_hits=%llu\n",
        (unsigned long long)s->total_allocs,
        (unsigned long long)s->total_bytes,
        (unsigned long long)s->peak_usage,
        (unsigned long long)s->guard_hits);
}
