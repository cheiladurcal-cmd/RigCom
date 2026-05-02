/* ============================================================
   RigCom v8.0 — include/frontend.h
   LanguageFrontend vtable — pipeline multi-lenguaje
   Fase 1-6: C11 · C++ · RigScript → RigIR universal
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef FRONTEND_H
#define FRONTEND_H

#include "rigir.h"
#include "rigctx.h"
#include "error.h"
#include <stdbool.h>

typedef struct LanguageFrontend LanguageFrontend;
struct LanguageFrontend {
    const char *extension;      /* ".c" ".cpp" ".rigc"               */
    const char *display_name;   /* "C11"  "C++17"  "RigScript"       */
    IRModule* (*compile_to_ir)(RigCtx *ctx, const char *src_path,
                               RigErrorLog *log);
    char*     (*generate_ast_json)(RigCtx *ctx, const char *src_path,
                                   RigErrorLog *log);
    char*     (*generate_header)(RigCtx *ctx, const char *src_path,
                                 RigErrorLog *log);
};

#define FRONTEND_MAX 16
extern LanguageFrontend g_frontends[FRONTEND_MAX];
extern int              g_n_frontends;

void                    frontends_init(void);
void                    frontend_register(LanguageFrontend fe);
const LanguageFrontend* frontend_for_file(const char *src_path);
const char*             frontend_lang_name(const char *src_path);

/* C11 frontend */
IRModule* c_frontend_compile_to_ir  (RigCtx*, const char*, RigErrorLog*);
char*     c_frontend_ast_json       (RigCtx*, const char*, RigErrorLog*);
char*     c_frontend_generate_header(RigCtx*, const char*, RigErrorLog*);

/* Pipeline entry-point — reemplaza pipeline_check_file */
IRModule* compile_source_file(const char *src_path, RigCtx *ctx,
                               RigErrorLog *log);

#endif /* FRONTEND_H */
