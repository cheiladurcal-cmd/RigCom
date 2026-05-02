/* ============================================================
   RigCom v8.0 — src/backend.c
   Backend: ARM64 native + LLVM (via clang), register allocator
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/backend.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Backend selection ──────────────────────────────────────── */
BackendKind backend_select(const RigCtx *ctx) {
    if (!ctx) return BACKEND_LLVM;
    return ctx->config.use_native_backend
           ? BACKEND_ARM64_NATIVE
           : BACKEND_LLVM;
}

/* ═══════════════════════════════════════════════════════════════
   REGISTER ALLOCATOR  (Linear Scan, simplified)
   ═══════════════════════════════════════════════════════════════ */

/* Physical registers available for allocation: x0–x18 (19 regs) */
#define PHYS_REGS 19u

typedef struct {
    uint32_t vreg;
    uint32_t first_def;  /* instruction index of first def */
    uint32_t last_use;   /* instruction index of last use  */
} LiveRange;

RegAlloc* regalloc_run(IRFunc *f) {
    if (!f) return NULL;

    uint32_t max_vreg = f->next_vreg;
    RegAlloc *ra = calloc(1, sizeof(RegAlloc));
    if (!ra) return NULL;

    ra->n_vregs      = max_vreg;
    ra->virt_to_phys = malloc(max_vreg * sizeof(uint32_t));
    ra->spill_offsets = malloc(max_vreg * sizeof(int32_t));

    for (uint32_t i = 0; i < max_vreg; i++) {
        ra->virt_to_phys[i]  = UINT32_MAX; /* unassigned */
        ra->spill_offsets[i] = 0;
    }

    /* Build live ranges */
    LiveRange *lr = calloc(max_vreg, sizeof(LiveRange));
    if (!lr) { regalloc_free(ra); return NULL; }

    uint32_t instr_idx = 0;
    for (IRBlock *b = f->entry; b; b = b->next) {
        for (uint32_t i = 0; i < b->n_instrs; i++, instr_idx++) {
            IRInstr *in = &b->instrs[i];
            if (in->dst.id && in->dst.id < max_vreg) {
                if (!lr[in->dst.id].first_def)
                    lr[in->dst.id].first_def = instr_idx;
                lr[in->dst.id].vreg = in->dst.id;
            }
            for (int s = 0; s < 2; s++) {
                uint32_t id = in->src[s].id;
                if (id && id < max_vreg)
                    lr[id].last_use = instr_idx;
            }
        }
    }

    /* Linear scan allocation */
    bool phys_free[PHYS_REGS];
    for (uint32_t i = 0; i < PHYS_REGS; i++) phys_free[i] = true;

    /* Simple first-fit */
    for (uint32_t v = 1; v < max_vreg; v++) {
        if (!lr[v].first_def && !lr[v].last_use) continue;

        /* Find free physical register */
        bool allocated = false;
        for (uint32_t p = 0; p < PHYS_REGS; p++) {
            if (phys_free[p]) {
                ra->virt_to_phys[v] = p;
                phys_free[p] = false;
                allocated = true;
                break;
            }
        }

        if (!allocated) {
            /* Spill: assign stack slot */
            ra->spill_offsets[v] = (int32_t)(ra->n_spills * 8 + 16);
            ra->n_spills++;
            /* virt_to_phys stays UINT32_MAX = spilled */
        }
    }

    /* Compute frame size (16-byte aligned) */
    uint32_t spill_bytes = ra->n_spills * 8;
    ra->frame_size = (int32_t)((spill_bytes + 15) & ~15u);

    free(lr);
    return ra;
}

void regalloc_free(RegAlloc *ra) {
    if (!ra) return;
    free(ra->virt_to_phys);
    free(ra->spill_offsets);
    free(ra);
}

/* ═══════════════════════════════════════════════════════════════
   ARM64 NATIVE BACKEND
   ═══════════════════════════════════════════════════════════════ */

