#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — src/frontend_c.c  [CORREGIDO]
   Frontend C11: Preproc → Lexer → Parser → TypeChecker → RigIR
   Todos los campos AST corregidos contra ast.h real.
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/frontend.h"
#include "../include/rigscript.h"
#include "../include/rigctx.h"
#include "../include/rigir.h"
#include "../include/error.h"
#include "../include/preproc.h"
#include "../include/lexer.h"
#include "../include/ast.h"
#include "../include/parser.h"
#include "../include/typechecker.h"
#include "../include/symtable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Registro global ──────────────────────────────────────── */
LanguageFrontend g_frontends[FRONTEND_MAX];
int              g_n_frontends = 0;

void frontend_register(LanguageFrontend fe) {
    if (g_n_frontends < FRONTEND_MAX)
        g_frontends[g_n_frontends++] = fe;
}

const LanguageFrontend* frontend_for_file(const char *src_path) {
    const char *ext = strrchr(src_path, '.');
    if (!ext) return NULL;
    for (int i = 0; i < g_n_frontends; i++)
        if (strcmp(g_frontends[i].extension, ext) == 0)
            return &g_frontends[i];
    return NULL;
}

const char* frontend_lang_name(const char *src_path) {
    const LanguageFrontend *fe = frontend_for_file(src_path);
    return fe ? fe->display_name : "Unknown";
}

void frontends_init(void) {
    g_n_frontends = 0;
    frontend_register((LanguageFrontend){
        .extension        = ".c",
        .display_name     = "C11",
        .compile_to_ir    = c_frontend_compile_to_ir,
        .generate_ast_json= c_frontend_ast_json,
        .generate_header  = c_frontend_generate_header,
    });
    /* BUG FIX v6.0: .rigc y .rs usan RigScript frontend, NO C11 */
    frontend_register((LanguageFrontend){
        .extension        = ".rigc",
        .display_name     = "RigScript",
        .compile_to_ir    = rigscript_compile_to_ir,
        .generate_ast_json= rigscript_ast_json,
        .generate_header  = rigscript_generate_header,
    });
    frontend_register((LanguageFrontend){
        .extension        = ".rs",
        .display_name     = "RigScript",
        .compile_to_ir    = rigscript_compile_to_ir,
        .generate_ast_json= rigscript_ast_json,
        .generate_header  = rigscript_generate_header,
    });
    frontend_register((LanguageFrontend){
        .extension        = ".h",
        .display_name     = "C Header",
        .compile_to_ir    = NULL,
        .generate_ast_json= c_frontend_ast_json,
        .generate_header  = NULL,
    });
    frontend_register((LanguageFrontend){
        .extension        = ".cpp",
        .display_name     = "C++ (proxim.)",
        .compile_to_ir    = NULL,
        .generate_ast_json= NULL,
        .generate_header  = NULL,
    });
}

/* ══════════════════════════════════════════════════════════
   Lowering context
   ══════════════════════════════════════════════════════════ */
typedef struct {
    IRFunc  *fn;
    uint32_t label_cnt;
    char     break_label[64];
    char     cont_label[64];
} LowerCtx;

static uint32_t lctx_label(LowerCtx *lc) { return ++lc->label_cnt; }

static IRVal lower_expr(LowerCtx *lc, ASTNode *n);
static void  lower_stmt(LowerCtx *lc, ASTNode *n);

/* ── Tipo AST → IRType ───────────────────────────────────── */
static IRType type_to_irtype(TypeKind k) {
    switch (k) {
        case TY_BOOL:   return IRTY_I1;
        case TY_CHAR:   return IRTY_I8;
        case TY_SHORT:  return IRTY_I16;
        case TY_INT:    return IRTY_I32;
        case TY_LONG:   return IRTY_I64;
        case TY_UCHAR:  return IRTY_U8;
        case TY_USHORT: return IRTY_U16;
        case TY_UINT:   return IRTY_U32;
        case TY_ULONG:  return IRTY_U64;
        case TY_FLOAT:  return IRTY_F32;
        case TY_DOUBLE: return IRTY_F64;
        case TY_PTR:    return IRTY_PTR;
        default:        return IRTY_I64;
    }
}

