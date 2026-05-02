/* ============================================================
   RigCom v8.0 — tests/test_basic.c
   Smoke tests for core modules
   ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../include/error.h"
#include "../include/lexer.h"
#include "../include/ast.h"
#include "../include/symtable.h"
#include "../include/preproc.h"
#include "../include/rigctx.h"

#define PASS(name) printf("  \033[32m✓\033[0m %s\n", name)
#define FAIL(name) printf("  \033[31m✗\033[0m %s\n", name); exit(1)

/* ── Test: error log ────────────────────────────────────────── */
static void test_error_log(void) {
    RigErrorLog *log = riglog_new();
    assert(log != NULL);
    assert(log->count == 0);

    riglog_add(log, ERR_SYNTAX, "test.c", 1, 5,
               "Error de prueba", "ctx", "sugerencia", "fix");
    assert(log->count == 1);
    assert(riglog_has_errors(log));

    char *json = riglog_to_json(log);
    assert(json != NULL);
    assert(strstr(json, "Error de prueba") != NULL);
    free(json);

    riglog_free(log);
    PASS("error_log: crear, agregar, serializar JSON");
}

/* ── Test: lexer ────────────────────────────────────────────── */
static void test_lexer(void) {
    const char *src = "int main(void) { return 0; }";
    Lexer lx;
    lexer_init(&lx, src, strlen(src), "test.c");

    Token t;
    int count = 0;
    do {
        t = lexer_next(&lx);
        count++;
    } while (t.kind != TOK_EOF && t.kind != TOK_ERROR && count < 100);

    assert(!lx.has_error);
    assert(count > 5);  /* At minimum: int, main, (, void, ), {, return, 0, ;, }, EOF */
    PASS("lexer: tokenizar 'int main(void) { return 0; }'");
}

/* ── Test: symbol table ─────────────────────────────────────── */
static void test_symtable(void) {
    SymTable *st = symtab_new();
    assert(st != NULL);

    bool ok = symtab_define(st, "x", SYM_VAR, NULL, NULL);
    assert(ok);

    Symbol *s = symtab_lookup(st, "x");
    assert(s != NULL);
    assert(strcmp(s->name, "x") == 0);

    /* Redefinition in same scope → false */
    ok = symtab_define(st, "x", SYM_VAR, NULL, NULL);
    assert(!ok);

    /* New scope: same name OK */
    symtab_push_scope(st);
    ok = symtab_define(st, "x", SYM_VAR, NULL, NULL);
    assert(ok);

    /* After pop: inner x gone */
    symtab_pop_scope(st);
    s = symtab_lookup_local(st, "x");
    assert(s != NULL); /* outer x still there */

    symtab_free(st);
    PASS("symtable: define, lookup, scopes anidados");
}

/* ── Test: arena allocator ──────────────────────────────────── */
static void test_arena(void) {
    ASTArena *arena = ast_arena_new();
    assert(arena != NULL);

    ASTNode *n = ast_node_new(arena, NODE_INT_LIT, 1, 1);
    assert(n != NULL);
    n->int_lit.value = 42;
    assert(n->int_lit.value == 42);

    char *s = ast_intern(arena, "hello", 5);
    assert(s != NULL);
    assert(strcmp(s, "hello") == 0);

    ast_arena_free(arena);
    PASS("arena: alloc nodo, intern string, free");
}

/* ── Test: preprocessor ─────────────────────────────────────── */
static void test_preproc(void) {
    RigCtx *ctx = rigctx_new(".");
    RigErrorLog *log = riglog_new();

    Preproc pp;
    preproc_init(&pp, ctx, log);
    preproc_define(&pp, "FOO", "42");
    preproc_define(&pp, "BAR", "FOO + 1");

    const char *src = "int x = FOO;\nint y = BAR;\n";
    char *out = preproc_run(&pp, src, strlen(src), "<test>");
    assert(out != NULL);
    assert(strstr(out, "42") != NULL);
    free(out);

    preproc_free(&pp);
    riglog_free(log);
    rigctx_free(ctx);
    PASS("preproc: define, expand macros");
}

/* ── Main ───────────────────────────────────────────────────── */
int main(void) {
    printf("\n  \033[1mRigCom v8.0 — Tests básicos\033[0m\n\n");

    test_error_log();
    test_lexer();
    test_symtable();
    test_arena();
    test_preproc();

    printf("\n  \033[32m✓ Todos los tests pasaron\033[0m\n\n");
    return 0;
}
