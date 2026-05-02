/* ============================================================
   RigCom v8.0 — include/holo_trace.h
   Holographic Execution Trace: DAP + Portal 3D
   Mientras el programa corre en PTY, emite eventos WS por
   cada función ejecutada → los nodos 3D del Portal brillan.
   Compatible con el engine Portal WebGL del ui/index.html.
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef HOLO_TRACE_H
#define HOLO_TRACE_H
#include <stdbool.h>
#include <stdint.h>
#include "rigctx.h"
#include "wsserver.h"

/* Un nodo en el grafo holográfico */
typedef struct HoloNode {
    char     name[128];       /* nombre de función */
    char     file[256];
    uint32_t line;
    uint64_t call_count;      /* veces ejecutada */
    uint64_t last_ts_ns;      /* timestamp última ejecución */
    double   energy;          /* 0.0-1.0: intensidad del glow */
    uint32_t id;
    struct HoloNode *next;
} HoloNode;

/* Arista en el grafo de llamadas */
typedef struct HoloEdge {
    uint32_t from_id;
    uint32_t to_id;
    uint64_t weight;          /* número de llamadas */
    struct HoloEdge *next;
} HoloEdge;

/* Sesión de traza holográfica */
typedef struct {
    WsServer  *ws;
    RigCtx    *ctx;
    HoloNode  *nodes;
    HoloEdge  *edges;
    uint32_t   n_nodes;
    uint32_t   n_edges;
    bool       active;
    int        ptrace_pid;    /* proceso siendo trazado */
    /* Decay de energía: cada tick sin llamada reduce energy */
    double     decay_rate;    /* por defecto 0.05/tick */
} HoloSession;

/* ── API pública ── */
HoloSession* holo_session_new  (RigCtx *ctx, WsServer *ws);
void         holo_session_free (HoloSession *s);

/* Adjunta al proceso PID y comienza a trazar */
bool         holo_attach       (HoloSession *s, int pid);

/* Registra que fn_name fue llamada (desde PTY/DAP stdout parser) */
void         holo_on_call      (HoloSession *s,
                                 const char *fn_name,
                                 const char *from_fn,
                                 const char *file, uint32_t line);

/* Emite el grafo completo (snapshot) como evento WS JSON */
void         holo_emit_graph   (HoloSession *s);

/* Emite un evento de "glow" para una función específica */
void         holo_emit_glow    (HoloSession *s, const char *fn_name);

/* Tick de decay: reduce energía de nodos inactivos */
void         holo_tick         (HoloSession *s);

/* Analiza línea de stdout del proceso PTY para detectar llamadas */
void         holo_parse_line   (HoloSession *s, const char *line);

/* Thread que corre el loop de traza */
typedef struct {
    HoloSession *session;
    int          pid;
} HoloThreadArg;
void* holo_trace_thread(void *arg);

#endif /* HOLO_TRACE_H */