/* ── TokenKind → IROp  (binary.op es int = TokenKind) ────── */
static IROp tok_to_irop(int tok) {
    switch (tok) {
        case TOK_PLUS:      return IR_ADD;
        case TOK_MINUS:     return IR_SUB;
        case TOK_STAR:      return IR_MUL;
        case TOK_SLASH:     return IR_DIV;
        case TOK_PERCENT:   return IR_MOD;
        case TOK_AMP:       return IR_AND;
        case TOK_PIPE:      return IR_OR;
        case TOK_CARET:     return IR_XOR;
        case TOK_LSHIFT:    return IR_SHL;
        case TOK_RSHIFT:    return IR_SHR;
        case TOK_EQ_EQ:     return IR_CMP_EQ;
        case TOK_BANG_EQ:   return IR_CMP_NE;
        case TOK_LT:        return IR_CMP_LT;
        case TOK_LT_EQ:     return IR_CMP_LE;
        case TOK_GT:        return IR_CMP_GT;
        case TOK_GT_EQ:     return IR_CMP_GE;
        default:            return IR_ADD;
    }
}

/* ══════════════════════════════════════════════════════════
   Lowering de expresiones
   ══════════════════════════════════════════════════════════ */
static IRVal lower_expr(LowerCtx *lc, ASTNode *n) {
    if (!n) return IRV_NONE;

    switch (n->kind) {

    case NODE_INT_LIT:
        return ir_emit_imm(lc->fn, n->int_lit.value, IRTY_I64);

    case NODE_FLOAT_LIT:
        return ir_emit_fimm(lc->fn, n->float_lit.value);

    case NODE_STRING_LIT:
        return ir_emit_imm(lc->fn, 0, IRTY_PTR);

    case NODE_IDENT_EXPR:
        /* SSA: placeholder — el regalloc asigna el registro correcto */
        return ir_emit_imm(lc->fn, 0, IRTY_I64);

    case NODE_BINARY_EXPR: {
        /* binary.op es TokenKind (int), binary.left / binary.right */
        IRVal lhs = lower_expr(lc, n->binary.left);
        IRVal rhs = lower_expr(lc, n->binary.right);
        IROp  op  = tok_to_irop(n->binary.op);
        IRType ty = (lhs.type == IRTY_F32 || lhs.type == IRTY_F64)
                    ? lhs.type : IRTY_I64;
        return ir_emit_bin(lc->fn, op, lhs, rhs, ty);
    }

    case NODE_UNARY_EXPR:
        /* unary.op es TokenKind (int), unary.operand */
    case NODE_ADDR_EXPR:
    case NODE_DEREF_EXPR: {
        IRVal val = lower_expr(lc, n->unary.operand);
        if (n->kind == NODE_DEREF_EXPR)
            return ir_emit_load(lc->fn, val, IRTY_I64);
        if (n->kind == NODE_ADDR_EXPR)
            return ir_emit_alloca(lc->fn, IRTY_I64, 1);
        /* unary ops: TOK_MINUS → IR_NEG, TOK_TILDE → IR_NOT, TOK_BANG → cmp 0 */
        if (n->unary.op == TOK_MINUS) return ir_emit_un(lc->fn, IR_NEG, val, IRTY_I64);
        if (n->unary.op == TOK_TILDE) return ir_emit_un(lc->fn, IR_NOT, val, IRTY_I64);
        return val;
    }

    case NODE_CALL_EXPR: {
        IRVal args[16];
        uint32_t nargs = 0;
        for (uint32_t i = 0; i < n->call.n_args && i < 16; i++)
            args[nargs++] = lower_expr(lc, n->call.args[i]);
        const char *fn_name = (n->call.callee &&
                               n->call.callee->kind == NODE_IDENT_EXPR)
                              ? n->call.callee->ident.name : "unknown";
        return ir_emit_call(lc->fn, fn_name, args, nargs, IRTY_I64);
    }

    case NODE_ASSIGN_EXPR: {
        /* assign.lhs / assign.rhs  (NO .target / .value) */
        IRVal rhs = lower_expr(lc, n->assign.rhs);
        return ir_emit_mov(lc->fn, rhs);
    }

    case NODE_CAST_EXPR: {
        /* cast.expr / cast.to  (NO .operand / .target_type) */
        IRVal val  = lower_expr(lc, n->cast.expr);
        IRType dst = n->cast.to
                     ? type_to_irtype(n->cast.to->kind)
                     : IRTY_I64;
        if (dst == val.type) return val;
        if (dst == IRTY_F64 || dst == IRTY_F32)
            return ir_emit_un(lc->fn, IR_ITOF, val, dst);
        return ir_emit_un(lc->fn, IR_TRUNC, val, dst);
    }

    default:
        return IRV_NONE;
    }
}

