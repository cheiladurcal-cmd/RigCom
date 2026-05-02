#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — src/oracle_ip.c
   Oracle Inter-Procedural: análisis de flujo de punteros
   usando nodos REALES del AST — sin búsqueda de texto.
   Rastrea malloc() → free() entre funciones y archivos.
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/oracle_ip.h"
#include "../include/frontend.h"
#include "../include/rigctx.h"
#include "../include/preproc.h"
#include "../include/lexer.h"
#include "../include/ast.h"
#include "../include/parser.h"
#include "../include/typechecker.h"
#include "../include/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ── Helpers internos ──────────────────────────────────── */
static OraclePtr* ptr_new(const char *name, const char *file,
                            uint32_t line) {
    OraclePtr *p = calloc(1, sizeof(OraclePtr));
    if (!p) return NULL;
    strncpy(p->name,        name, sizeof(p->name)-1);
    strncpy(p->origin_file, file, sizeof(p->origin_file)-1);
    p->alloc_line = line;
    return p;
}

static OracleFnDiag* fn_diag_new(const char *name, const char *file,
                                   uint32_t line) {
    OracleFnDiag *d = calloc(1, sizeof(OracleFnDiag));
    if (!d) return NULL;
    strncpy(d->fn_name, name, sizeof(d->fn_name)-1);
    strncpy(d->file,    file, sizeof(d->file)-1);
    d->line = line;
    return d;
}

/* ══════════════════════════════════════════════════════════
   Análisis de un bloque de sentencias
   Busca NODE_CALL_EXPR cuyo callee es "malloc"/"calloc"
   usando el AST real, no texto.
   ══════════════════════════════════════════════════════════ */
static void analyze_block(ASTNode *block, OracleFnDiag *diag,
                            const char *file);

/* Comprueba si un nodo es una llamada a fn_name */
static bool is_call_to(ASTNode *n, const char *fn_name) {
    if (!n || n->kind != NODE_CALL_EXPR) return false;
    ASTNode *callee = n->call.callee;
    if (!callee || callee->kind != NODE_IDENT_EXPR) return false;
    return callee->ident.name &&
           strcmp(callee->ident.name, fn_name) == 0;
}

/* Extrae nombre de variable del lado izquierdo de una asignación */
static const char* lhs_name(ASTNode *assign) {
    if (!assign) return NULL;
    if (assign->kind == NODE_ASSIGN_EXPR) {
        ASTNode *lhs = assign->assign.lhs;
        if (lhs && lhs->kind == NODE_IDENT_EXPR)
            return lhs->ident.name;
    }
    if (assign->kind == NODE_VAR_DECL)
        return assign->var_decl.name;
    return NULL;
}

/* Analiza un nodo expresión buscando free(), null-check */
static void analyze_expr_for_frees(ASTNode *expr,
                                    OracleFnDiag *diag) {
    if (!expr) return;

    /* free(var) — usa nodo real de call */
    if (is_call_to(expr, "free")) {
        if (expr->call.n_args > 0 && expr->call.args[0]) {
            ASTNode *arg = expr->call.args[0];
            if (arg->kind == NODE_IDENT_EXPR && arg->ident.name) {
                /* Marcar el puntero como liberado */
                for (OraclePtr *p = diag->ptrs; p; p = p->next)
                    if (strcmp(p->name, arg->ident.name) == 0)
                        p->freed = true;
            }
        }
    }

    /* Llamada pasando puntero a otra función → interprocedural */
    if (expr->kind == NODE_CALL_EXPR) {
        const char *called = (expr->call.callee &&
                              expr->call.callee->kind == NODE_IDENT_EXPR)
                             ? expr->call.callee->ident.name : NULL;
        for (uint32_t i = 0; i < expr->call.n_args; i++) {
            ASTNode *arg = expr->call.args[i];
            if (!arg) continue;
            /* Si el argumento es un ident que conocemos como ptr */
            if (arg->kind == NODE_IDENT_EXPR && arg->ident.name) {
                for (OraclePtr *p = diag->ptrs; p; p = p->next) {
                    if (strcmp(p->name, arg->ident.name) == 0) {
                        p->passed_to_fn = true;
                        if (called)
                            strncpy(p->passed_fn, called,
                                    sizeof(p->passed_fn)-1);
                    }
                }
            }
        }
        diag->n_calls++;
    }
}

