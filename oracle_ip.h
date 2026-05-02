/* ============================================================
   RigCom v8.0 — include/oracle_ip.h
   Oracle Inter-Procedural: análisis de flujo de punteros
   entre funciones y archivos usando el AST real
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef ORACLE_IP_H
#define ORACLE_IP_H

#include "ast.h"
#include "wsserver.h"
#include <stdbool.h>
#include <stdint.h>

/* Forward declaration — evita include circular con rigctx.h */
typedef struct RigCtx RigCtx;

/* ── Un puntero rastreado inter-proceduralmente ────────────── */
typedef struct OraclePtr {
    char     name[128];      /* nombre de variable              */
    char     origin_file[256];
    uint32_t alloc_line;     /* línea donde se llamó malloc()   */
    bool     freed;          /* ¿tiene free() confirmado?       */
    bool     null_checked;   /* ¿tiene null-check?              */
    bool     passed_to_fn;   /* ¿se pasó a otra función?        */
    char     passed_fn[128]; /* nombre de la función receptora  */
    struct OraclePtr *next;
} OraclePtr;

/* ── Diagnóstico de función ──────────────────────────────── */
typedef struct OracleFnDiag {
    char     fn_name[128];
    char     file[256];
    uint32_t line;
    /* Punteros en esta función */
    OraclePtr *ptrs;
    int        n_ptrs;
    /* Estadísticas */
    int        n_calls;        /* cuántas veces llama a otras fns */
    int        cyclomatic;     /* complejidad ciclomática         */
    bool       has_recursion;
    struct OracleFnDiag *next;
} OracleFnDiag;

/* ── Sesión de análisis inter-procedural ─────────────────── */
typedef struct {
    OracleFnDiag *fns;
    int            n_fns;
    /* Estadísticas globales */
    int            total_leaks;
    int            total_null_risks;
    int            total_type_errors;
} OracleIPSession;

/* ── API pública ─────────────────────────────────────────── */

/* Analiza un AST completo (translation unit) usando nodos reales */
OracleIPSession* oracle_ip_analyze_ast(ASTNode *tu,
                                        const char *file);

/* Analiza múltiples archivos (inter-procedural cross-file) */
OracleIPSession* oracle_ip_analyze_project(const char **files,
                                            int n_files,
                                            RigCtx *ctx);

/* Emite todos los diagnósticos por WebSocket */
void oracle_ip_emit_ws(OracleIPSession *s, WsServer *srv,
                        const char *trigger_file);

/* Libera sesión */
void oracle_ip_free(OracleIPSession *s);

/* ── Thread arg para análisis async ─────────────────────── */
typedef struct {
    WsServer *srv;
    char      file[512];
    bool      interprocedural; /* true = escanea todo el proyecto */
} OracleIPArg;

void* oracle_ip_thread(void *arg);

#endif /* ORACLE_IP_H */