bool backend_arm64_compile(IRModule *m, RigCtx *ctx, const char *out_asm) {
    if (!m || !out_asm) return false;
    (void)ctx; /* reservado para futuras opciones de target */

    FILE *fp = fopen(out_asm, "w");
    if (!fp) {
        fprintf(stderr, "[ARM64] No se pudo abrir '%s' para escritura\n",
                out_asm);
        return false;
    }

    fprintf(fp, "// RigCom v8.0 — ARM64 Assembly\n");
    fprintf(fp, "// Target: aarch64-linux-android\n");
    fprintf(fp, "// φ = 1.6180339887498948482\n\n");
    fprintf(fp, "\t.arch armv8-a\n");
    fprintf(fp, "\t.text\n\n");

    for (uint32_t fi = 0; fi < m->n_funcs; fi++) {
        IRFunc *f = m->funcs[fi];

        /* Register allocation */
        RegAlloc *ra = regalloc_run(f);

        fprintf(fp, "\t.global %s\n", f->name);
        fprintf(fp, "\t.type %s, %%function\n", f->name);
        fprintf(fp, "%s:\n", f->name);

        /* Prologue */
        int32_t frame = ra ? ra->frame_size + 16 : 16;
        frame = (frame + 15) & ~15; /* align */
        fprintf(fp, "\tsub sp, sp, #%d\n", frame);
        fprintf(fp, "\tstp x29, x30, [sp, #%d]\n", frame - 16);
        fprintf(fp, "\tadd x29, sp, #%d\n", frame - 16);

        /* Emit function parameters: x0..xN */
        for (uint32_t pi = 0; pi < f->n_params && pi < 8; pi++) {
            uint32_t vreg = f->params[pi].id;
            if (ra && vreg < ra->n_vregs && ra->virt_to_phys[vreg] != UINT32_MAX) {
                uint32_t preg = ra->virt_to_phys[vreg];
                if (preg != pi)
                    fprintf(fp, "\tmov x%u, x%u\n", preg, pi);
            }
        }

        /* Emit blocks */
        for (IRBlock *b = f->entry; b; b = b->next) {
            fprintf(fp, ".L%s_%s:\n", f->name, b->label);

            for (uint32_t ii = 0; ii < b->n_instrs; ii++) {
                IRInstr *in = &b->instrs[ii];

                /* Helper: vreg → register name */
#define XREG(v) (ra && (v) < ra->n_vregs && \
                 ra->virt_to_phys[(v)] != UINT32_MAX \
                 ? ra->virt_to_phys[(v)] : (v) % 19u)

                uint32_t dst  = in->dst.id;
                uint32_t src0 = in->src[0].id;
                uint32_t src1 = in->src[1].id;

                switch (in->op) {
                    case IR_IMOV:
                        if (in->imm_i >= 0 && in->imm_i <= 0xFFFF)
                            fprintf(fp, "\tmov x%u, #%lld\n",
                                    XREG(dst), (long long)in->imm_i);
                        else {
                            fprintf(fp, "\tmov x%u, #%lld\n",
                                    XREG(dst),
                                    (long long)(in->imm_i & 0xFFFF));
                            if (in->imm_i > 0xFFFF)
                                fprintf(fp, "\tmovk x%u, #%lld, lsl #16\n",
                                        XREG(dst),
                                        (long long)((in->imm_i >> 16) & 0xFFFF));
                        }
                        break;
                    case IR_ADD:
                        fprintf(fp, "\tadd x%u, x%u, x%u\n",
                                XREG(dst), XREG(src0), XREG(src1));
                        break;
                    case IR_SUB:
                        fprintf(fp, "\tsub x%u, x%u, x%u\n",
                                XREG(dst), XREG(src0), XREG(src1));
                        break;
                    case IR_MUL:
                        fprintf(fp, "\tmul x%u, x%u, x%u\n",
                                XREG(dst), XREG(src0), XREG(src1));
                        break;
                    case IR_DIV:
                        fprintf(fp, "\tsdiv x%u, x%u, x%u\n",
                                XREG(dst), XREG(src0), XREG(src1));
                        break;
                    case IR_MOD:
                        /* a % b = a - (a/b)*b */
                        fprintf(fp, "\tsdiv x%u, x%u, x%u\n",
                                XREG(dst), XREG(src0), XREG(src1));
                        fprintf(fp, "\tmsub x%u, x%u, x%u, x%u\n",
                                XREG(dst), XREG(dst), XREG(src1), XREG(src0));
                        break;
                    case IR_AND:
                        fprintf(fp, "\tand x%u, x%u, x%u\n",
                                XREG(dst), XREG(src0), XREG(src1));
                        break;
                    case IR_OR:
                        fprintf(fp, "\torr x%u, x%u, x%u\n",
                                XREG(dst), XREG(src0), XREG(src1));
                        break;
                    case IR_XOR:
                        fprintf(fp, "\teor x%u, x%u, x%u\n",
                                XREG(dst), XREG(src0), XREG(src1));
                        break;
                    case IR_SHL:
                        fprintf(fp, "\tlsl x%u, x%u, x%u\n",
                                XREG(dst), XREG(src0), XREG(src1));
                        break;
                    case IR_SHR:
                        fprintf(fp, "\tasr x%u, x%u, x%u\n",
                                XREG(dst), XREG(src0), XREG(src1));
                        break;
                    case IR_NEG:
                        fprintf(fp, "\tneg x%u, x%u\n",
                                XREG(dst), XREG(src0));
                        break;
                    case IR_NOT:
                        fprintf(fp, "\tmvn x%u, x%u\n",
                                XREG(dst), XREG(src0));
                        break;
                    case IR_CMP_EQ: case IR_CMP_NE:
                    case IR_CMP_LT: case IR_CMP_LE:
                    case IR_CMP_GT: case IR_CMP_GE: {
                        fprintf(fp, "\tcmp x%u, x%u\n",
                                XREG(src0), XREG(src1));
                        const char *cset =
                            (in->op == IR_CMP_EQ) ? "eq" :
                            (in->op == IR_CMP_NE) ? "ne" :
                            (in->op == IR_CMP_LT) ? "lt" :
                            (in->op == IR_CMP_LE) ? "le" :
                            (in->op == IR_CMP_GT) ? "gt" : "ge";
                        fprintf(fp, "\tcset x%u, %s\n", XREG(dst), cset);
                        break;
                    }
                    case IR_ALLOCA: {
                        int32_t off = 0;
                        if (ra && in->dst.id < ra->n_vregs)
                            off = ra->spill_offsets[in->dst.id];
                        fprintf(fp, "\tadd x%u, sp, #%d\n", XREG(dst), off);
                        break;
                    }
                    case IR_STORE:
                        fprintf(fp, "\tstr x%u, [x%u]\n",
                                XREG(src0), XREG(src1));
                        break;
                    case IR_LOAD:
                        fprintf(fp, "\tldr x%u, [x%u]\n",
                                XREG(dst), XREG(src0));
                        break;
                    case IR_GEP:
                        fprintf(fp, "\tadd x%u, x%u, x%u, lsl #3\n",
                                XREG(dst), XREG(src0), XREG(src1));
                        break;
                    case IR_CALL: {
                        /* Move args to x0..x7 */
                        for (uint32_t ai = 0; ai < in->n_extra && ai < 8; ai++) {
                            uint32_t av = in->extra[ai].id;
                            if (XREG(av) != ai)
                                fprintf(fp, "\tmov x%u, x%u\n", ai, XREG(av));
                        }
                        fprintf(fp, "\tbl %s\n",
                                in->label0 ? in->label0 : "unknown");
                        if (dst && XREG(dst) != 0)
                            fprintf(fp, "\tmov x%u, x0\n", XREG(dst));
                        break;
                    }
                    case IR_BR:
                        fprintf(fp, "\tb .L%s_%s\n",
                                f->name, in->label0 ? in->label0 : "exit");
                        break;
                    case IR_COND_BR:
                        fprintf(fp, "\tcbnz x%u, .L%s_%s\n",
                                XREG(src0), f->name,
                                in->label0 ? in->label0 : "then");
                        fprintf(fp, "\tb .L%s_%s\n",
                                f->name, in->label1 ? in->label1 : "else");
                        break;
                    case IR_MOV:
                        if (dst && XREG(dst) != XREG(src0))
                            fprintf(fp, "\tmov x%u, x%u\n",
                                    XREG(dst), XREG(src0));
                        break;
                    case IR_RET:
                        if (src0 && XREG(src0) != 0)
                            fprintf(fp, "\tmov x0, x%u\n", XREG(src0));
                        fprintf(fp, "\tldp x29, x30, [sp, #%d]\n",
                                frame - 16);
                        fprintf(fp, "\tadd sp, sp, #%d\n", frame);
                        fprintf(fp, "\tret\n");
                        break;
                    default:
                        fprintf(fp, "\t// unimplemented op %d\n", (int)in->op);
                        break;
                }
#undef XREG
            }
        }

        fprintf(fp, "\t.size %s, .-%s\n\n", f->name, f->name);
        regalloc_free(ra);
    }

    fclose(fp);
    printf("  [ARM64] Ensamblado → %s\n", out_asm);
    return true;
}