static void analyze_stmt(ASTNode *st, OracleFnDiag *diag,
                           const char *file) {
    if (!st) return;

    switch (st->kind) {
    case NODE_VAR_DECL: {
        /* int *p = malloc(n) — captura en el AST real */
        ASTNode *init = st->var_decl.init;
        if (!init) break;

        /* La inicialización puede ser un CALL a malloc/calloc/realloc */
        ASTNode *call_node = init;
        /* O puede ser un cast: (int*)malloc(...) */
        if (init->kind == NODE_CAST_EXPR)
            call_node = init->cast.expr;

        bool is_alloc = is_call_to(call_node, "malloc") ||
                        is_call_to(call_node, "calloc") ||
                        is_call_to(call_node, "realloc") ||
                        is_call_to(call_node, "strdup");

        if (is_alloc && st->var_decl.name) {
            OraclePtr *ptr = ptr_new(st->var_decl.name, file, st->line);
            if (ptr) {
                ptr->next   = diag->ptrs;
                diag->ptrs  = ptr;
                diag->n_ptrs++;
            }
        }
        break;
    }

    case NODE_ASSIGN_EXPR: {
        /* p = malloc(...) — asignación a variable existente */
        ASTNode *rhs = st->assign.rhs;
        if (!rhs) break;
        ASTNode *call_node = rhs;
        if (rhs->kind == NODE_CAST_EXPR) call_node = rhs->cast.expr;

        bool is_alloc = is_call_to(call_node, "malloc") ||
                        is_call_to(call_node, "calloc") ||
                        is_call_to(call_node, "realloc");
        if (is_alloc) {
            const char *vname = lhs_name(st);
            if (vname) {
                OraclePtr *ptr = ptr_new(vname, file, st->line);
                if (ptr) {
                    ptr->next = diag->ptrs;
                    diag->ptrs= ptr;
                    diag->n_ptrs++;
                }
            }
        }
        analyze_expr_for_frees(rhs, diag);
        break;
    }

    case NODE_EXPR_STMT:
        analyze_expr_for_frees(st->expr_stmt.expr, diag);
        break;

    case NODE_IF_STMT: {
        /* if (!p) — detecta null-check */
        ASTNode *cond = st->if_stmt.cond;
        if (cond && cond->kind == NODE_UNARY_EXPR &&
            cond->unary.op == TOK_BANG) {
            ASTNode *operand = cond->unary.operand;
            if (operand && operand->kind == NODE_IDENT_EXPR) {
                for (OraclePtr *p = diag->ptrs; p; p = p->next)
                    if (strcmp(p->name, operand->ident.name) == 0)
                        p->null_checked = true;
            }
        }
        /* Analizar cuerpos */
        analyze_block(st->if_stmt.then_br, diag, file);
        analyze_block(st->if_stmt.else_br, diag, file);
        diag->cyclomatic++;
        break;
    }

    case NODE_WHILE_STMT:
    case NODE_DO_WHILE_STMT:
        analyze_block(st->while_stmt.body, diag, file);
        diag->cyclomatic++;
        break;

    case NODE_FOR_STMT:
        analyze_stmt(st->for_stmt.init, diag, file);
        analyze_block(st->for_stmt.body, diag, file);
        diag->cyclomatic++;
        break;

    case NODE_BLOCK:
        analyze_block(st, diag, file);
        break;

    case NODE_RETURN_STMT:
        analyze_expr_for_frees(st->ret.value, diag);
        break;

    default:
        break;
    }
}

static void analyze_block(ASTNode *block, OracleFnDiag *diag,
                            const char *file) {
    if (!block || block->kind != NODE_BLOCK) {
        /* También acepta statement suelto */
        if (block) analyze_stmt(block, diag, file);
        return;
    }
    for (uint32_t i = 0; i < block->block.n_stmts; i++)
        analyze_stmt(block->block.stmts[i], diag, file);
}

