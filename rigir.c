#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — src/rigir.c
   RigIR: SSA Intermediate Representation
   Lifecycle, instruction emission, opt passes, IR export
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/rigir.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════
   MODULE
   ═══════════════════════════════════════════════════════════════ */

IRModule* irmod_new(void) {
    IRModule *m = calloc(1, sizeof(IRModule));
    if (!m) return NULL;
    m->cap     = 32;
    m->funcs   = malloc(m->cap * sizeof(IRFunc *));
    m->strings = malloc(32 * sizeof(char *));
    return m;
}

void irmod_free(IRModule *m) {
    if (!m) return;
    for (uint32_t i = 0; i < m->n_funcs; i++)
        irfunc_free(m->funcs[i]);
    free(m->funcs);
    for (uint32_t i = 0; i < m->n_strings; i++)
        free(m->strings[i]);
    free(m->strings);
    free(m);
}

IRFunc* irmod_add_func(IRModule *m, const char *name, IRType ret) {
    if (!m) return NULL;
    if (m->n_funcs >= m->cap) {
        m->cap *= 2;
        IRFunc **tmp = realloc(m->funcs, m->cap * sizeof(IRFunc *));
        if (!tmp) return NULL;
        m->funcs = tmp;
    }
    IRFunc *f = calloc(1, sizeof(IRFunc));
    if (!f) return NULL;
    f->name      = strdup(name);
    f->ret_type  = ret;
    f->next_vreg = 1;   /* 0 = unused */
    m->funcs[m->n_funcs++] = f;
    return f;
}

/* ═══════════════════════════════════════════════════════════════
   FUNCTION / BLOCK
   ═══════════════════════════════════════════════════════════════ */

void irfunc_free(IRFunc *f) {
    if (!f) return;
    free(f->name);
    free(f->params);
    IRBlock *b = f->entry;
    while (b) {
        IRBlock *next = b->next;
        for (uint32_t i = 0; i < b->n_instrs; i++) {
            free(b->instrs[i].extra);
            free(b->instrs[i].label0);
            free(b->instrs[i].label1);
        }
        free(b->instrs);
        free(b->label);
        free(b);
        b = next;
    }
    free(f);
}

IRBlock* irfunc_new_block(IRFunc *f, const char *label) {
    if (!f) return NULL;
    IRBlock *b = calloc(1, sizeof(IRBlock));
    if (!b) return NULL;
    b->label  = strdup(label);
    b->cap    = 64;
    b->instrs = malloc(b->cap * sizeof(IRInstr));

    if (!f->entry) {
        f->entry   = b;
        f->current = b;
        f->last    = b;
    } else {
        f->last->next = b;
        f->last       = b;
    }
    f->n_blocks++;
    return b;
}

void irfunc_set_block(IRFunc *f, IRBlock *b) {
    if (f) f->current = b;
}

/* ═══════════════════════════════════════════════════════════════
   VREG ALLOCATION
   ═══════════════════════════════════════════════════════════════ */

IRVal ir_next_vreg(IRFunc *f, IRType ty) {
    IRVal v;
    v.id   = f->next_vreg++;
    v.type = ty;
    return v;
}

/* ═══════════════════════════════════════════════════════════════
   INSTRUCTION EMISSION  (append to f->current block)
   ═══════════════════════════════════════════════════════════════ */

static IRInstr* emit(IRFunc *f) {
    IRBlock *b = f->current;
    if (!b) {
        /* Auto-create entry block */
        b = irfunc_new_block(f, "entry");
    }
    if (b->n_instrs >= b->cap) {
        b->cap  *= 2;
        IRInstr *tmp = realloc(b->instrs, b->cap * sizeof(IRInstr));
        if (!tmp) return NULL;
        b->instrs = tmp;
    }
    IRInstr *in = &b->instrs[b->n_instrs++];
    memset(in, 0, sizeof(IRInstr));
    return in;
}

IRVal ir_emit_bin(IRFunc *f, IROp op, IRVal a, IRVal b, IRType ty) {
    IRVal dst = ir_next_vreg(f, ty);
    IRInstr *in = emit(f);
    if (!in) return IRV_NONE;
    in->op     = op;
    in->dst    = dst;
    in->src[0] = a;
    in->src[1] = b;
    return dst;
}

