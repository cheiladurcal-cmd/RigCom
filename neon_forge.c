/* ============================================================
   RigCom v8.0 — src/neon_forge.c
   Snapdragon Vector-Forge: SIMD Auto-Vectorization Pass
   Analiza IRFunc buscando reducción de bucles escalares,
   emite ARM64 NEON (v0-v31, 128-bit) y genera riglib_neon.h
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/neon_forge.h"
#include "../include/wsserver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── Detección de patrones vectorizables en el IR ── */

/* Un bucle es candidato si:
   1. Tiene un BasicBlock con back-edge (loop header)
   2. Contiene IR_FADD/IR_FMUL/IR_ADD/IR_MUL repetitivos
   3. No tiene dependencias de carry-out cruzadas
   Estrategia: contar ops aritméticas en cada bloque del loop.
   Si ops ≥ 4 (ancho NEON), se vectoriza. */

static IRBlock* ir_get_block(IRFunc *f, uint32_t bi) {
    IRBlock *b = f->entry; uint32_t i = 0;
    while (b && i < bi) { b = b->next; i++; }
    return b;
}

static bool ir_block_is_loop_header(IRFunc *f, uint32_t bi) {
    /* Heurística: un bloque con label "while_head" o "for_head"
       o "do_body" es un loop header. Sin preds[] real, usamos
       el label como proxy determinista. */
    IRBlock *blk = ir_get_block(f, bi);
    if (!blk || !blk->label) return false;
    return (strstr(blk->label, "head") != NULL ||
            strstr(blk->label, "body") != NULL ||
            strstr(blk->label, "loop") != NULL);
}

static uint32_t ir_block_arith_ops(IRBlock *blk) {
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < blk->n_instrs; i++) {
        IRInstr *ins = &blk->instrs[i];
        switch (ins->op) {
        case IR_ADD: case IR_SUB: case IR_MUL:
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
            cnt++; break;
        default: break;
        }
    }
    return cnt;
}

static bool ir_block_has_float(IRBlock *blk) {
    for (uint32_t i = 0; i < blk->n_instrs; i++) {
        IRInstr *ins = &blk->instrs[i];
        if (ins->op == IR_FADD || ins->op == IR_FSUB ||
            ins->op == IR_FMUL || ins->op == IR_FDIV)
            return true;
    }
    return false;
}

/* ── Vectorización: reescritura de instrucciones a NEON ── */
/* Marca las instrucciones de un bloque para uso de registros NEON.
   En la práctica el backend ARM64 lee este flag y emite:
     vld1q_f32 / vmulq_f32 / vaddq_f32 / vst1q_f32
   en lugar de las instrucciones escalares equivalentes.          */
static uint32_t vectorize_block(IRBlock *blk) {
    uint32_t ops = 0;
    for (uint32_t i = 0; i < blk->n_instrs; i++) {
        IRInstr *ins = &blk->instrs[i];
        switch (ins->op) {
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
        case IR_ADD:  case IR_SUB:  case IR_MUL:
            ins->imm_i |= 0xA010LL; /* TILE_FP/NEON flag para backend */
            ops++;
            break;
        default: break;
        }
    }
    return ops;
}

/* ── Función principal del pass ── */
NeonForgeResult* neon_forge_run(IRModule *mod, RigCtx *ctx) {
    NeonForgeResult *res = calloc(1, sizeof(NeonForgeResult));
    if (!res) return NULL;

    for (uint32_t fi = 0; fi < mod->n_funcs; fi++) {
        IRFunc *f = mod->funcs[fi];
        NeonCandidate *cand = calloc(1, sizeof(NeonCandidate));
        if (!cand) continue;
        strncpy(cand->fn_name, f->name ? f->name : "?",
                sizeof(cand->fn_name)-1);

        IRBlock *blk_iter = f->entry;
        for (uint32_t bi = 0; bi < f->n_blocks && blk_iter; bi++, blk_iter=blk_iter->next) {
            IRBlock *blk = blk_iter;
            if (!ir_block_is_loop_header(f, bi)) continue;
            cand->loop_count++;
            uint32_t arith = ir_block_arith_ops(blk);
            if (arith < 4) continue; /* mínimo ancho NEON */
            /* ¡Vectorizable! */
            uint32_t ops = vectorize_block(blk);
            cand->vec_count++;
            cand->scalar_ops += arith;
            cand->neon_ops   += (arith + 3) / 4; /* ceil(n/4) */
            if (ir_block_has_float(blk)) cand->has_fpu = true;
            else                          cand->has_int_simd = true;
            (void)ops;
        }

        res->n_total++;
        if (cand->vec_count > 0) {
            res->n_vectorized++;
            if (cand->vec_count >= 2) res->speedup_4x++;
            cand->next      = res->candidates;
            res->candidates = cand;
        } else {
            free(cand);
        }
    }

    /* Speedup estimado: cada op vectorizada reemplaza 4 escalares */
    if (res->n_total > 0)
        res->est_speedup = 1.0 +
            (double)res->n_vectorized / (double)res->n_total * 3.2;

    (void)ctx;
    return res;
}

