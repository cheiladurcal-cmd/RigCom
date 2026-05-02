/* ============================================================
   RigCom v8.0 — include/rigir.h
   RigIR — SSA Intermediate Representation
   Middle layer: AST → RigIR → ARM64 / LLVM
   ============================================================ */
#ifndef RIGIR_H
#define RIGIR_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* ── IR value types ─────────────────────────────────────────── */
typedef enum {
    IRTY_VOID = 0,
    IRTY_I1, IRTY_I8, IRTY_I16, IRTY_I32, IRTY_I64,
    IRTY_U8, IRTY_U16, IRTY_U32, IRTY_U64,
    IRTY_F32, IRTY_F64,
    IRTY_PTR,
} IRType;

/* ── IR opcodes ─────────────────────────────────────────────── */
typedef enum {
    /* Arithmetic (integer) */
    IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
    IR_NEG, IR_NOT,
    /* Arithmetic (float) */
    IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV,
    /* Bitwise */
    IR_AND, IR_OR, IR_XOR, IR_SHL, IR_SHR,
    /* Comparison → i1 */
    IR_CMP_EQ, IR_CMP_NE, IR_CMP_LT, IR_CMP_LE, IR_CMP_GT, IR_CMP_GE,
    /* Memory */
    IR_ALLOCA,   /* dst = alloca(type, n)                */
    IR_LOAD,     /* dst = *ptr                           */
    IR_STORE,    /* *ptr = val                           */
    IR_GEP,      /* dst = getelementptr(base, index)     */
    /* Control */
    IR_BR,       /* unconditional branch                 */
    IR_COND_BR,  /* cond_br cond, true_lbl, false_lbl    */
    IR_CALL,     /* dst = call fn(args...)               */
    IR_RET,      /* ret val (or void)                    */
    /* Data movement */
    IR_MOV,      /* dst = src                            */
    IR_IMOV,     /* dst = immediate_int64                */
    IR_FMOV,     /* dst = immediate_f64                  */
    /* PHI (SSA join) */
    IR_PHI,
    /* Conversions */
    IR_TRUNC, IR_ZEXT, IR_SEXT, IR_FPEXT, IR_FTRUNC,
    IR_ITOF, IR_FTOI,
    IR_PTOI, IR_ITOP,
} IROp;

/* ── SSA virtual register ───────────────────────────────────── */
typedef struct {
    uint32_t id;     /* 0 = unused  */
    IRType   type;
} IRVal;

#define IRV_NONE  ((IRVal){0, IRTY_VOID})
#define IRV(n, t) ((IRVal){(n), (t)})

/* ── Single three-address instruction ─────────────────────────*/
#define IR_MAX_ARGS 8

typedef struct IRInstr IRInstr;
struct IRInstr {
    IROp     op;
    IRVal    dst;
    IRVal    src[2];
    /* Extra operands for CALL, PHI, COND_BR */
    IRVal   *extra;      /* heap-allocated, NULL if unused */
    uint32_t n_extra;
    /* Immediates */
    int64_t  imm_i;
    double   imm_f;
    /* Labels / function names */
    char    *label0;     /* branch target or callee name   */
    char    *label1;     /* COND_BR false target           */
    IRType   alloca_ty;  /* ALLOCA: element type           */
};

/* ── Basic block ────────────────────────────────────────────── */
typedef struct IRBlock IRBlock;
struct IRBlock {
    char      *label;
    IRInstr   *instrs;
    uint32_t   n_instrs;
    uint32_t   cap;
    IRBlock   *next;     /* linked list of blocks in function */
};

/* ── Function ───────────────────────────────────────────────── */
typedef struct {
    char     *name;
    IRBlock  *entry;
    IRBlock  *current;
    IRBlock  *last;
    uint32_t  n_blocks;
    uint32_t  next_vreg;  /* virtual register counter */
    /* Parameters */
    IRVal    *params;
    uint32_t  n_params;
    IRType    ret_type;
} IRFunc;

/* ── Module (collection of functions) ──────────────────────── */
typedef struct {
    IRFunc  **funcs;
    uint32_t  n_funcs;
    uint32_t  cap;
    /* global string literals */
    char    **strings;
    uint32_t  n_strings;
} IRModule;

/* ── Lifecycle ──────────────────────────────────────────────── */
IRModule* irmod_new      (void);
void      irmod_free     (IRModule *m);
IRFunc*   irmod_add_func (IRModule *m, const char *name, IRType ret);
void      irfunc_free    (IRFunc *f);

/* ── Block management ───────────────────────────────────────── */
IRBlock* irfunc_new_block (IRFunc *f, const char *label);
void     irfunc_set_block (IRFunc *f, IRBlock *b);

/* ── Emit instructions ──────────────────────────────────────── */
IRVal ir_emit_bin  (IRFunc *f, IROp op, IRVal a, IRVal b, IRType ty);
IRVal ir_emit_un   (IRFunc *f, IROp op, IRVal a, IRType ty);
IRVal ir_emit_imm  (IRFunc *f, int64_t imm, IRType ty);
IRVal ir_emit_fimm (IRFunc *f, double  imm);
IRVal ir_emit_alloca(IRFunc *f, IRType element_ty, uint32_t n);
void  ir_emit_store(IRFunc *f, IRVal val, IRVal ptr);
IRVal ir_emit_load (IRFunc *f, IRVal ptr, IRType ty);
void  ir_emit_br   (IRFunc *f, const char *label);
void  ir_emit_cond_br(IRFunc *f, IRVal cond, const char *t, const char *fl);
IRVal ir_emit_call (IRFunc *f, const char *fn, IRVal *args, uint32_t n, IRType ret);
void  ir_emit_ret  (IRFunc *f, IRVal val);
void  ir_emit_ret_void(IRFunc *f);
IRVal ir_emit_phi  (IRFunc *f, IRVal *vals, const char **labels, uint32_t n, IRType ty);
IRVal ir_emit_gep  (IRFunc *f, IRVal base, IRVal idx, IRType elem_ty);
IRVal ir_emit_cmp  (IRFunc *f, IROp cmp_op, IRVal a, IRVal b);
IRVal ir_emit_mov  (IRFunc *f, IRVal src);
IRVal ir_next_vreg (IRFunc *f, IRType ty);

/* ── Optimization passes ────────────────────────────────────── */
void ir_pass_const_fold  (IRFunc *f);
void ir_pass_dce         (IRFunc *f);  /* dead code elimination */
void ir_pass_copy_prop   (IRFunc *f);  /* copy propagation      */

/* ── Export ─────────────────────────────────────────────────── */
char* ir_to_llvm    (IRModule *m);          /* → LLVM .ll text   */
char* irfunc_to_asm (IRFunc *f);            /* → ARM64 .s text   */
void  ir_dump       (IRModule *m, FILE *fp);/* debug dump        */

#endif /* RIGIR_H */