IRVal ir_emit_un(IRFunc *f, IROp op, IRVal a, IRType ty) {
    IRVal dst = ir_next_vreg(f, ty);
    IRInstr *in = emit(f);
    if (!in) return IRV_NONE;
    in->op     = op;
    in->dst    = dst;
    in->src[0] = a;
    return dst;
}

IRVal ir_emit_imm(IRFunc *f, int64_t imm, IRType ty) {
    IRVal dst = ir_next_vreg(f, ty);
    IRInstr *in = emit(f);
    if (!in) return IRV_NONE;
    in->op    = IR_IMOV;
    in->dst   = dst;
    in->imm_i = imm;
    return dst;
}

IRVal ir_emit_fimm(IRFunc *f, double imm) {
    IRVal dst = ir_next_vreg(f, IRTY_F64);
    IRInstr *in = emit(f);
    if (!in) return IRV_NONE;
    in->op    = IR_FMOV;
    in->dst   = dst;
    in->imm_f = imm;
    return dst;
}

IRVal ir_emit_alloca(IRFunc *f, IRType element_ty, uint32_t n) {
    IRVal dst = ir_next_vreg(f, IRTY_PTR);
    IRInstr *in = emit(f);
    if (!in) return IRV_NONE;
    in->op       = IR_ALLOCA;
    in->dst      = dst;
    in->alloca_ty = element_ty;
    in->imm_i    = (int64_t)n;
    return dst;
}

void ir_emit_store(IRFunc *f, IRVal val, IRVal ptr) {
    IRInstr *in = emit(f);
    if (!in) return;
    in->op     = IR_STORE;
    in->src[0] = val;
    in->src[1] = ptr;
}

IRVal ir_emit_load(IRFunc *f, IRVal ptr, IRType ty) {
    IRVal dst = ir_next_vreg(f, ty);
    IRInstr *in = emit(f);
    if (!in) return IRV_NONE;
    in->op     = IR_LOAD;
    in->dst    = dst;
    in->src[0] = ptr;
    return dst;
}

void ir_emit_br(IRFunc *f, const char *label) {
    IRInstr *in = emit(f);
    if (!in) return;
    in->op     = IR_BR;
    in->label0 = strdup(label);
}

void ir_emit_cond_br(IRFunc *f, IRVal cond, const char *t, const char *fl) {
    IRInstr *in = emit(f);
    if (!in) return;
    in->op     = IR_COND_BR;
    in->src[0] = cond;
    in->label0 = strdup(t);
    in->label1 = strdup(fl);
}

IRVal ir_emit_call(IRFunc *f, const char *fn, IRVal *args,
                    uint32_t n, IRType ret) {
    IRVal dst = (ret == IRTY_VOID) ? IRV_NONE : ir_next_vreg(f, ret);
    IRInstr *in = emit(f);
    if (!in) return IRV_NONE;
    in->op     = IR_CALL;
    in->dst    = dst;
    in->label0 = strdup(fn);
    if (n > 0) {
        in->extra   = malloc(n * sizeof(IRVal));
        in->n_extra = n;
        memcpy(in->extra, args, n * sizeof(IRVal));
    }
    return dst;
}

void ir_emit_ret(IRFunc *f, IRVal val) {
    IRInstr *in = emit(f);
    if (!in) return;
    in->op     = IR_RET;
    in->src[0] = val;
}

void ir_emit_ret_void(IRFunc *f) {
    IRInstr *in = emit(f);
    if (!in) return;
    in->op = IR_RET;
    in->dst = IRV_NONE;
}

IRVal ir_emit_phi(IRFunc *f, IRVal *vals, const char **labels,
                   uint32_t n, IRType ty) {
    IRVal dst = ir_next_vreg(f, ty);
    IRInstr *in = emit(f);
    if (!in) return IRV_NONE;
    in->op      = IR_PHI;
    in->dst     = dst;
    in->extra   = malloc(n * sizeof(IRVal));
    in->n_extra = n;
    memcpy(in->extra, vals, n * sizeof(IRVal));
    /* Labels stored as label0 (comma-joined for simplicity) */
    if (n > 0 && labels) {
        size_t total = 0;
        for (uint32_t i = 0; i < n; i++) total += strlen(labels[i]) + 2;
        char *lbuf = malloc(total + 1);
        lbuf[0] = '\0';
        for (uint32_t i = 0; i < n; i++) {
            strcat(lbuf, labels[i]);
            if (i + 1 < n) strcat(lbuf, ",");
        }
        in->label0 = lbuf;
    }
    return dst;
}

