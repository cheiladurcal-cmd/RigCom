/* ============================================================
   RigCom v8.0 — include/neon_forge.h
   Snapdragon Vector-Forge: Auto-NEON SIMD Pass
   Detecta bucles for matemáticos en el IR y emite instrucciones
   NEON (vld1q/vmulq/vaddq) para registros v0-v31 de 128 bits.
   Multiplicador de throughput: 4x floats / 8x int16 por ciclo.
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef NEON_FORGE_H
#define NEON_FORGE_H
#include <stdbool.h>
#include <stdint.h>
#include "rigir.h"
#include "rigctx.h"

/* Resultado de análisis de vectorizabilidad */
typedef struct NeonCandidate {
    char         fn_name[128];
    uint32_t     loop_count;     /* bucles detectados */
    uint32_t     vec_count;      /* bucles vectorizados */
    uint32_t     scalar_ops;     /* ops escalares reemplazadas */
    uint32_t     neon_ops;       /* instrucciones NEON emitidas */
    bool         has_fpu;        /* usa floats */
    bool         has_int_simd;   /* usa enteros 16/32 */
    struct NeonCandidate *next;
} NeonCandidate;

typedef struct {
    NeonCandidate *candidates;
    uint32_t       n_total;
    uint32_t       n_vectorized;
    uint32_t       speedup_4x;   /* funciones con 4x teórico */
    double         est_speedup;  /* speedup global estimado */
} NeonForgeResult;

/* ── API pública ── */

/* Analiza y vectoriza un IRModule completo */
NeonForgeResult* neon_forge_run  (IRModule *mod, RigCtx *ctx);

/* Emite resultado como evento WebSocket */
void             neon_forge_emit (NeonForgeResult *r, RigCtx *ctx,
                                  const char *file);

/* Genera header ARM64 NEON para el proyecto del usuario.
   Escribe riglib_neon.h en el directorio indicado. */
bool             neon_forge_gen_header(const char *out_dir);

void             neon_forge_free (NeonForgeResult *r);

/* Thread arg para análisis async */
typedef struct {
    char      file[512];
    RigCtx   *ctx;
    void     *srv;  /* WsServer* — evitar circular include */
} NeonForgeArg;
void* neon_forge_thread(void *arg);

#endif /* NEON_FORGE_H */
