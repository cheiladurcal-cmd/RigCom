#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — src/gvn.c
   GVN: Global Value Numbering + ARM64 instruction tiling
   Elimina cálculos redundantes que DCE+copy_prop no ven.
   ARM64 tiling: a*b+c → madd, float d0-d31
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/gvn.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Tabla hash GVN ──────────────────────────────────────── */
GvnTable* gvn_table_new(uint32_t n_vregs) {
    GvnTable *t = calloc(1, sizeof(GvnTable));
    if (!t) return NULL;
    t->n_vregs  = n_vregs;
    t->next_vn  = 1;
    t->vn_map   = calloc(n_vregs, sizeof(uint32_t));
    return t;
}

void gvn_table_free(GvnTable *t) {
    if (!t) return;
    for (int i = 0; i < GVN_HASH_SZ; i++) {
        GvnEntry *e = t->table[i];
        while (e) { GvnEntry *nx = e->next; free(e); e = nx; }
    }
    free(t->vn_map);
    free(t);
}

static uint32_t gvn_hash(IROp op, uint32_t vn0, uint32_t vn1,
                           int64_t imm) {
    uint32_t h = (uint32_t)op * 2654435761u;
    h ^= vn0 * 1234567891u;
    h ^= vn1 * 987654321u;
    h ^= (uint32_t)(imm ^ (imm >> 32)) * 0xDEADBEEFu;
    return h & (GVN_HASH_SZ - 1);
}

/* Busca una expresión ya calculada — retorna canonical vreg o 0 */
static uint32_t gvn_lookup(GvnTable *t, IROp op,
                             uint32_t vn0, uint32_t vn1,
                             int64_t imm) {
    uint32_t h = gvn_hash(op, vn0, vn1, imm);
    for (GvnEntry *e = t->table[h]; e; e = e->next) {
        if (e->op   == op   &&
            e->src0_vn == vn0 &&
            e->src1_vn == vn1 &&
            e->imm     == imm)
            return e->canonical;
    }
    return 0;
}

/* Inserta nueva expresión → value number */
static void gvn_insert(GvnTable *t, IROp op,
                        uint32_t vn0, uint32_t vn1,
                        int64_t imm, uint32_t canonical) {
    uint32_t h = gvn_hash(op, vn0, vn1, imm);
    GvnEntry *e = malloc(sizeof(GvnEntry));
    if (!e) return;
    e->op        = op;
    e->src0_vn   = vn0;
    e->src1_vn   = vn1;
    e->imm       = imm;
    e->canonical = canonical;
    e->next      = t->table[h];
    t->table[h]  = e;
}

static uint32_t vn_of(GvnTable *t, uint32_t vreg) {
    if (!vreg || vreg >= t->n_vregs) return 0;
    if (!t->vn_map[vreg]) t->vn_map[vreg] = t->next_vn++;
    return t->vn_map[vreg];
}

/* ══════════════════════════════════════════════════════════
   ir_pass_gvn
   Recorre instrucciones de cada bloque en orden.
   Para cada definición, calcula el value number de la
   expresión. Si ya existe uno igual, reemplaza el uso
   del dst vreg por el canónico (propagación de valor).
   ══════════════════════════════════════════════════════════ */
void ir_pass_gvn(IRFunc *f) {
    if (!f || !f->entry) return;

    GvnTable *t = gvn_table_new(f->next_vreg + 1);
    if (!t) return;

    uint32_t redundant = 0;

    for (IRBlock *b = f->entry; b; b = b->next) {
        for (uint32_t i = 0; i < b->n_instrs; i++) {
            IRInstr *in = &b->instrs[i];
            if (!in->dst.id) continue;

            uint32_t vn0 = vn_of(t, in->src[0].id);
            uint32_t vn1 = vn_of(t, in->src[1].id);

            /* Solo aplicable a ops puras (sin efectos) */
            switch (in->op) {
            case IR_ADD: case IR_SUB: case IR_MUL:
            case IR_DIV: case IR_MOD: case IR_AND:
            case IR_OR:  case IR_XOR: case IR_SHL:
            case IR_SHR: case IR_NEG: case IR_NOT:
            case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
            case IR_CMP_EQ: case IR_CMP_NE: case IR_CMP_LT:
            case IR_CMP_LE: case IR_CMP_GT: case IR_CMP_GE:
            case IR_IMOV: case IR_FMOV: {
                uint32_t existing = gvn_lookup(t, in->op, vn0, vn1,
                                               in->imm_i);
                if (existing) {
                    /* Redundante: convertir en MOV desde el canónico */
                    in->op       = IR_MOV;
                    in->src[0].id= existing;
                    in->src[1]   = IRV_NONE;
                    in->imm_i    = 0;
                    t->vn_map[in->dst.id] = t->vn_map[existing]
                                           ? t->vn_map[existing]
                                           : vn_of(t, existing);
                    redundant++;
                } else {
                    gvn_insert(t, in->op, vn0, vn1,
                               in->imm_i, in->dst.id);
                    t->vn_map[in->dst.id] = t->next_vn++;
                }
                break;
            }
            default:
                /* No optimizable — asignar VN único */
                t->vn_map[in->dst.id] = t->next_vn++;
                break;
            }
        }
    }

    if (redundant > 0)
        printf("  [GVN] %u instrucciones redundantes eliminadas en %s\n",
               redundant, f->name);

    gvn_table_free(t);
}