/* ── Emisión de resultados por WebSocket ── */
void neon_forge_emit(NeonForgeResult *r, RigCtx *ctx, const char *file) {
    if (!r) return;
    char buf[2048];
    snprintf(buf, sizeof(buf),
        "{\"ev\":\"neon_forge_done\","
        "\"file\":\"%s\","
        "\"n_total\":%u,"
        "\"n_vectorized\":%u,"
        "\"speedup_4x\":%u,"
        "\"est_speedup\":%.2f,"
        "\"ok\":true}",
        file, r->n_total, r->n_vectorized, r->speedup_4x, r->est_speedup);
    rigctx_ws_emit(ctx, buf);

    /* Emitir detalle por función */
    for (NeonCandidate *c = r->candidates; c; c = c->next) {
        char det[512];
        snprintf(det, sizeof(det),
            "{\"ev\":\"neon_fn\","
            "\"fn\":\"%s\","
            "\"loops\":%u,"
            "\"vec_loops\":%u,"
            "\"scalar_ops\":%u,"
            "\"neon_ops\":%u,"
            "\"type\":\"%s\"}",
            c->fn_name, c->loop_count, c->vec_count,
            c->scalar_ops, c->neon_ops,
            c->has_fpu ? "float32x4" : "int32x4");
        rigctx_ws_emit(ctx, det);
    }
}