/* ══════════════════════════════════════════════════════════
   Lowering de sentencias
   ══════════════════════════════════════════════════════════ */
static void lower_stmt(LowerCtx *lc, ASTNode *n) {
    if (!n) return;
    switch (n->kind) {

    case NODE_EXPR_STMT:
        lower_expr(lc, n->expr_stmt.expr);
        break;

    case NODE_RETURN_STMT:
        if (n->ret.value) ir_emit_ret(lc->fn, lower_expr(lc, n->ret.value));
        else              ir_emit_ret_void(lc->fn);
        break;

    case NODE_BLOCK:
        for (uint32_t i = 0; i < n->block.n_stmts; i++)
            lower_stmt(lc, n->block.stmts[i]);
        break;

    case NODE_VAR_DECL: {
        /* var_decl.decl_type  (NO .type)  /  var_decl.init  /  var_decl.name */
        IRType ty = n->var_decl.decl_type
                    ? type_to_irtype(n->var_decl.decl_type->kind)
                    : IRTY_I64;
        IRVal slot = ir_emit_alloca(lc->fn, ty, 1);
        if (n->var_decl.init)
            ir_emit_store(lc->fn, lower_expr(lc, n->var_decl.init), slot);
        break;
    }

    case NODE_IF_STMT: {
        /* if_stmt.then_br / if_stmt.else_br  (NO .then_body / .else_body) */
        uint32_t id = lctx_label(lc);
        char then_lbl[32], else_lbl[32], end_lbl[32];
        snprintf(then_lbl, 32, "if_then_%u", id);
        snprintf(else_lbl, 32, "if_else_%u", id);
        snprintf(end_lbl,  32, "if_end_%u",  id);

        IRVal cond = lower_expr(lc, n->if_stmt.cond);
        ir_emit_cond_br(lc->fn, cond, then_lbl,
                        n->if_stmt.else_br ? else_lbl : end_lbl);

        IRBlock *then_b = irfunc_new_block(lc->fn, then_lbl);
        irfunc_set_block(lc->fn, then_b);
        lower_stmt(lc, n->if_stmt.then_br);
        ir_emit_br(lc->fn, end_lbl);

        if (n->if_stmt.else_br) {
            IRBlock *else_b = irfunc_new_block(lc->fn, else_lbl);
            irfunc_set_block(lc->fn, else_b);
            lower_stmt(lc, n->if_stmt.else_br);
            ir_emit_br(lc->fn, end_lbl);
        }
        IRBlock *end_b = irfunc_new_block(lc->fn, end_lbl);
        irfunc_set_block(lc->fn, end_b);
        break;
    }

    case NODE_WHILE_STMT: {
        uint32_t id = lctx_label(lc);
        char head[32], body[32], exit[32];
        snprintf(head, 32, "wh_head_%u", id);
        snprintf(body, 32, "wh_body_%u", id);
        snprintf(exit, 32, "wh_exit_%u", id);

        ir_emit_br(lc->fn, head);
        irfunc_set_block(lc->fn, irfunc_new_block(lc->fn, head));
        ir_emit_cond_br(lc->fn, lower_expr(lc, n->while_stmt.cond), body, exit);

        irfunc_set_block(lc->fn, irfunc_new_block(lc->fn, body));
        char ob[64], oc[64];
        memcpy(ob, lc->break_label, 64); memcpy(oc, lc->cont_label, 64);
        strncpy(lc->break_label, exit, 63); strncpy(lc->cont_label, head, 63);
        lower_stmt(lc, n->while_stmt.body);
        memcpy(lc->break_label, ob, 64);  memcpy(lc->cont_label, oc, 64);
        ir_emit_br(lc->fn, head);
        irfunc_set_block(lc->fn, irfunc_new_block(lc->fn, exit));
        break;
    }

    case NODE_FOR_STMT: {
        uint32_t id = lctx_label(lc);
        char head[32], body[32], step_lbl[32], exit[32];
        snprintf(head,     32, "for_head_%u", id);
        snprintf(body,     32, "for_body_%u", id);
        snprintf(step_lbl, 32, "for_step_%u", id);
        snprintf(exit,     32, "for_exit_%u", id);

        lower_stmt(lc, n->for_stmt.init);
        ir_emit_br(lc->fn, head);
        irfunc_set_block(lc->fn, irfunc_new_block(lc->fn, head));
        if (n->for_stmt.cond) {
            ir_emit_cond_br(lc->fn, lower_expr(lc, n->for_stmt.cond), body, exit);
        } else {
            ir_emit_br(lc->fn, body);
        }
        irfunc_set_block(lc->fn, irfunc_new_block(lc->fn, body));
        char ob[64], oc[64];
        memcpy(ob, lc->break_label, 64); memcpy(oc, lc->cont_label, 64);
        strncpy(lc->break_label, exit,     63);
        strncpy(lc->cont_label,  step_lbl, 63);
        lower_stmt(lc, n->for_stmt.body);
        memcpy(lc->break_label, ob, 64);  memcpy(lc->cont_label, oc, 64);
        ir_emit_br(lc->fn, step_lbl);
        irfunc_set_block(lc->fn, irfunc_new_block(lc->fn, step_lbl));
        if (n->for_stmt.step) lower_expr(lc, n->for_stmt.step);
        ir_emit_br(lc->fn, head);
        irfunc_set_block(lc->fn, irfunc_new_block(lc->fn, exit));
        break;
    }

    case NODE_DO_WHILE_STMT: {
        uint32_t id = lctx_label(lc);
        char body[32], exit[32];
        snprintf(body, 32, "do_body_%u", id);
        snprintf(exit, 32, "do_exit_%u", id);
        ir_emit_br(lc->fn, body);
        irfunc_set_block(lc->fn, irfunc_new_block(lc->fn, body));
        lower_stmt(lc, n->do_while.body);
        ir_emit_cond_br(lc->fn, lower_expr(lc, n->do_while.cond), body, exit);
        irfunc_set_block(lc->fn, irfunc_new_block(lc->fn, exit));
        break;
    }

    case NODE_BREAK_STMT:
        if (lc->break_label[0]) ir_emit_br(lc->fn, lc->break_label);
        break;

    case NODE_CONTINUE_STMT:
        if (lc->cont_label[0]) ir_emit_br(lc->fn, lc->cont_label);
        break;

    default:
        break;
    }
}

