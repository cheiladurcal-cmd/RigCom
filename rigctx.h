/* ============================================================
   RigCom v8.0 — include/rigctx.h
   Global compiler context + memory pool + WS pipeline bridge
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef RIGCTX_H
#define RIGCTX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "toml.h"
#include "error.h"

/* Forward declaration — avoid circular include */
struct WsServer;

/* ── Build configuration (loaded from rigcom.toml) ── */
typedef struct {
    char     *project_name;
    char     *project_version;
    char     *target;              /* "aarch64-linux-android" */
    char     *optimize;            /* "O0"|"O1"|"O2"|"O3"     */
    char    **defines;
    uint32_t  n_defines;
    char    **sources;             /* glob patterns             */
    uint32_t  n_sources;
    char    **include_dirs;
    uint32_t  n_include_dirs;
    char     *output_exec;
    bool      use_native_backend;
    uint32_t  n_cores;
    bool      enable_bootstrap;
    bool      enable_bench;
} RigConfig;

/* ── Arena / memory pool ── */
typedef struct {
    void    **ptrs;
    uint32_t  capacity;
    uint32_t  count;
} RigMemPool;

/* ── Global compiler context ── */
typedef struct RigCtx {
    RigConfig   config;
    RigMemPool  mem;
    TomlDoc     toml;
    char       *project_root;
    uint64_t    build_id;          /* cache invalidation key  */

    /* LLVM context (opaque) */
    void       *llvm_ctx;

    /* Error / warning counts (accumulated per build) */
    uint32_t    error_count;
    uint32_t    warning_count;

    /* ── WebSocket live bridge ── */
    struct WsServer *ws;           /* NULL when UI not running */

    /* ── Pipeline state (real-time broadcast) ── */
    uint32_t    files_total;
    uint32_t    files_done;
    char        current_phase[32]; /* "lex","parse","typecheck","ir","backend" */
    double      last_build_time;   /* seconds */
    bool        build_ok;          /* result of last build     */
} RigCtx;

/* ── Lifecycle ── */
RigCtx* rigctx_new  (const char *project_root);
void    rigctx_free (RigCtx *ctx);

/* ── Allocation — all ptrs auto-freed by rigctx_free ── */
void*   rigctx_alloc  (RigCtx *ctx, size_t size);
char*   rigctx_strdup (RigCtx *ctx, const char *s);
void    rigctx_free_all(RigCtx *ctx);

/* ── Config ── */
bool    rigctx_load_config(RigCtx *ctx, const char *toml_path);

/* ── Source discovery ── */
uint32_t rigctx_find_sources(RigCtx *ctx, char ***out_paths);

/* ── WS bridge ── */
/* Attach a running WsServer so pipeline events are broadcast live */
void rigctx_attach_ws(RigCtx *ctx, struct WsServer *ws);

/* Emit a JSON event to the WS dashboard (no-op if ws == NULL) */
void rigctx_ws_emit(RigCtx *ctx, const char *json_event);

/* Convenience: emit phase transition */
void rigctx_ws_phase(RigCtx *ctx, const char *phase,
                      const char *file, uint32_t file_idx);

/* Convenience: emit all errors from log as individual events */
void rigctx_ws_emit_errors(RigCtx *ctx, RigErrorLog *log);

#endif /* RIGCTX_H */