IRVal ir_emit_gep(IRFunc *f, IRVal base, IRVal idx, IRType elem_ty) {
    IRVal dst = ir_next_vreg(f, IRTY_PTR);
    IRInstr *in = emit(f);
    if (!in) return IRV_NONE;
    in->op       = IR_GEP;
    in->dst      = dst;
    in->src[0]   = base;
    in->src[1]   = idx;
    in->alloca_ty = elem_ty;
    return dst;
}

IRVal ir_emit_cmp(IRFunc *f, IROp cmp_op, IRVal a, IRVal b) {
    return ir_emit_bin(f, cmp_op, a, b, IRTY_I1);
}

IRVal ir_emit_mov(IRFunc *f, IRVal src) {
    IRVal dst = ir_next_vreg(f, src.type);
    IRInstr *in = emit(f);
    if (!in) return IRV_NONE;
    in->op     = IR_MOV;
    in->dst    = dst;
    in->src[0] = src;
    return dst;
}

/* ═══════════════════════════════════════════════════════════════
   OPTIMIZATION PASSES
   ═══════════════════════════════════════════════════════════════ */

/* Constant folding: replace binary ops where both operands are
   produced by IR_IMOV instructions in the same basic block.
   Algorithm: single-pass forward scan per block building a
   vreg→imm table, then fold matching arithmetic into a new IMOV.
   Covers: ADD SUB MUL DIV MOD  AND OR XOR SHL SHR  CMP_EQ/NE/LT/LE/GT/GE */
void ir_pass_const_fold(IRFunc *f) {
    if (!f) return;

    for (IRBlock *b = f->entry; b; b = b->next) {
        uint32_t max_vr = f->next_vreg;
        if (max_vr == 0) continue;

        int64_t *imm_val = calloc(max_vr, sizeof(int64_t));
        bool    *known   = calloc(max_vr, sizeof(bool));
        if (!imm_val || !known) { free(imm_val); free(known); continue; }

        for (uint32_t i = 0; i < b->n_instrs; i++) {
            IRInstr *in = &b->instrs[i];

            /* Record immediates produced by IMOV */
            if (in->op == IR_IMOV && in->dst.id < max_vr) {
                imm_val[in->dst.id] = in->imm_i;
                known  [in->dst.id] = true;
                continue;
            }

            /* Propagate MOV of known vreg */
            if (in->op == IR_MOV && in->dst.id < max_vr
                && in->src[0].id < max_vr && known[in->src[0].id]) {
                imm_val[in->dst.id] = imm_val[in->src[0].id];
                known  [in->dst.id] = true;
                /* Convert MOV to IMOV */
                in->op    = IR_IMOV;
                in->imm_i = imm_val[in->dst.id];
                continue;
            }

            /* ── Integer arithmetic ─────────────────────── */
            bool is_arith = (in->op >= IR_ADD && in->op <= IR_MOD);
            /* ── Bitwise ────────────────────────────────── */
            bool is_bitwise = (in->op >= IR_AND && in->op <= IR_SHR);
            /* ── Comparison ─────────────────────────────── */
            bool is_cmp = (in->op >= IR_CMP_EQ && in->op <= IR_CMP_GE);

            if (is_arith || is_bitwise || is_cmp) {
                uint32_t l = in->src[0].id;
                uint32_t r = in->src[1].id;
                if (l < max_vr && r < max_vr && known[l] && known[r]) {
                    int64_t lv = imm_val[l];
                    int64_t rv = imm_val[r];
                    int64_t result = 0;
                    bool    ok     = true;
                    switch (in->op) {
                        /* Arithmetic */
                        case IR_ADD:    result = lv + rv;               break;
                        case IR_SUB:    result = lv - rv;               break;
                        case IR_MUL:    result = lv * rv;               break;
                        case IR_DIV:    ok = rv != 0;
                                        if (ok) { result = lv / rv; } else { break; }
                        case IR_MOD:    ok = rv != 0;
                                        if (ok) { result = lv % rv; } else { break; }
                        /* Bitwise */
                        case IR_AND:    result = lv & rv;               break;
                        case IR_OR:     result = lv | rv;               break;
                        case IR_XOR:    result = lv ^ rv;               break;
                        case IR_SHL:    ok = rv >= 0 && rv < 64;
                                        if (ok) { result = lv << rv; } else { break; }
                        case IR_SHR:    ok = rv >= 0 && rv < 64;
                                        if (ok) { result = (int64_t)((uint64_t)lv >> rv); } else { break; }
                        /* Comparisons → boolean i1 */
                        case IR_CMP_EQ: result = (lv == rv) ? 1 : 0;   break;
                        case IR_CMP_NE: result = (lv != rv) ? 1 : 0;   break;
                        case IR_CMP_LT: result = (lv <  rv) ? 1 : 0;   break;
                        case IR_CMP_LE: result = (lv <= rv) ? 1 : 0;   break;
                        case IR_CMP_GT: result = (lv >  rv) ? 1 : 0;   break;
                        case IR_CMP_GE: result = (lv >= rv) ? 1 : 0;   break;
                        default:        ok = false;                      break;
                    }
                    if (ok) {
                        in->op        = IR_IMOV;
                        in->imm_i     = result;
                        in->src[0].id = 0;
                        in->src[1].id = 0;
                        if (in->dst.id < max_vr) {
                            imm_val[in->dst.id] = result;
                            known  [in->dst.id] = true;
                        }
                    }
                }
            }

            /* Unary NEG/NOT on known constant */
            if ((in->op == IR_NEG || in->op == IR_NOT)
                && in->src[0].id < max_vr && known[in->src[0].id]) {
                int64_t sv = imm_val[in->src[0].id];
                int64_t result = (in->op == IR_NEG) ? -sv : ~sv;
                in->op        = IR_IMOV;
                in->imm_i     = result;
                in->src[0].id = 0;
                if (in->dst.id < max_vr) {
                    imm_val[in->dst.id] = result;
                    known  [in->dst.id] = true;
                }
            }
        }

        free(imm_val);
        free(known);
    }
}