/* ══════════════════════════════════════════════════════════
   oracle_ip_analyze_ast
   Punto de entrada: recibe la translation unit del parser
   ══════════════════════════════════════════════════════════ */
OracleIPSession* oracle_ip_analyze_ast(ASTNode *tu,
                                        const char *file) {
    OracleIPSession *sess = calloc(1, sizeof(OracleIPSession));
    if (!sess) return NULL;

    if (!tu || tu->kind != NODE_TRANSLATION_UNIT)
        return sess;

    for (uint32_t i = 0; i < tu->tu.n_decls; i++) {
        ASTNode *d = tu->tu.decls[i];
        if (!d || d->kind != NODE_FUNC_DEF) continue;

        OracleFnDiag *diag = fn_diag_new(
            d->func.name ? d->func.name : "anon",
            file, d->line);
        if (!diag) continue;

        diag->cyclomatic = 1; /* base */

        /* Analizar el cuerpo de la función */
        analyze_block(d->func.body, diag, file);

        /* Detectar recursión: busca llamada con mismo nombre */
        diag->has_recursion = false;
        if (d->func.body && d->func.name) {
            ASTNode *body = d->func.body;
            if (body->kind == NODE_BLOCK) {
                for (uint32_t si = 0; si < body->block.n_stmts; si++) {
                    ASTNode *st = body->block.stmts[si];
                    if (st && st->kind == NODE_EXPR_STMT &&
                        is_call_to(st->expr_stmt.expr, d->func.name))
                        diag->has_recursion = true;
                }
            }
        }

        /* Contar leaks: punteros no liberados y no pasados */
        for (OraclePtr *p = diag->ptrs; p; p = p->next) {
            if (!p->freed && !p->passed_to_fn)
                sess->total_leaks++;
            if (!p->null_checked)
                sess->total_null_risks++;
        }

        /* Insertar diag en sesión */
        diag->next   = sess->fns;
        sess->fns    = diag;
        sess->n_fns++;
    }

    return sess;
}

/* ══════════════════════════════════════════════════════════
   oracle_ip_analyze_project
   Analiza múltiples archivos del proyecto (cross-file)
   ══════════════════════════════════════════════════════════ */
OracleIPSession* oracle_ip_analyze_project(const char **files,
                                            int n_files,
                                            RigCtx *ctx) {
    OracleIPSession *mega = calloc(1, sizeof(OracleIPSession));
    if (!mega) return NULL;

    for (int fi = 0; fi < n_files; fi++) {
        FILE *fp = fopen(files[fi], "r");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END); long fsz = ftell(fp); rewind(fp);
        char *raw = malloc((size_t)fsz + 1);
        if (!raw) { fclose(fp); continue; }
        size_t nr = fread(raw, 1, (size_t)fsz, fp);
        raw[nr] = '\0'; fclose(fp);

        RigErrorLog tmp = {0};
        Preproc pp; preproc_init(&pp, ctx, &tmp);
        preproc_add_path(&pp, "."); preproc_add_path(&pp, "include");
        char *src = preproc_run(&pp, raw, nr, files[fi]);
        free(raw);
        if (!src) { preproc_free(&pp); continue; }

        Lexer lx; lexer_init(&lx, src, strlen(src), files[fi]);
        ASTArena *ar = ast_arena_new();
        Parser ps;  parser_init(&ps, &lx, &tmp, ar, files[fi]);
        ASTNode *tu = parser_parse(&ps);

        OracleIPSession *sub = oracle_ip_analyze_ast(tu, files[fi]);
        if (sub) {
            /* Fusionar sub → mega */
            mega->total_leaks      += sub->total_leaks;
            mega->total_null_risks += sub->total_null_risks;
            mega->n_fns            += sub->n_fns;
            /* Concatenar lista de funciones */
            if (sub->fns) {
                OracleFnDiag *last = sub->fns;
                while (last->next) last = last->next;
                last->next = mega->fns;
                mega->fns  = sub->fns;
                sub->fns   = NULL;
            }
            oracle_ip_free(sub);
        }

        ast_arena_free(ar); free(src); preproc_free(&pp);
    }

    return mega;
}