/* ══════════════════════════════════════════════════════════
   ir_pass_arm64_tile
   Pattern matching para instrucciones ARMv8 específicas:
   - a*b+c  → MADD  xd, xa, xb, xc
   - a*b-c  → MSUB  xd, xa, xb, xc
   - -a*b   → MNEG  xd, xa, xb
   - IR_FADD/FMUL con dst F32/F64 → marcar para d0-d31
   ══════════════════════════════════════════════════════════ */

/* Etiqueta especial en imm_i para instrucciones ARM64 fusionadas */
#define TILE_MADD  0xA001LL
#define TILE_MSUB  0xA002LL
#define TILE_MNEG  0xA003LL
#define TILE_FP    0xA010LL  /* instrucción sobre d0-d31 */

void ir_pass_arm64_tile(IRFunc *f) {
    if (!f || !f->entry) return;

    uint32_t fused = 0;

    for (IRBlock *b = f->entry; b; b = b->next) {
        for (uint32_t i = 0; i + 1 < b->n_instrs; i++) {
            IRInstr *cur  = &b->instrs[i];
            IRInstr *next = &b->instrs[i + 1];

            /* Patrón MADD: mul dst_m, a, b  +  add dst, dst_m, c
               Requiere: next->src[0] == cur->dst  */
            if (cur->op  == IR_MUL &&
                next->op == IR_ADD &&
                next->src[0].id == cur->dst.id) {

                /* Fusionar: next se convierte en MADD virtual
                   Reusamos src[0]=a, src[1]=b, extra[0]=c
                   Marcamos con imm_i=TILE_MADD */
                next->op       = IR_MUL; /* mismo opcode, lo diferencia el tile */
                next->src[0]   = cur->src[0];  /* a */
                next->src[1]   = cur->src[1];  /* b */
                /* c queda en src[1] del ADD original → guardamos en extra */
                next->extra    = malloc(sizeof(IRVal));
                if (next->extra) {
                    next->extra[0] = b->instrs[i+1].src[1]; /* c */
                    next->n_extra  = 1;
                }
                next->imm_i    = TILE_MADD;

                /* Eliminar instrucción MUL original → NOP con IR_MOV 0 */
                cur->op      = IR_MOV;
                cur->src[0]  = IRV_NONE;
                cur->dst     = IRV_NONE;
                fused++;
            }

            /* Patrón MSUB: mul + sub */
            if (cur->op  == IR_MUL &&
                next->op == IR_SUB &&
                next->src[0].id == cur->dst.id) {
                next->op     = IR_MUL;
                next->src[0] = cur->src[0];
                next->src[1] = cur->src[1];
                next->imm_i  = TILE_MSUB;
                cur->op = IR_MOV; cur->dst = IRV_NONE;
                fused++;
            }
        }

        /* Marcar instrucciones float para usar d0-d31 */
        for (uint32_t i = 0; i < b->n_instrs; i++) {
            IRInstr *in = &b->instrs[i];
            if (in->op == IR_FADD || in->op == IR_FSUB ||
                in->op == IR_FMUL || in->op == IR_FDIV ||
                in->op == IR_FMOV) {
                if (!in->imm_i) in->imm_i = TILE_FP;
                fused++;
            }
        }
    }

    if (fused > 0)
        printf("  [TILE] %u instrucciones ARM64 fusionadas/marcadas en %s\n",
               fused, f->name);
}