/* Dead code elimination: remove instructions whose dst is never used */
void ir_pass_dce(IRFunc *f) {
    if (!f) return;
    /* Count uses for each vreg */
    uint32_t max_vreg = f->next_vreg;
    uint32_t *use_count = calloc(max_vreg, sizeof(uint32_t));
    if (!use_count) return;

    /* Mark uses */
    for (IRBlock *b = f->entry; b; b = b->next) {
        for (uint32_t i = 0; i < b->n_instrs; i++) {
            IRInstr *in = &b->instrs[i];
            if (in->src[0].id < max_vreg) use_count[in->src[0].id]++;
            if (in->src[1].id < max_vreg) use_count[in->src[1].id]++;
            for (uint32_t j = 0; j < in->n_extra; j++)
                if (in->extra[j].id < max_vreg)
                    use_count[in->extra[j].id]++;
        }
    }

    /* Nullify dead pure instructions */
    for (IRBlock *b = f->entry; b; b = b->next) {
        for (uint32_t i = 0; i < b->n_instrs; i++) {
            IRInstr *in = &b->instrs[i];
            if (in->dst.id == 0) continue; /* no dst */
            if (in->op == IR_CALL || in->op == IR_STORE ||
                in->op == IR_RET  || in->op == IR_BR    ||
                in->op == IR_COND_BR) continue; /* side effects */
            if (use_count[in->dst.id] == 0) {
                /* Mark dead: convert to NOP via IR_MOV 0→0 */
                in->op = IR_MOV;
                in->dst.id = 0;
                in->src[0].id = 0;
            }
        }
    }
    free(use_count);
}

/* Copy propagation: replace uses of MOV dst with src */
void ir_pass_copy_prop(IRFunc *f) {
    if (!f) return;
    uint32_t max_vreg = f->next_vreg;
    uint32_t *alias = malloc(max_vreg * sizeof(uint32_t));
    if (!alias) return;
    for (uint32_t i = 0; i < max_vreg; i++) alias[i] = i; /* identity */

    /* Build alias table from MOV instructions */
    for (IRBlock *b = f->entry; b; b = b->next) {
        for (uint32_t i = 0; i < b->n_instrs; i++) {
            IRInstr *in = &b->instrs[i];
            if (in->op == IR_MOV && in->dst.id < max_vreg &&
                in->src[0].id < max_vreg) {
                alias[in->dst.id] = in->src[0].id;
            }
        }
    }

    /* Apply aliases */
    for (IRBlock *b = f->entry; b; b = b->next) {
        for (uint32_t i = 0; i < b->n_instrs; i++) {
            IRInstr *in = &b->instrs[i];
            if (in->src[0].id < max_vreg)
                in->src[0].id = alias[in->src[0].id];
            if (in->src[1].id < max_vreg)
                in->src[1].id = alias[in->src[1].id];
            for (uint32_t j = 0; j < in->n_extra; j++)
                if (in->extra[j].id < max_vreg)
                    in->extra[j].id = alias[in->extra[j].id];
        }
    }
    free(alias);
}