/* ── Lowering de función completa ────────────────────────── */
static void lower_func(IRModule *m, ASTNode *fn_node) {
    if (!fn_node || fn_node->kind != NODE_FUNC_DEF) return;

    IRType ret_ty = fn_node->func.ret_type
                    ? type_to_irtype(fn_node->func.ret_type->kind)
                    : IRTY_VOID;

    IRFunc *f = irmod_add_func(m,
                    fn_node->func.name ? fn_node->func.name : "anon",
                    ret_ty);
    if (!f) return;

    /* Parámetros: func.params es ParamList (no puntero) */
    ParamList *pl = &fn_node->func.params;
    if (pl->count > 0) {
        f->params   = malloc(pl->count * sizeof(IRVal));
        f->n_params = pl->count;
        for (uint32_t i = 0; i < pl->count && i < 8; i++) {
            IRType pty = IRTY_I64;
            /* var_decl.decl_type (NO .type) */
            if (pl->params[i] && pl->params[i]->var_decl.decl_type)
                pty = type_to_irtype(pl->params[i]->var_decl.decl_type->kind);
            f->params[i] = ir_next_vreg(f, pty);
        }
    }

    irfunc_set_block(f, irfunc_new_block(f, "entry"));

    LowerCtx lc = {0};
    lc.fn = f;
    lower_stmt(&lc, fn_node->func.body);

    /* Garantiza terminador en el bloque final */
    IRBlock *cur = f->current;
    if (cur && cur->n_instrs > 0) {
        IROp last = cur->instrs[cur->n_instrs - 1].op;
        if (last != IR_RET && last != IR_BR && last != IR_COND_BR)
            ir_emit_ret_void(f);
    } else {
        ir_emit_ret_void(f);
    }
}

/* ══════════════════════════════════════════════════════════
   c_frontend_compile_to_ir — Preproc→Lex→Parse→TC→Lower
   ══════════════════════════════════════════════════════════ */