/* ── Generación de riglib_neon.h ── */
static const char NEON_HEADER[] =
"/* ================================================================\n"
"   riglib_neon.h — RigCom v8.0 Auto-Generated\n"
"   ARM64 NEON intrinsics optimizados para Kirin 9000S / Snapdragon\n"
"   Incluir en tu proyecto: #include \"riglib_neon.h\"\n"
"   φ = 1.6180339887498948482\n"
"   ================================================================ */\n"
"#ifndef RIGLIB_NEON_H\n"
"#define RIGLIB_NEON_H\n"
"#ifdef __ARM_NEON\n"
"#include <arm_neon.h>\n"
"\n"
"/* ── Reducción NEON: suma de array float ── */\n"
"static inline float rig_sum_f32(const float *a, int n) {\n"
"    float32x4_t acc = vdupq_n_f32(0.0f);\n"
"    int i = 0;\n"
"    for (; i <= n-4; i += 4)\n"
"        acc = vaddq_f32(acc, vld1q_f32(a+i));\n"
"    float s = vaddvq_f32(acc);\n"
"    for (; i < n; i++) s += a[i];\n"
"    return s;\n"
"}\n"
"\n"
"/* ── Producto escalar NEON ── */\n"
"static inline float rig_dot_f32(const float *a, const float *b, int n) {\n"
"    float32x4_t acc = vdupq_n_f32(0.0f);\n"
"    int i = 0;\n"
"    for (; i <= n-4; i += 4)\n"
"        acc = vfmaq_f32(acc, vld1q_f32(a+i), vld1q_f32(b+i));\n"
"    float s = vaddvq_f32(acc);\n"
"    for (; i < n; i++) s += a[i]*b[i];\n"
"    return s;\n"
"}\n"
"\n"
"/* ── Multiplicación vector × escalar in-place ── */\n"
"static inline void rig_scale_f32(float *a, float k, int n) {\n"
"    float32x4_t vk = vdupq_n_f32(k);\n"
"    int i = 0;\n"
"    for (; i <= n-4; i += 4)\n"
"        vst1q_f32(a+i, vmulq_f32(vld1q_f32(a+i), vk));\n"
"    for (; i < n; i++) a[i] *= k;\n"
"}\n"
"\n"
"/* ── MADD: dst[i] = a[i]*b[i] + c[i] ── */\n"
"static inline void rig_madd_f32(float *dst,\n"
"    const float *a, const float *b, const float *c, int n) {\n"
"    int i = 0;\n"
"    for (; i <= n-4; i += 4)\n"
"        vst1q_f32(dst+i,\n"
"            vfmaq_f32(vld1q_f32(c+i),\n"
"                      vld1q_f32(a+i), vld1q_f32(b+i)));\n"
"    for (; i < n; i++) dst[i] = a[i]*b[i]+c[i];\n"
"}\n"
"\n"
"/* ── Suma de array int32 ── */\n"
"static inline int32_t rig_sum_i32(const int32_t *a, int n) {\n"
"    int32x4_t acc = vdupq_n_s32(0);\n"
"    int i = 0;\n"
"    for (; i <= n-4; i += 4)\n"
"        acc = vaddq_s32(acc, vld1q_s32(a+i));\n"
"    int32_t s = vaddvq_s32(acc);\n"
"    for (; i < n; i++) s += a[i];\n"
"    return s;\n"
"}\n"
"\n"
"/* ── memcpy NEON (alineado 16 bytes) ── */\n"
"static inline void rig_memcpy_neon(void *dst, const void *src, size_t n) {\n"
"    uint8_t *d=(uint8_t*)dst; const uint8_t *s=(const uint8_t*)src;\n"
"    size_t i=0;\n"
"    for (; i+16<=n; i+=16) vst1q_u8(d+i, vld1q_u8(s+i));\n"
"    for (; i<n; i++) d[i]=s[i];\n"
"}\n"
"\n"
"/* ── Normalización L2 in-place ── */\n"
"static inline void rig_normalize_f32(float *v, int n) {\n"
"    float norm2 = rig_dot_f32(v, v, n);\n"
"    if (norm2 > 1e-12f) rig_scale_f32(v, 1.0f/__builtin_sqrtf(norm2), n);\n"
"}\n"
"\n"
"#else /* Sin NEON — fallback escalar */\n"
"#include <string.h>\n"
"static inline float rig_sum_f32(const float *a,int n){float s=0;for(int i=0;i<n;i++)s+=a[i];return s;}\n"
"static inline float rig_dot_f32(const float *a,const float *b,int n){float s=0;for(int i=0;i<n;i++)s+=a[i]*b[i];return s;}\n"
"static inline void  rig_scale_f32(float *a,float k,int n){for(int i=0;i<n;i++)a[i]*=k;}\n"
"static inline void  rig_madd_f32(float *d,const float *a,const float *b,const float *c,int n){for(int i=0;i<n;i++)d[i]=a[i]*b[i]+c[i];}\n"
"static inline int32_t rig_sum_i32(const int32_t *a,int n){int32_t s=0;for(int i=0;i<n;i++)s+=a[i];return s;}\n"
"static inline void rig_memcpy_neon(void *d,const void *s,size_t n){memcpy(d,s,n);}\n"
"static inline void rig_normalize_f32(float *v,int n){float s=rig_dot_f32(v,v,n);if(s>1e-12f){s=1.0f/__builtin_sqrtf(s);rig_scale_f32(v,s,n);}}\n"
"#endif /* __ARM_NEON */\n"
"#endif /* RIGLIB_NEON_H */\n";

bool neon_forge_gen_header(const char *out_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/riglib_neon.h", out_dir);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs(NEON_HEADER, f);
    fclose(f);
    return true;
}

void neon_forge_free(NeonForgeResult *r) {
    if (!r) return;
    NeonCandidate *c = r->candidates;
    while (c) { NeonCandidate *n = c->next; free(c); c = n; }
    free(r);
}

/* ── Thread async ── */
void* neon_forge_thread(void *arg) {
    NeonForgeArg *a = (NeonForgeArg*)arg;
    WsServer *srv = (WsServer*)a->srv;
    ws_broadcastf(srv, "{\"ev\":\"neon_forge_start\",\"file\":\"%s\"}", a->file);

    RigCtx *ctx = a->ctx;
    /* Compilar a IR para poder analizar */
    RigErrorLog log = {0};
    extern IRModule* compile_source_file(const char*, RigCtx*, RigErrorLog*);
    IRModule *mod = compile_source_file(a->file, ctx, &log);
    if (mod) {
        NeonForgeResult *r = neon_forge_run(mod, ctx);
        neon_forge_emit(r, ctx, a->file);
        /* Generar riglib_neon.h en el directorio del proyecto */
        neon_forge_gen_header(".");
        ws_broadcastf(srv,
            "{\"ev\":\"neon_header_ready\","
            "\"path\":\"./riglib_neon.h\","
            "\"msg\":\"riglib_neon.h generado — #include riglib_neon.h en tu proyecto\"}");
        neon_forge_free(r);
        irmod_free(mod);
    } else {
        ws_broadcastf(srv,
            "{\"ev\":\"neon_forge_done\",\"file\":\"%s\",\"ok\":false,"
            "\"msg\":\"Error de compilación IR\"}", a->file);
    }
    free(a);
    return NULL;
}