/* ═══════════════════════════════════════════════════════════════
   EXPORT: LLVM IR TEXT
   ═══════════════════════════════════════════════════════════════ */

static const char* ir_type_ll(IRType t) {
    switch (t) {
        case IRTY_VOID: return "void";
        case IRTY_I1:   return "i1";
        case IRTY_I8:   return "i8";
        case IRTY_I16:  return "i16";
        case IRTY_I32:  return "i32";
        case IRTY_I64:  return "i64";
        case IRTY_U8:   return "i8";
        case IRTY_U16:  return "i16";
        case IRTY_U32:  return "i32";
        case IRTY_U64:  return "i64";
        case IRTY_F32:  return "float";
        case IRTY_F64:  return "double";
        case IRTY_PTR:  return "ptr";
        default:        return "i64";
    }
}

static const char* irop_ll(IROp op) {
    switch (op) {
        case IR_ADD: return "add";   case IR_SUB: return "sub";
        case IR_MUL: return "mul";   case IR_DIV: return "sdiv";
        case IR_MOD: return "srem";  case IR_AND: return "and";
        case IR_OR:  return "or";    case IR_XOR: return "xor";
        case IR_SHL: return "shl";   case IR_SHR: return "ashr";
        case IR_FADD: return "fadd"; case IR_FSUB: return "fsub";
        case IR_FMUL: return "fmul"; case IR_FDIV: return "fdiv";
        default: return "add";
    }
}

static const char* ircmp_ll(IROp op) {
    switch (op) {
        case IR_CMP_EQ: return "icmp eq";  case IR_CMP_NE: return "icmp ne";
        case IR_CMP_LT: return "icmp slt"; case IR_CMP_LE: return "icmp sle";
        case IR_CMP_GT: return "icmp sgt"; case IR_CMP_GE: return "icmp sge";
        default: return "icmp eq";
    }
}

/* Dynamic string builder */
typedef struct { char *buf; size_t pos; size_t cap; } SB;
static void sb_ensure(SB *s, size_t need) {
    if (s->pos + need >= s->cap) {
        s->cap = (s->cap + need) * 2;
        s->buf = realloc(s->buf, s->cap);
    }
}
static void sb_printf(SB *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sb_ensure(s, 512);
    int n = vsnprintf(s->buf + s->pos, s->cap - s->pos, fmt, ap);
    va_end(ap);
    if (n > 0) s->pos += (size_t)n;
}

#include <stdarg.h>

