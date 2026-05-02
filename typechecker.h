/* ============================================================
   RigCom v8.0 — include/typechecker.h
   Semantic analysis + type checking
   ============================================================ */
#ifndef TYPECHECKER_H
#define TYPECHECKER_H

#include "ast.h"
#include "symtable.h"
#include "error.h"
#include <stdbool.h>

typedef struct {
    SymTable    *syms;
    ASTArena    *arena;
    RigErrorLog *log;
    Type        *current_func_ret;  /* return type of function being checked */
    uint32_t     error_count;
    uint32_t     warning_count;
    const char  *file;
    bool         in_loop;           /* for break/continue validation */
} TypeChecker;

/* ── Lifecycle ──────────────────────────────────────────────── */
TypeChecker* tc_new  (ASTArena *arena, RigErrorLog *log, const char *file);
void         tc_free (TypeChecker *tc);

/* ── Entry point ────────────────────────────────────────────── */
bool tc_check(TypeChecker *tc, ASTNode *translation_unit);

/* ── Builtins registration ──────────────────────────────────── */
void tc_register_builtins(TypeChecker *tc);

#endif /* TYPECHECKER_H */