/* ═══════════════════════════════════════════════════════════════
   LLVM BACKEND  (writes .ll, invokes clang)
   ═══════════════════════════════════════════════════════════════ */

bool backend_llvm_compile(IRModule *m, RigCtx *ctx, const char *out_obj) {
    if (!m || !out_obj) return false;

    /* Write .ll file */
    char ll_path[512];
    snprintf(ll_path, sizeof(ll_path), "%s.ll", out_obj);

    char *ll_text = ir_to_llvm(m);
    if (!ll_text) return false;

    FILE *fp = fopen(ll_path, "w");
    if (!fp) { free(ll_text); return false; }
    fputs(ll_text, fp);
    fclose(fp);
    free(ll_text);

    /* Build clang command */
    const char *target  = (ctx && ctx->config.target)
                          ? ctx->config.target
                          : "aarch64-linux-android";
    const char *opt     = (ctx && ctx->config.optimize)
                          ? ctx->config.optimize
                          : "O2";
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "clang -target %s -%s -c %s -o %s",
             target, opt, ll_path, out_obj);

    printf("  [LLVM] %s\n", cmd);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "[LLVM] clang falló (rc=%d)\n", rc);
        return false;
    }
    return true;
}

/* ── Link object files ──────────────────────────────────────── */
bool backend_link(const char **objs, uint32_t n_objs, RigCtx *ctx,
                   const char *out_exec) {
    if (!objs || !n_objs || !out_exec) return false;

    char cmd[4096];
    int  pos = 0;

    const char *target = (ctx && ctx->config.target)
                         ? ctx->config.target
                         : "aarch64-linux-android";

    /* Chequear si estamos en modo APK */
    bool is_apk = (strstr(out_exec, ".apk") != NULL);

    /* Si es APK, el linker debe crear un .so (Shared Library) */
    const char *link_flags = is_apk
        ? "-shared -fPIC -landroid -llog -lEGL -lGLESv3 -lm -lpthread"
        : "-lm -lpthread";

    const char *actual_out = is_apk ? "build/libmain.so" : out_exec;

    pos += snprintf(cmd + pos, sizeof(cmd) - (size_t)pos,
                    "clang -target %s", target);

    for (uint32_t i = 0; i < n_objs && pos < 3800; i++) {
        pos += snprintf(cmd + pos, sizeof(cmd) - (size_t)pos,
                        " %s", objs[i]);
    }
    pos += snprintf(cmd + pos, sizeof(cmd) - (size_t)pos,
                    " -o %s %s", actual_out, link_flags);

    printf("  [LINK] %s\n", cmd);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "[LINK] Enlace falló (rc=%d)\n", rc);
        return false;
    }

    /* Si es un APK, llamar al orquestador */
    if (is_apk) {
        extern bool apk_build(RigCtx *c, const char *so, const char *apk);
        return apk_build(ctx, "build/libmain.so", out_exec);
    }

    return true;
}
