/* ============================================================
   RigCom v8.0 — include/backend.h
   Backend selector: ARM64 native | LLVM
   ============================================================ */
#ifndef BACKEND_H
#define BACKEND_H

#include "rigctx.h"
#include "rigir.h"
#include <stdbool.h>

typedef enum {
    BACKEND_LLVM         = 0,
    BACKEND_ARM64_NATIVE = 1,
} BackendKind;

/* ── Auto-select based on config ── */
BackendKind backend_select(const RigCtx *ctx);

/* ── ARM64 native backend ── */
/* Emits real ARM64 assembly (.s) from IRFunc */
bool backend_arm64_compile(IRModule *m, RigCtx *ctx, const char *out_asm);

/* ── LLVM backend ── */
/* Writes .ll file and invokes clang to produce object */
bool backend_llvm_compile(IRModule *m, RigCtx *ctx, const char *out_obj);

/* ── Link object files ── */
bool backend_link(const char **objs, uint32_t n_objs, RigCtx *ctx,
                  const char *out_exec);

/* ── Register allocator (used by ARM64 backend) ── */
typedef struct {
    uint32_t *virt_to_phys;  /* virtual_reg → physical_reg (UINT32_MAX = spilled) */
    uint32_t  n_vregs;
    uint32_t  n_spills;
    int32_t  *spill_offsets; /* stack offset for spilled vreg */
    int32_t   frame_size;
} RegAlloc;

RegAlloc* regalloc_run (IRFunc *f);
void      regalloc_free(RegAlloc *ra);

#endif /* BACKEND_H */
