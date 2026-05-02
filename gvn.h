/* ============================================================
   RigCom v8.0 — include/gvn.h
   GVN + ARM64 tiling + NEON Auto-Vectorización (Fase 2)
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef GVN_H
#define GVN_H

#include "rigir.h"
#include <stdbool.h>
#include <stdint.h>

/* ── GVN: elimina cálculos redundantes inter-bloque ─────── */
void ir_pass_gvn(IRFunc *f);

/* ── ARM64 instruction tiling (MADD, NEON, FP) ──────────── */
void ir_pass_arm64_tile(IRFunc *f);

/* ── NEON Auto-Vectorización (Fase 2) ───────────────────── */
/* Detecta loops sobre arrays y emite instrucciones SIMD.
   Retorna número de loops vectorizados. */
typedef struct {
    uint32_t loops_detected;    /* loops candidatos encontrados */
    uint32_t loops_vectorized;  /* loops realmente vectorizados */
    uint32_t insns_replaced;    /* instrucciones escalares → SIMD */
    bool     used_neon;         /* true si se emitieron SIMD reales */
} NeonVecResult;

NeonVecResult ir_pass_neon_vectorize(IRFunc *f);

/* Genera texto de instrucciones NEON para un bloque vectorizado */
int  neon_emit_load4s(char *buf, size_t sz, const char *vreg_name,
                       const char *base_reg);
int  neon_emit_add4s (char *buf, size_t sz, const char *dst,
                       const char *src0, const char *src1);
int  neon_emit_store4s(char *buf, size_t sz, const char *vreg_name,
                        const char *base_reg);

/* ── Float backend: d0-d31 ──────────────────────────────── */
const char* fp_reg_name(uint32_t vreg);

/* ── GVN internal structures ─────────────────────────────── */
typedef struct GvnEntry {
    IROp        op;
    uint32_t    src0_vn;
    uint32_t    src1_vn;
    int64_t     imm;
    uint32_t    canonical;
    struct GvnEntry *next;
} GvnEntry;

#define GVN_HASH_SZ 256
typedef struct {
    GvnEntry  *table[GVN_HASH_SZ];
    uint32_t  *vn_map;
    uint32_t   n_vregs;
    uint32_t   next_vn;
} GvnTable;

GvnTable* gvn_table_new(uint32_t n_vregs);
void      gvn_table_free(GvnTable *t);

/* Marcas de tiling ARM64 en imm_i */
#define TILE_MADD       0xA001LL
#define TILE_MSUB       0xA002LL
#define TILE_MNEG       0xA003LL
#define TILE_FP         0xA010LL
#define TILE_NEON_LD1   0xA020LL   /* LD1 {v0.4s}, [x0]  */
#define TILE_NEON_ST1   0xA021LL   /* ST1 {v0.4s}, [x0]  */
#define TILE_NEON_ADD   0xA022LL   /* ADD v1.4s, v0.4s, v2.4s */
#define TILE_NEON_MUL   0xA023LL   /* MUL v1.4s, v0.4s, v2.4s */
#define TILE_NEON_FMA   0xA024LL   /* FMLA v0.4s, v1.4s, v2.4s */

#endif /* GVN_H */
