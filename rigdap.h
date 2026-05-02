/* ============================================================
   RigCom v8.0 — include/rigdap.h
   Motor de Rayos X — Debugger real con ptrace (Fase 1)
   DAP sobre WebSocket + ptrace ARM64 nativo.
   Breakpoints reales BRK #0, registros x0-x18 / d0-d31,
   inspección de variables via SymTable.
   Compatible con VS Code "type":"rigcom",port:4711
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef RIGDAP_H
#define RIGDAP_H

#include <stdbool.h>
#include <stdint.h>
#include "rigctx.h"
#include "wsserver.h"

/* Punto de quiebre */
typedef struct Breakpoint {
    char     file[256];
    uint32_t line;
    uint64_t addr;        /* dirección de memoria (si se inyectó BRK) */
    bool     verified;
    bool     has_addr;    /* true si se inyectó BRK en addr */
    struct Breakpoint *next;
} Breakpoint;

/* Estado del proceso bajo debug */
typedef enum {
    DAP_STATE_IDLE = 0,
    DAP_STATE_RUNNING,
    DAP_STATE_PAUSED,
    DAP_STATE_TERMINATED,
} DapState;

/* ── Captura de registros ARM64 / fallback x86 ───────────── */
typedef struct {
    uint64_t x[19];    /* x0-x18 (general purpose)      */
    uint64_t sp;       /* stack pointer                  */
    uint64_t pc;       /* program counter                */
    uint64_t psr;      /* processor state                */
    uint64_t d[32];    /* d0-d31 (FP, bits de double)   */
    bool     valid;    /* false si ptrace no disponible  */
} DapRegState;

/* Sesión DAP */
typedef struct {
    RigCtx      *ctx;
    WsServer    *ws;             /* reusa el WS de rigcom ui */
    DapState     state;
    Breakpoint  *breakpoints;
    char         exec_path[512]; /* binario bajo debug */
    int          proc_pid;       /* PID del proceso hijo */
    bool         initialized;
    DapRegState  last_regs;      /* último estado de registros capturado */
} DapSession;

/* ── Ciclo de vida ─────────────────────────────────────── */
DapSession* dap_session_new  (RigCtx *ctx, WsServer *ws);
void        dap_session_free (DapSession *s);

/* ── Comandos DAP (cliente → adaptador) ───────────────── */
bool dap_cmd_initialize      (DapSession *s);
bool dap_cmd_launch          (DapSession *s, const char *program,
                               bool stop_at_entry);
bool dap_cmd_set_breakpoints (DapSession *s, const char *file,
                               uint32_t *lines, uint32_t n_lines);
bool dap_cmd_continue        (DapSession *s);
bool dap_cmd_pause           (DapSession *s);
bool dap_cmd_next            (DapSession *s);  /* step over  */
bool dap_cmd_step_in         (DapSession *s);
bool dap_cmd_step_out        (DapSession *s);
bool dap_cmd_terminate       (DapSession *s);
bool dap_cmd_disconnect      (DapSession *s);

/* ── Eventos DAP (adaptador → cliente, por WS) ─────────── */
void dap_event_stopped       (DapSession *s, const char *reason,
                               const char *file, uint32_t line);
void dap_event_continued     (DapSession *s);
void dap_event_terminated    (DapSession *s);
void dap_event_output        (DapSession *s, const char *category,
                               const char *msg);

/* ── Handler de mensajes WS para DAP ──────────────────── */
void dap_on_ws_message (DapSession *s, const char *json, size_t len);

#endif /* RIGDAP_H */