/* ── Emit por WebSocket ─────────────────────────────────── */
void oracle_ip_emit_ws(OracleIPSession *s, WsServer *srv,
                        const char *trigger_file) {
    if (!s || !srv) return;

    /* Emitir resumen global */
    ws_broadcastf(srv,
        "{\"ev\":\"oracle_ip_summary\","
        "\"file\":\"%s\","
        "\"n_fns\":%d,"
        "\"total_leaks\":%d,"
        "\"total_null_risks\":%d}",
        trigger_file ? trigger_file : "project",
        s->n_fns, s->total_leaks, s->total_null_risks);

    /* Emitir diagnóstico por función */
    for (OracleFnDiag *d = s->fns; d; d = d->next) {
        /* Diagnóstico de función base */
        ws_broadcastf(srv,
            "{\"ev\":\"oracle_fn_diag\","
            "\"fn\":\"%s\","
            "\"file\":\"%s\","
            "\"line\":%u,"
            "\"cyclomatic\":%d,"
            "\"n_calls\":%d,"
            "\"n_ptrs\":%d,"
            "\"has_recursion\":%s}",
            d->fn_name, d->file, d->line,
            d->cyclomatic, d->n_calls, d->n_ptrs,
            d->has_recursion ? "true" : "false");

        /* Un hint por puntero problemático */
        for (OraclePtr *p = d->ptrs; p; p = p->next) {
            if (!p->freed && !p->passed_to_fn) {
                ws_broadcastf(srv,
                    "{\"ev\":\"oracle_hint\","
                    "\"file\":\"%s\","
                    "\"line\":%u,"
                    "\"kind\":\"orphan_ptr\","
                    "\"msg\":\"Memory leak: '%s' asignado en línea %u sin free()\","
                    "\"fix\":\"free(%s);\","
                    "\"action\":\"inject_free\","
                    "\"var\":\"%s\","
                    "\"fn\":\"%s\","
                    "\"alloc_line\":%u,"
                    "\"interprocedural\":%s}",
                    d->file, p->alloc_line,
                    p->name, p->alloc_line,
                    p->name, p->name, d->fn_name,
                    p->alloc_line,
                    p->passed_to_fn ? "true" : "false");
            }
            if (!p->null_checked) {
                ws_broadcastf(srv,
                    "{\"ev\":\"oracle_hint\","
                    "\"file\":\"%s\","
                    "\"line\":%u,"
                    "\"kind\":\"null_deref_risk\","
                    "\"msg\":\"Puntero '%s' sin null-check después de malloc()\","
                    "\"fix\":\"if (!%s) { free(%s); return NULL; }\","
                    "\"action\":\"add_null_check\","
                    "\"var\":\"%s\","
                    "\"fn\":\"%s\"}",
                    d->file, p->alloc_line,
                    p->name, p->name, p->name,
                    p->name, d->fn_name);
            }
            if (p->passed_to_fn) {
                ws_broadcastf(srv,
                    "{\"ev\":\"oracle_hint\","
                    "\"file\":\"%s\","
                    "\"line\":%u,"
                    "\"kind\":\"interprocedural\","
                    "\"msg\":\"Puntero '%s' pasado a '%s()' — verificar que esa función lo libera\","
                    "\"fix\":\"Añadir documentación o assert que %s() llama free()\","
                    "\"action\":\"review_callee\","
                    "\"var\":\"%s\","
                    "\"callee\":\"%s\"}",
                    d->file, p->alloc_line,
                    p->name, p->passed_fn,
                    p->passed_fn, p->name, p->passed_fn);
            }
        }

        /* Complejidad ciclomática alta */
        if (d->cyclomatic > 10) {
            ws_broadcastf(srv,
                "{\"ev\":\"oracle_hint\","
                "\"file\":\"%s\","
                "\"line\":%u,"
                "\"kind\":\"perf\","
                "\"msg\":\"Función '%s' tiene complejidad ciclomática %d — refactorizar\","
                "\"fix\":\"Dividir en funciones más pequeñas\","
                "\"action\":\"refactor\","
                "\"fn\":\"%s\","
                "\"cyclomatic\":%d}",
                d->file, d->line,
                d->fn_name, d->cyclomatic,
                d->fn_name, d->cyclomatic);
        }
    }

    ws_broadcastf(srv,
        "{\"ev\":\"oracle_ip_done\",\"file\":\"%s\","
        "\"hints\":%d}",
        trigger_file ? trigger_file : "project",
        s->total_leaks + s->total_null_risks);
}