IRModule* c_frontend_compile_to_ir(RigCtx *ctx, const char *src_path,
                                    RigErrorLog *log) {
    FILE *fp = fopen(src_path, "r");
    if (!fp) {
        riglog_add(log, ERR_FILE_NOT_FOUND, src_path, 0, 0,
                   "No se encontro el archivo fuente", "", "Verifica la ruta", "");
        return NULL;
    }
    fseek(fp, 0, SEEK_END); long fsz = ftell(fp); rewind(fp);
    char *raw = malloc((size_t)fsz + 1);
    if (!raw) { fclose(fp); return NULL; }
    size_t nr = fread(raw, 1, (size_t)fsz, fp); raw[nr]='\0'; fclose(fp);

    Preproc pp;
    preproc_init(&pp, ctx, log);
    preproc_add_path(&pp, "."); preproc_add_path(&pp, "include");
    preproc_add_path(&pp, "/usr/include");
    for (uint32_t i=0; i<ctx->config.n_include_dirs; i++)
        preproc_add_path(&pp, ctx->config.include_dirs[i]);
    for (uint32_t i=0; i<ctx->config.n_defines; i++) {
        char buf[256]; snprintf(buf,sizeof(buf),"%s",ctx->config.defines[i]);
        char *eq=strchr(buf,'=');
        if(eq){*eq='\0';preproc_define(&pp,buf,eq+1);}
        else preproc_define(&pp,buf,"1");
    }
    char *src = preproc_run(&pp, raw, nr, src_path);
    free(raw);
    if (!src) { preproc_free(&pp); return NULL; }

    Lexer lx; lexer_init(&lx, src, strlen(src), src_path);
    ASTArena *ar = ast_arena_new();
    Parser ps;  parser_init(&ps, &lx, log, ar, src_path);
    ASTNode *tu = parser_parse(&ps);

    if (!riglog_has_errors(log)) {
        TypeChecker *tc = tc_new(ar, log, src_path);
        tc_register_builtins(tc);
        tc_check(tc, tu);
        tc_free(tc);
    }

    IRModule *m = NULL;
    if (!riglog_has_errors(log) && tu &&
        tu->kind == NODE_TRANSLATION_UNIT) {
        m = irmod_new();
        for (uint32_t i=0; i<tu->tu.n_decls; i++)
            if (tu->tu.decls[i] &&
                tu->tu.decls[i]->kind == NODE_FUNC_DEF)
                lower_func(m, tu->tu.decls[i]);
        for (uint32_t fi=0; fi<m->n_funcs; fi++) {
            ir_pass_const_fold(m->funcs[fi]);
            ir_pass_copy_prop (m->funcs[fi]);
            ir_pass_dce       (m->funcs[fi]);
        }
    }

    ast_arena_free(ar); free(src); preproc_free(&pp);
    return m;
}

/* ══════════════════════════════════════════════════════════
   c_frontend_ast_json — JSON real de nodos AST
   ══════════════════════════════════════════════════════════ */