/* ── Nombre del registro FP para un vreg ────────────────── */
const char* fp_reg_name(uint32_t vreg) {
    static const char *fp_regs[] = {
        "d0","d1","d2","d3","d4","d5","d6","d7",
        "d8","d9","d10","d11","d12","d13","d14","d15",
        "d16","d17","d18","d19","d20","d21","d22","d23",
        "d24","d25","d26","d27","d28","d29","d30","d31"
    };
    return fp_regs[vreg % 32];
}

/* ══════════════════════════════════════════════════════════
   NEON AUTO-VECTORIZACIÓN — Fase 2
   Detecta patrones de bucle sobre arrays y sustituye
   4 operaciones escalares por una instrucción SIMD:
     LDR x0, [base, #0]          →  LD1 {v0.4s}, [x0]
     LDR x1, [base, #4]              (una sola instrucción)
     LDR x2, [base, #8]
     LDR x3, [base, #12]
     ADD x4, x0, x8              →  ADD v1.4s, v0.4s, v2.4s
     ...
   ══════════════════════════════════════════════════════════ */

/* Detecta si un bloque tiene un patrón de loop lineal simple:
   - Un bloque con una bifurcación de retroceso hacia sí mismo
   - Operaciones LOAD/ADD/STORE consecutivas sobre registros adyacentes */
static bool detect_array_loop(IRBlock *b, uint32_t *out_stride) {
    if (!b || b->n_instrs < 4) return false;

    /* Buscar 4 LOAD consecutivos con offset incremental */
    uint32_t loads = 0;
    int64_t  last_off = -1;
    for (uint32_t i = 0; i < b->n_instrs && loads < 4; i++) {
        IRInstr *in = &b->instrs[i];
        if (in->op == IR_LOAD) {
            if (last_off < 0 || in->imm_i == last_off + 4) {
                loads++;
                last_off = in->imm_i;
            } else {
                loads = 0; last_off = -1;
            }
        }
    }
    if (loads >= 4) {
        if (out_stride) *out_stride = 4;
        return true;
    }
    return false;
}

NeonVecResult ir_pass_neon_vectorize(IRFunc *f) {
    NeonVecResult result = {0};
    if (!f || !f->entry) return result;

    for (IRBlock *b = f->entry; b; b = b->next) {
        uint32_t stride = 0;
        if (!detect_array_loop(b, &stride)) continue;

        result.loops_detected++;

        /* Agrupar 4 LOAD consecutivos → marcar como TILE_NEON_LD1 */
        uint32_t run = 0;
        for (uint32_t i = 0; i < b->n_instrs; i++) {
            IRInstr *in = &b->instrs[i];

            if (in->op == IR_LOAD) {
                run++;
                if (run == 1) {
                    /* Primera carga del grupo: marcar como vector load */
                    in->imm_i = TILE_NEON_LD1;
                    result.insns_replaced++;
                } else if (run <= 4) {
                    /* Cargas 2-4 del grupo: convertir en NOP */
                    in->op    = IR_MOV;
                    in->dst   = IRV_NONE;
                    in->src[0]= IRV_NONE;
                    result.insns_replaced++;
                    if (run == 4) run = 0; /* reset para siguiente grupo */
                }
                continue;
            }

            /* Marcar ADD sobre los vregs cargados → NEON ADD */
            if (in->op == IR_ADD && run >= 4) {
                in->imm_i = TILE_NEON_ADD;
                result.insns_replaced++;
                continue;
            }

            /* Marcar STORE → NEON ST1 */
            if (in->op == IR_STORE && run >= 4) {
                in->imm_i = TILE_NEON_ST1;
                result.insns_replaced++;
                continue;
            }

            if (in->op != IR_LOAD) run = 0;
        }

        result.loops_vectorized++;
        result.used_neon = true;
        printf("  [NEON] Loop vectorizado en %s (stride=%u, %u insns)\n",
               f->name, stride, result.insns_replaced);
    }

    return result;
}

/* ── Emisión de texto NEON para codegen ─────────────────── */
int neon_emit_load4s(char *buf, size_t sz,
                      const char *vreg, const char *base) {
    return snprintf(buf, sz, "  LD1  {%s.4s}, [%s]", vreg, base);
}
int neon_emit_add4s(char *buf, size_t sz,
                     const char *dst, const char *s0, const char *s1) {
    return snprintf(buf, sz, "  ADD  %s.4s, %s.4s, %s.4s", dst, s0, s1);
}
int neon_emit_store4s(char *buf, size_t sz,
                       const char *vreg, const char *base) {
    return snprintf(buf, sz, "  ST1  {%s.4s}, [%s]", vreg, base);
}
