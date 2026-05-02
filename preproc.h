/* ============================================================
   RigCom v8.0 — include/preproc.h
   Autonomous C preprocessor:
     #include expansion · #define / #undef
     #if / #ifdef / #ifndef / #elif / #else / #endif
     Macro expansion (object-like + function-like)
   ============================================================ */
#ifndef PREPROC_H
#define PREPROC_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "rigctx.h"
#include "error.h"

/* ── Macro ──────────────────────────────────────────────────── */
typedef struct {
    char      *name;
    char      *body;
    char     **params;
    uint32_t   n_params;
    bool       func_like;
} Macro;

/* ── Search paths ─────────────────────────────────────────── */
#define MAX_INCLUDE_PATHS 64

typedef struct {
    RigCtx      *ctx;
    RigErrorLog *log;
    Macro        macros[4096];
    uint32_t     n_macros;
    const char  *include_paths[MAX_INCLUDE_PATHS];
    uint32_t     n_paths;
    int          cond_depth;   /* #if nesting */
    bool         cond_true[64];/* condition stack */
} Preproc;

/* ── API ────────────────────────────────────────────────────── */
void  preproc_init      (Preproc *pp, RigCtx *ctx, RigErrorLog *log);
void  preproc_add_path  (Preproc *pp, const char *path);
void  preproc_define    (Preproc *pp, const char *name, const char *body);

/* Process source → returns heap-allocated expanded text */
char* preproc_run       (Preproc *pp, const char *src,
                          size_t src_len, const char *filename);

void  preproc_free      (Preproc *pp);

#endif /* PREPROC_H */