char* ir_to_llvm(IRModule *m) {
    if (!m) return NULL;
    SB sb;
    sb.pos = 0; sb.cap = 4096;
    sb.buf = malloc(sb.cap);
    if (!sb.buf) return NULL;

    sb_printf(&sb, "; RigCom v8.0 — LLVM IR\n");
    sb_printf(&sb, "target triple = \"aarch64-linux-android\"\n\n");

    /* String constants */
    for (uint32_t i = 0; i < m->n_strings; i++) {
        size_t len = strlen(m->strings[i]) + 1;
        sb_printf(&sb, "@.str.%u = private constant [%zu x i8] c\"%s\\00\"\n",
                  i, len, m->strings[i]);
    }
    if (m->n_strings) sb_printf(&sb, "\n");

    for (uint32_t fi = 0; fi < m->n_funcs; fi++) {
        IRFunc *f = m->funcs[fi];
        sb_printf(&sb, "define %s @%s(",
                  ir_type_ll(f->ret_type), f->name);
        for (uint32_t pi = 0; pi < f->n_params; pi++) {
            if (pi) sb_printf(&sb, ", ");
            sb_printf(&sb, "%s %%p%u",
                      ir_type_ll(f->params[pi].type), f->params[pi].id);
        }
        sb_printf(&sb, ") {\n");

        for (IRBlock *b = f->entry; b; b = b->next) {
            sb_printf(&sb, "%s:\n", b->label);
            for (uint32_t i = 0; i < b->n_instrs; i++) {
                IRInstr *in = &b->instrs[i];
                sb_printf(&sb, "  ");
                switch (in->op) {
                    case IR_IMOV:
                        sb_printf(&sb, "%%%u = add %s 0, %lld\n",
                                  in->dst.id, ir_type_ll(in->dst.type),
                                  (long long)in->imm_i);
                        break;
                    case IR_FMOV:
                        sb_printf(&sb, "%%%u = fadd double 0.0, %g\n",
                                  in->dst.id, in->imm_f);
                        break;
                    case IR_MOV:
                        if (in->dst.id == 0) { sb_printf(&sb, "; nop\n"); break; }
                        sb_printf(&sb, "%%%u = add %s %%%u, 0\n",
                                  in->dst.id, ir_type_ll(in->dst.type),
                                  in->src[0].id);
                        break;
                    case IR_ADD: case IR_SUB: case IR_MUL:
                    case IR_DIV: case IR_MOD:
                    case IR_AND: case IR_OR: case IR_XOR:
                    case IR_SHL: case IR_SHR:
                    case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
                        sb_printf(&sb, "%%%u = %s %s %%%u, %%%u\n",
                                  in->dst.id, irop_ll(in->op),
                                  ir_type_ll(in->dst.type),
                                  in->src[0].id, in->src[1].id);
                        break;
                    case IR_CMP_EQ: case IR_CMP_NE:
                    case IR_CMP_LT: case IR_CMP_LE:
                    case IR_CMP_GT: case IR_CMP_GE:
                        sb_printf(&sb, "%%%u = %s %s %%%u, %%%u\n",
                                  in->dst.id, ircmp_ll(in->op),
                                  ir_type_ll(in->src[0].type),
                                  in->src[0].id, in->src[1].id);
                        break;
                    case IR_ALLOCA:
                        sb_printf(&sb, "%%%u = alloca %s, i32 %lld\n",
                                  in->dst.id, ir_type_ll(in->alloca_ty),
                                  (long long)in->imm_i);
                        break;
                    case IR_LOAD:
                        sb_printf(&sb, "%%%u = load %s, ptr %%%u\n",
                                  in->dst.id, ir_type_ll(in->dst.type),
                                  in->src[0].id);
                        break;
                    case IR_STORE:
                        sb_printf(&sb, "store %s %%%u, ptr %%%u\n",
                                  ir_type_ll(in->src[0].type),
                                  in->src[0].id, in->src[1].id);
                        break;
                    case IR_GEP:
                        sb_printf(&sb, "%%%u = getelementptr %s, ptr %%%u, i64 %%%u\n",
                                  in->dst.id, ir_type_ll(in->alloca_ty),
                                  in->src[0].id, in->src[1].id);
                        break;
                    case IR_BR:
                        sb_printf(&sb, "br label %%%s\n",
                                  in->label0 ? in->label0 : "exit");
                        break;
                    case IR_COND_BR:
                        sb_printf(&sb, "br i1 %%%u, label %%%s, label %%%s\n",
                                  in->src[0].id,
                                  in->label0 ? in->label0 : "then",
                                  in->label1 ? in->label1 : "else");
                        break;
                    case IR_CALL: {
                        if (in->dst.id)
                            sb_printf(&sb, "%%%u = ", in->dst.id);
                        sb_printf(&sb, "call %s @%s(",
                                  ir_type_ll(in->dst.type),
                                  in->label0 ? in->label0 : "unknown");
                        for (uint32_t j = 0; j < in->n_extra; j++) {
                            if (j) sb_printf(&sb, ", ");
                            sb_printf(&sb, "%s %%%u",
                                      ir_type_ll(in->extra[j].type),
                                      in->extra[j].id);
                        }
                        sb_printf(&sb, ")\n");
                        break;
                    }
                    case IR_RET:
                        if (in->src[0].id)
                            sb_printf(&sb, "ret %s %%%u\n",
                                      ir_type_ll(in->src[0].type),
                                      in->src[0].id);
                        else
                            sb_printf(&sb, "ret void\n");
                        break;
                    default:
                        sb_printf(&sb, "; unknown op %d\n", (int)in->op);
                        break;
                }
            }
        }
        sb_printf(&sb, "}\n\n");
    }

    sb_ensure(&sb, 1);
    sb.buf[sb.pos] = '\0';
    return sb.buf;
}