/* ── Liberación ─────────────────────────────────────────── */
void oracle_ip_free(OracleIPSession *s) {
    if (!s) return;
    OracleFnDiag *d = s->fns;
    while (d) {
        OracleFnDiag *nd = d->next;
        OraclePtr *p = d->ptrs;
        while (p) { OraclePtr *np = p->next; free(p); p = np; }
        free(d);
        d = nd;
    }
    free(s);
}

/* ── Thread async ───────────────────────────────────────── */
void* oracle_ip_thread(void *arg) {
    OracleIPArg *a = (OracleIPArg *)arg;
    WsServer *srv  = a->srv;
    char file[512]; memcpy(file, a->file, sizeof(file));
    bool ip = a->interprocedural;
    free(a);

    ws_broadcastf(srv,
        "{\"ev\":\"oracle_scan_start\","
        "\"file\":\"%s\","
        "\"interprocedural\":%s}",
        file, ip ? "true" : "false");

    OracleIPSession *sess = NULL;

    if (ip) {
        /* Analizar todo el directorio src/ */
        FILE *find = popen("find src -name '*.c' 2>/dev/null", "r");
        char *files[128]; int nf = 0;
        if (find) {
            char ln[512];
            while (nf < 128 && fgets(ln, sizeof(ln), find)) {
                size_t l = strlen(ln);
                while (l > 0 && (ln[l-1]=='\n'||ln[l-1]=='\r'))
                    ln[--l]='\0';
                if (l > 0) files[nf++] = strdup(ln);
            }
            pclose(find);
        }
        RigCtx *ctx = rigctx_new(".");
        rigctx_load_config(ctx, "rigcom.toml");
        sess = oracle_ip_analyze_project(
                   (const char **)files, nf, ctx);
        for (int i = 0; i < nf; i++) free(files[i]);
        rigctx_free(ctx);
    } else {
        /* Analizar solo el archivo indicado */
        FILE *fp = fopen(file, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END); long fsz = ftell(fp); rewind(fp);
            char *raw = malloc((size_t)fsz + 1);
            size_t nr = fread(raw, 1, (size_t)fsz, fp); raw[nr]='\0';
            fclose(fp);
            RigCtx *ctx = rigctx_new(".");
            rigctx_load_config(ctx, "rigcom.toml");
            RigErrorLog tmp = {0};
            Preproc pp; preproc_init(&pp, ctx, &tmp);
            preproc_add_path(&pp, "."); preproc_add_path(&pp, "include");
            char *src = preproc_run(&pp, raw, nr, file);
            free(raw);
            if (src) {
                Lexer lx; lexer_init(&lx, src, strlen(src), file);
                ASTArena *ar = ast_arena_new();
                Parser ps; parser_init(&ps, &lx, &tmp, ar, file);
                ASTNode *tu = parser_parse(&ps);
                sess = oracle_ip_analyze_ast(tu, file);
                ast_arena_free(ar); free(src);
            }
            preproc_free(&pp); rigctx_free(ctx);
        }
    }

    if (sess) {
        oracle_ip_emit_ws(sess, srv, file);
        oracle_ip_free(sess);
    } else {
        ws_broadcastf(srv,
            "{\"ev\":\"oracle_ip_done\",\"file\":\"%s\","
            "\"hints\":0}", file);
    }

    return NULL;
}