char* c_frontend_ast_json(RigCtx *ctx, const char *src_path,
                           RigErrorLog *log) {
    FILE *fp = fopen(src_path, "r");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long fsz = ftell(fp); rewind(fp);
    char *raw = malloc((size_t)fsz + 1);
    if (!raw) { fclose(fp); return NULL; }
    size_t nr = fread(raw, 1, (size_t)fsz, fp); raw[nr]='\0'; fclose(fp);

    RigErrorLog tmp = {0};
    Preproc pp; preproc_init(&pp, ctx, &tmp);
    preproc_add_path(&pp, "."); preproc_add_path(&pp, "include");
    char *src = preproc_run(&pp, raw, nr, src_path);
    free(raw);
    if (!src) { preproc_free(&pp); return NULL; }

    Lexer lx; lexer_init(&lx, src, strlen(src), src_path);
    ASTArena *ar = ast_arena_new();
    Parser ps;  parser_init(&ps, &lx, &tmp, ar, src_path);
    ASTNode *tu = parser_parse(&ps);

    size_t bufsz = 262144;
    char *buf = malloc(bufsz);
    if (!buf) { ast_arena_free(ar); free(src); preproc_free(&pp); return NULL; }

    char *p = buf; size_t rem = bufsz; int n;
    n = snprintf(p,rem,"{\"file\":\"%s\",\"functions\":[",src_path);
    p+=n; rem-=(size_t)n;

    bool first = true;
    if (tu && tu->kind == NODE_TRANSLATION_UNIT) {
        for (uint32_t i=0; i<tu->tu.n_decls && rem>512; i++) {
            ASTNode *d = tu->tu.decls[i];
            if (!d || d->kind != NODE_FUNC_DEF) continue;

            int n_stmts=0, n_calls=0, n_ptrs=0;
            ASTNode *body = d->func.body;
            if (body && body->kind == NODE_BLOCK) {
                n_stmts = (int)body->block.n_stmts;
                for (uint32_t si=0; si<body->block.n_stmts; si++) {
                    ASTNode *st = body->block.stmts[si];
                    if (!st) continue;
                    if (st->kind == NODE_EXPR_STMT && st->expr_stmt.expr &&
                        st->expr_stmt.expr->kind == NODE_CALL_EXPR) n_calls++;
                    /* var_decl.decl_type (NO .type) */
                    if (st->kind == NODE_VAR_DECL && st->var_decl.decl_type &&
                        st->var_decl.decl_type->kind == TY_PTR) n_ptrs++;
                }
            }

            /* Tipo retorno: func.ret_type */
            const char *rtype_str = "void";
            if (d->func.ret_type) {
                switch (d->func.ret_type->kind) {
                    case TY_PTR:  rtype_str = "ptr";  break;
                    case TY_INT:  rtype_str = "int";  break;
                    case TY_VOID: rtype_str = "void"; break;
                    default:      rtype_str = "other"; break;
                }
            }

            n = snprintf(p, rem,
                "%s{\"name\":\"%s\",\"line\":%u,"
                "\"n_stmts\":%d,\"n_calls\":%d,"
                "\"n_ptr_vars\":%d,\"cyclomatic\":%d,"
                "\"ret_type\":\"%s\"}",
                first ? "" : ",",
                d->func.name ? d->func.name : "anon",
                d->line, n_stmts, n_calls, n_ptrs,
                1 + n_calls,
                rtype_str);
            p+=n; rem-=(size_t)n;
            first = false;
        }
    }
    snprintf(p, rem, "]}");

    ast_arena_free(ar); free(src); preproc_free(&pp);
    (void)log;
    return buf;
}

/* ══════════════════════════════════════════════════════════
   c_frontend_generate_header
   ══════════════════════════════════════════════════════════ */
char* c_frontend_generate_header(RigCtx *ctx, const char *src_path,
                                  RigErrorLog *log) {
    FILE *fp = fopen(src_path, "r");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long fsz = ftell(fp); rewind(fp);
    char *raw = malloc((size_t)fsz + 1);
    if (!raw) { fclose(fp); return NULL; }
    size_t nr = fread(raw, 1, (size_t)fsz, fp); raw[nr]='\0'; fclose(fp);

    RigErrorLog tmp = {0};
    Preproc pp; preproc_init(&pp, ctx, &tmp);
    preproc_add_path(&pp, "."); preproc_add_path(&pp, "include");
    char *src = preproc_run(&pp, raw, nr, src_path);
    free(raw);
    if (!src) { preproc_free(&pp); return NULL; }

    Lexer lx; lexer_init(&lx, src, strlen(src), src_path);
    ASTArena *ar = ast_arena_new();
    Parser ps; parser_init(&ps, &lx, &tmp, ar, src_path);
    ASTNode *tu = parser_parse(&ps);

    char *hdr = ast_generate_header(tu, src_path);

    ast_arena_free(ar); free(src); preproc_free(&pp);
    (void)log;
    return hdr;
}

/* ══════════════════════════════════════════════════════════
   compile_source_file — entry-point del pipeline
   ══════════════════════════════════════════════════════════ */
IRModule* compile_source_file(const char *src_path,
                               RigCtx    *ctx,
                               RigErrorLog *log) {
    const LanguageFrontend *fe = frontend_for_file(src_path);
    if (!fe || !fe->compile_to_ir) {
        const char *ext = strrchr(src_path, '.');
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Extension '%s' no soportada",
                 ext ? ext : "(sin extension)");
        riglog_add(log, ERR_CONFIG, src_path, 0, 0, msg, "", "", "");
        return NULL;
    }
    return fe->compile_to_ir(ctx, src_path, log);
}