/* ═══════════════════════════════════════════════════════════════
   EXPORT: ARM64 ASSEMBLY TEXT
   ═══════════════════════════════════════════════════════════════ */

char* irfunc_to_asm(IRFunc *f) {
    if (!f) return NULL;
    SB sb;
    sb.pos = 0; sb.cap = 2048;
    sb.buf = malloc(sb.cap);
    if (!sb.buf) return NULL;

    sb_printf(&sb, "\t.text\n");
    sb_printf(&sb, "\t.global %s\n", f->name);
    sb_printf(&sb, "\t.type %s, %%function\n", f->name);
    sb_printf(&sb, "%s:\n", f->name);
    sb_printf(&sb, "\tstp x29, x30, [sp, #-16]!\n");
    sb_printf(&sb, "\tmov x29, sp\n");

    /* Simplified: emit per block */
    for (IRBlock *b = f->entry; b; b = b->next) {
        sb_printf(&sb, ".%s_%s:\n", f->name, b->label);
        for (uint32_t i = 0; i < b->n_instrs; i++) {
            IRInstr *in = &b->instrs[i];
            switch (in->op) {
                case IR_IMOV:
                    sb_printf(&sb, "\tmov x%u, #%lld\n",
                              in->dst.id % 19, (long long)in->imm_i);
                    break;
                case IR_ADD:
                    sb_printf(&sb, "\tadd x%u, x%u, x%u\n",
                              in->dst.id%19, in->src[0].id%19, in->src[1].id%19);
                    break;
                case IR_SUB:
                    sb_printf(&sb, "\tsub x%u, x%u, x%u\n",
                              in->dst.id%19, in->src[0].id%19, in->src[1].id%19);
                    break;
                case IR_MUL:
                    sb_printf(&sb, "\tmul x%u, x%u, x%u\n",
                              in->dst.id%19, in->src[0].id%19, in->src[1].id%19);
                    break;
                case IR_RET:
                    sb_printf(&sb, "\tldp x29, x30, [sp], #16\n");
                    sb_printf(&sb, "\tret\n");
                    break;
                case IR_BR:
                    sb_printf(&sb, "\tb .%s_%s\n",
                              f->name, in->label0 ? in->label0 : "exit");
                    break;
                case IR_COND_BR:
                    sb_printf(&sb, "\tcbnz x%u, .%s_%s\n",
                              in->src[0].id%19, f->name,
                              in->label0 ? in->label0 : "then");
                    sb_printf(&sb, "\tb .%s_%s\n",
                              f->name, in->label1 ? in->label1 : "else");
                    break;
                default:
                    sb_printf(&sb, "\t// op %d\n", (int)in->op);
                    break;
            }
        }
    }
    sb_printf(&sb, "\t.size %s, .-%s\n", f->name, f->name);

    sb_ensure(&sb, 1);
    sb.buf[sb.pos] = '\0';
    return sb.buf;
}

/* ═══════════════════════════════════════════════════════════════
   DEBUG DUMP
   ═══════════════════════════════════════════════════════════════ */

void ir_dump(IRModule *m, FILE *fp) {
    if (!m || !fp) return;
    fprintf(fp, "=== RigIR Module (%u funcs) ===\n", m->n_funcs);
    for (uint32_t fi = 0; fi < m->n_funcs; fi++) {
        IRFunc *f = m->funcs[fi];
        fprintf(fp, "\nfunc %s → %s (%u blocks, %u vregs):\n",
                f->name, ir_type_ll(f->ret_type),
                f->n_blocks, f->next_vreg - 1);
        for (IRBlock *b = f->entry; b; b = b->next) {
            fprintf(fp, "  [%s]\n", b->label);
            for (uint32_t i = 0; i < b->n_instrs; i++) {
                IRInstr *in = &b->instrs[i];
                if (in->dst.id)
                    fprintf(fp, "    %%%u = op%d\n", in->dst.id, (int)in->op);
                else
                    fprintf(fp, "    op%d\n", (int)in->op);
            }
        }
    }
}
