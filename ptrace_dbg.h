/* ============================================================
   RigCom v8.0 — include/ptrace_dbg.h
   Debugger de Bajo Nivel Real: ptrace() Backend ARM64
   Lee/escribe registros CPU (x0-x30, v0-v31 NEON), memoria
   arbitraria y variables en caliente. GDB embebido en Dashboard.
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef PTRACE_DBG_H
#define PTRACE_DBG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "rigctx.h"
#include "wsserver.h"

/* ── Registros ARM64 completos ── */
typedef struct {
    uint64_t x[31];     /* x0–x30 (x30 = link register) */
    uint64_t sp;        /* stack pointer                 */
    uint64_t pc;        /* program counter               */
    uint64_t pstate;    /* CPSR / NZCV                   */
    /* NEON / FP: v0–v31, cada uno 128 bits (2 × uint64_t) */
    uint64_t v[32][2];
} Arm64Regs;

/* ── Breakpoint ── */
typedef struct PtraceBreakpoint {
    uint64_t addr;
    uint32_t orig_instr;  /* instrucción original de 4 bytes */
    bool     active;
    char     sym_name[128];
    uint32_t source_line;
    char     source_file[256];
    struct PtraceBreakpoint *next;
} PtraceBreakpoint;

/* ── Watchpoint (hardware breakpoint sobre dirección de memoria) ── */
typedef struct PtraceWatchpoint {
    uint64_t addr;
    size_t   size;        /* 1, 2, 4, u 8 bytes           */
    bool     on_read;
    bool     on_write;
    struct PtraceWatchpoint *next;
} PtraceWatchpoint;

/* ── Frame de pila ── */
typedef struct StackFrame {
    uint64_t pc;
    uint64_t sp;
    char     fn_name[128];
    char     file[256];
    uint32_t line;
    struct StackFrame *next;
} StackFrame;

/* ── Variable en memoria (leída desde proceso) ── */
typedef struct DebugVar {
    char     name[128];
    uint64_t addr;
    uint8_t  raw[16];     /* hasta 16 bytes de valor      */
    size_t   size;
    char     type_str[64];
    char     value_str[64]; /* representación legible       */
    struct DebugVar *next;
} DebugVar;

/* ── Estado del proceso bajo depuración ── */
typedef enum {
    DBG_STATE_DETACHED = 0,
    DBG_STATE_RUNNING,
    DBG_STATE_STOPPED,    /* en breakpoint o señal        */
    DBG_STATE_EXITED,
} DbgState;

/* ── Sesión principal del debugger ── */
typedef struct {
    pid_t              pid;
    DbgState           state;
    Arm64Regs          regs;         /* snapshot actual de registros */
    PtraceBreakpoint  *breakpoints;
    PtraceWatchpoint  *watchpoints;
    StackFrame        *call_stack;   /* unwound al detenerse         */
    DebugVar          *locals;       /* variables locales del frame  */
    uint32_t           n_bps;
    uint32_t           n_wps;
    int                exit_code;
    WsServer          *ws;
    RigCtx            *ctx;
} PtraceSession;

/* ── API pública ── */

/* Crear/destruir sesión */
PtraceSession* ptrace_session_new  (RigCtx *ctx, WsServer *ws);
void           ptrace_session_free (PtraceSession *s);

/* Adjuntar a proceso ya corriendo */
bool ptrace_attach   (PtraceSession *s, pid_t pid);

/* Lanzar ejecutable bajo depuración */
bool ptrace_launch   (PtraceSession *s, const char *exe,
                      char *const argv[]);

/* Desadjuntar (proceso continúa sin debugger) */
void ptrace_detach   (PtraceSession *s);

/* ── Control de ejecución ── */
bool ptrace_continue   (PtraceSession *s);
bool ptrace_step       (PtraceSession *s);  /* single step (1 instrucción) */
bool ptrace_step_over  (PtraceSession *s);  /* step over call              */
bool ptrace_step_out   (PtraceSession *s);  /* step out de función actual  */

/* ── Breakpoints ── */
bool ptrace_bp_set    (PtraceSession *s, uint64_t addr,
                       const char *sym, const char *file, uint32_t line);
bool ptrace_bp_del    (PtraceSession *s, uint64_t addr);
bool ptrace_bp_enable (PtraceSession *s, uint64_t addr);
bool ptrace_bp_disable(PtraceSession *s, uint64_t addr);

/* Breakpoint por nombre de símbolo (resuelve addr vía /proc/PID/maps + nm) */
bool ptrace_bp_sym    (PtraceSession *s, const char *sym_name);

/* ── Watchpoints ── */
bool ptrace_wp_set (PtraceSession *s, uint64_t addr,
                    size_t size, bool on_r, bool on_w);
bool ptrace_wp_del (PtraceSession *s, uint64_t addr);

/* ── Lectura/escritura de memoria ── */
bool    ptrace_mem_read  (PtraceSession *s, uint64_t addr,
                          void *buf, size_t len);
bool    ptrace_mem_write (PtraceSession *s, uint64_t addr,
                          const void *buf, size_t len);

/* Leer string nulo terminado desde memoria del proceso */
bool    ptrace_mem_read_str(PtraceSession *s, uint64_t addr,
                             char *out, size_t max);

/* ── Registros ARM64 ── */
bool ptrace_regs_get   (PtraceSession *s, Arm64Regs *out);
bool ptrace_regs_set   (PtraceSession *s, const Arm64Regs *regs);

/* Leer/escribir registro específico por nombre ("x0", "sp", "v3", "pc") */
bool ptrace_reg_get_by_name(PtraceSession *s, const char *reg,
                             uint64_t *out_lo, uint64_t *out_hi);
bool ptrace_reg_set_by_name(PtraceSession *s, const char *reg,
                             uint64_t val_lo, uint64_t val_hi);

/* ── Stack unwinding ── */
StackFrame* ptrace_unwind_stack(PtraceSession *s, uint32_t max_depth);

/* ── Resolución de símbolos y variables ── */
DebugVar*   ptrace_read_locals (PtraceSession *s);

/* Resolver símbolo → dirección usando /proc/PID/maps + readelf */
uint64_t    ptrace_sym_to_addr (PtraceSession *s, const char *sym);

/* ── Emisión de eventos WebSocket ── */
void ptrace_emit_stopped  (PtraceSession *s, const char *reason,
                            uint64_t addr);
void ptrace_emit_regs     (PtraceSession *s);
void ptrace_emit_stack    (PtraceSession *s);
void ptrace_emit_locals   (PtraceSession *s);
void ptrace_emit_memory   (PtraceSession *s, uint64_t addr,
                            const uint8_t *data, size_t len);

/* ── Loop principal del debugger (corre en thread) ── */
typedef struct {
    PtraceSession *session;
    char           exe[512];
    pid_t          attach_pid;  /* 0 = launch mode */
} PtraceThreadArg;
void* ptrace_dbg_thread(void *arg);

/* ── Comandos de alto nivel (parsing de JSON WS) ── */
/* Procesa un JSON de comando del dashboard y ejecuta la acción */
void ptrace_handle_cmd(PtraceSession *s, const char *json_cmd);

#endif /* PTRACE_DBG_H */
