/* ============================================================
   RigCom v8.0 — src/typechecker.c
   Semantic analysis + type checking on the AST
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/typechecker.h"
#include "../include/lexer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Forward declarations ───────────────────────────────────── */
static Type* tc_check_expr (TypeChecker *tc, ASTNode *n);
static void  tc_check_stmt (TypeChecker *tc, ASTNode *n);
static void  tc_check_decl (TypeChecker *tc, ASTNode *n);

/* ── Helpers ─────────────────────────────────────────────────── */
static void tc_error(TypeChecker *tc, ASTNode *n,
                      RigErrorKind kind, const char *msg,
                      const char *sug, const char *fix) {
    riglog_add(tc->log, (int)kind, tc->file,
               n ? n->line : 0, n ? n->col : 0,
               msg, "", sug, fix);
    tc->error_count++;
}

static void tc_warn(TypeChecker *tc, ASTNode *n,
                     RigErrorKind kind, const char *msg) {
    riglog_add(tc->log, (int)kind, tc->file,
               n ? n->line : 0, n ? n->col : 0,
               msg, "", "", "");
    tc->warning_count++;
}

static Type* make_builtin(TypeChecker *tc, TypeKind k) {
    return ast_type_new(tc->arena, k);
}

/* ── Lifecycle ──────────────────────────────────────────────── */
TypeChecker* tc_new(ASTArena *arena, RigErrorLog *log, const char *file) {
    TypeChecker *tc = calloc(1, sizeof(TypeChecker));
    if (!tc) return NULL;
    tc->arena  = arena;
    tc->log    = log;
    tc->file   = file ? file : "<unknown>";
    tc->syms   = symtab_new();
    return tc;
}

void tc_free(TypeChecker *tc) {
    if (!tc) return;
    symtab_free(tc->syms);
    free(tc);
}

/* ── Register builtin functions ─────────────────────────────── */
void tc_register_builtins(TypeChecker *tc) {
    if (!tc) return;
    /* printf, scanf, malloc, free, etc. */
    static const struct { const char *name; TypeKind ret; } builtins[] = {
        {"printf",   TY_INT},
        {"fprintf",  TY_INT},
        {"sprintf",  TY_INT},
        {"snprintf", TY_INT},
        {"scanf",    TY_INT},
        {"malloc",   TY_PTR},
        {"calloc",   TY_PTR},
        {"realloc",  TY_PTR},
        {"free",     TY_VOID},
        {"memcpy",   TY_PTR},
        {"memmove",  TY_PTR},
        {"memset",   TY_PTR},
        {"strlen",   TY_ULONG},
        {"strcmp",   TY_INT},
        {"strcpy",   TY_PTR},
        {"strcat",   TY_PTR},
        {"strdup",   TY_PTR},
        {"strrchr",  TY_PTR},
        {"strstr",   TY_PTR},
        {"atoi",     TY_INT},
        {"atof",     TY_DOUBLE},
        {"strtoll",  TY_LONG},
        {"strtod",   TY_DOUBLE},
        {"exit",     TY_VOID},
        {"abort",    TY_VOID},
        {"puts",     TY_INT},
        {"fopen",    TY_PTR},
        {"fclose",   TY_INT},
        {"fread",    TY_ULONG},
        {"fwrite",   TY_ULONG},
        {"fgets",    TY_PTR},
        {"fputs",    TY_INT},
        {"fflush",   TY_INT},
        {"feof",     TY_INT},
        {"ferror",   TY_INT},
        {"rewind",   TY_VOID},
        {"stat",     TY_INT},
        {"opendir",  TY_PTR},
        {"readdir",  TY_PTR},
        {"closedir", TY_INT},
        {"time",     TY_LONG},
        {"pthread_create",  TY_INT},
        {"pthread_join",    TY_INT},
        {"pthread_mutex_lock",    TY_INT},
        {"pthread_mutex_unlock",  TY_INT},
        {"pthread_cond_wait",     TY_INT},
        {"pthread_cond_signal",   TY_INT},
        {"pthread_cond_broadcast",TY_INT},
        {NULL, TY_VOID}
    };

    for (int i = 0; builtins[i].name; i++) {
        Type *ft = ast_type_new(tc->arena, TY_FUNC);
        ft->ret      = ast_type_new(tc->arena, builtins[i].ret);
        ft->variadic = true;
        symtab_define(tc->syms, builtins[i].name, SYM_FUNC, ft, NULL);
    }
}

/* ═══════════════════════════════════════════════════════════════
   EXPRESSION TYPE CHECKING
   ═══════════════════════════════════════════════════════════════ */


/* ═══════════════════════════════════════════════════════════════
   CONVERSIONES IMPLICITAS C11 — COMPLETAS
   Cubre: void*, array-to-pointer decay, numeric promotion,
          NULL literal, pointer arithmetic, implicit int.
   ═══════════════════════════════════════════════════════════════ */
static bool tc_types_compatible(Type *a, Type *b) {
    if (!a || !b) return true;  /* unknown — permissive */
    if (a->kind == b->kind) return true;

    /* void* es compatible con cualquier puntero (C11 §6.3.2.3) */
    if (a->kind == TY_PTR && b->kind == TY_PTR) {
        if (a->base && a->base->kind == TY_VOID) return true;
        if (b->base && b->base->kind == TY_VOID) return true;
        return true; /* ptr-ptr always compatible for assignment */
    }

    /* Array-to-pointer decay: T[] → T* (C11 §6.3.2.1) */
    if (a->kind == TY_PTR && b->kind == TY_ARRAY) {
        if (!a->base || !b->base) return true;
        return (a->base->kind == b->base->kind);
    }
    if (a->kind == TY_ARRAY && b->kind == TY_PTR) {
        if (!a->base || !b->base) return true;
        return (a->base->kind == b->base->kind);
    }

    /* Numeric promotions (C11 §6.3.1) */
    bool a_num = (a->kind >= TY_BOOL && a->kind <= TY_DOUBLE);
    bool b_num = (b->kind >= TY_BOOL && b->kind <= TY_DOUBLE);
    if (a_num && b_num) return true;

    /* NULL literal: integer 0 → pointer (C11 §6.3.2.3p3) */
    if (a->kind == TY_PTR && type_is_integer(b)) return true;
    if (b->kind == TY_PTR && type_is_integer(a)) return true;

    /* Function pointer vs pointer */
    if (a->kind == TY_FUNC && b->kind == TY_PTR) return true;
    if (a->kind == TY_PTR  && b->kind == TY_FUNC) return true;

    /* Enum → int */
    if (a->kind == TY_ENUM && type_is_integer(b)) return true;
    if (b->kind == TY_ENUM && type_is_integer(a)) return true;

    /* Typedef resolution */
    if (a->kind == TY_TYPEDEF_REF || b->kind == TY_TYPEDEF_REF) return true;

    return false;
}

/* Array decay: si el tipo es TY_ARRAY, retorna puntero al elemento */
static Type* tc_decay(TypeChecker *tc, Type *t) {
    if (!t) return t;
    if (t->kind == TY_ARRAY) {
        Type *pt = ast_type_new(tc->arena, TY_PTR);
        pt->base = t->base;
        return pt;
    }
    return t;
}

static Type* tc_check_expr(TypeChecker *tc, ASTNode *n) {
    if (!n) return NULL;

    switch (n->kind) {
        case NODE_INT_LIT:
            n->resolved_type = make_builtin(tc, TY_INT);
            return n->resolved_type;

        case NODE_FLOAT_LIT:
            n->resolved_type = make_builtin(tc, TY_DOUBLE);
            return n->resolved_type;

        case NODE_STRING_LIT:
        case NODE_CHAR_LIT: {
            Type *pt = ast_type_new(tc->arena, TY_PTR);
            pt->base = make_builtin(tc, TY_CHAR);
            n->resolved_type = (n->kind == NODE_CHAR_LIT)
                               ? make_builtin(tc, TY_INT) : pt;
            return n->resolved_type;
        }

        case NODE_IDENT_EXPR: {
            Symbol *sym = symtab_lookup(tc->syms, n->ident.name);
            if (!sym) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Símbolo no definido: '%s'", n->ident.name);
                tc_error(tc, n, ERR_UNDEFINED_SYMBOL, msg,
                         "Declare la variable antes de usarla",
                         "int variable = 0;");
                n->resolved_type = make_builtin(tc, TY_INT);
            } else {
                n->resolved_type = sym->type;
            }
            return n->resolved_type;
        }

        case NODE_UNARY_EXPR:
        case NODE_ADDR_EXPR:
        case NODE_DEREF_EXPR: {
            Type *inner = tc_check_expr(tc, n->unary.operand);
            if (n->kind == NODE_ADDR_EXPR) {
                Type *pt = ast_type_new(tc->arena, TY_PTR);
                pt->base = inner;
                n->resolved_type = pt;
            } else if (n->kind == NODE_DEREF_EXPR) {
                if (inner && type_is_pointer(inner))
                    n->resolved_type = inner->base ? inner->base
                                                   : make_builtin(tc, TY_INT);
                else {
                    tc_error(tc, n, ERR_TYPE_MISMATCH,
                             "Desreferencia de tipo no-puntero",
                             "Usa un puntero con '*'", "int *p;");
                    n->resolved_type = make_builtin(tc, TY_INT);
                }
            } else {
                n->resolved_type = inner;
            }
            return n->resolved_type;
        }

        case NODE_BINARY_EXPR: {
            Type *lt = tc_check_expr(tc, n->binary.left);
            Type *rt = tc_check_expr(tc, n->binary.right);
            lt = tc_decay(tc, lt); rt = tc_decay(tc, rt);
            if (lt && rt && !tc_types_compatible(lt, rt)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Operandos incompatibles: '%s' y '%s'",
                         type_name(lt), type_name(rt));
                tc_error(tc, n, ERR_TYPE_MISMATCH, msg,
                         "Use un cast explícito si es necesario",
                         "(int)expr");
            }
            /* Comparison → bool */
            int op = n->binary.op;
            if (op == (int)TOK_EQ_EQ || op == (int)TOK_BANG_EQ ||
                op == (int)TOK_LT    || op == (int)TOK_LT_EQ   ||
                op == (int)TOK_GT    || op == (int)TOK_GT_EQ) {
                n->resolved_type = make_builtin(tc, TY_BOOL);
            } else {
                n->resolved_type = lt ? lt : rt;
            }
            return n->resolved_type;
        }

        case NODE_ASSIGN_EXPR: {
            Type *lt = tc_check_expr(tc, n->assign.lhs);
            Type *rt = tc_check_expr(tc, n->assign.rhs);
            lt = tc_decay(tc, lt); rt = tc_decay(tc, rt);
            if (lt && rt && !tc_types_compatible(lt, rt)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Asignación incompatible: '%s' = '%s'",
                         type_name(lt), type_name(rt));
                /* Special case: int a = "hola" */
                if (type_is_integer(lt) &&
                    (rt->kind == TY_PTR &&
                     rt->base && rt->base->kind == TY_CHAR)) {
                    tc_error(tc, n, ERR_TYPE_MISMATCH, msg,
                             "No se puede asignar string a entero",
                             "const char *s = \"hola\";");
                } else if (type_is_pointer(lt) && type_is_integer(rt)) {
                    tc_warn(tc, n, WARN_IMPLICIT_CONV,
                            "Conversión implícita de entero a puntero");
                } else {
                    tc_error(tc, n, ERR_TYPE_MISMATCH, msg,
                             "Usa un cast explícito",
                             "(TargetType)expr");
                }
            }
            n->resolved_type = lt;
            return n->resolved_type;
        }

        case NODE_TERNARY_EXPR: {
            tc_check_expr(tc, n->ternary.cond);
            Type *t1 = tc_check_expr(tc, n->ternary.then_expr);
            Type *t2 = tc_check_expr(tc, n->ternary.else_expr);
            if (t1 && t2 && !tc_types_compatible(t1, t2)) {
                tc_error(tc, n, ERR_TYPE_MISMATCH,
                         "Ramas del ternario con tipos incompatibles",
                         "Asegura que ambas ramas tengan el mismo tipo",
                         "cond ? (int)a : b");
            }
            n->resolved_type = t1 ? t1 : t2;
            return n->resolved_type;
        }

        case NODE_CALL_EXPR: {
            /* Check callee */
            Type *callee_ty = tc_check_expr(tc, n->call.callee);
            Type *ret_ty = NULL;

            if (callee_ty) {
                if (callee_ty->kind == TY_FUNC)
                    ret_ty = callee_ty->ret;
                else if (callee_ty->kind == TY_PTR &&
                         callee_ty->base &&
                         callee_ty->base->kind == TY_FUNC)
                    ret_ty = callee_ty->base->ret;
            }

            /* Check argument types */
            for (uint32_t i = 0; i < n->call.n_args; i++)
                tc_check_expr(tc, n->call.args[i]);

            n->resolved_type = ret_ty ? ret_ty
                                      : make_builtin(tc, TY_INT);
            return n->resolved_type;
        }

        case NODE_INDEX_EXPR: {
            Type *arr_ty = tc_check_expr(tc, n->idx.array);
            Type *idx_ty = tc_check_expr(tc, n->idx.index);
            if (idx_ty && !type_is_integer(idx_ty)) {
                tc_error(tc, n, ERR_TYPE_MISMATCH,
                         "Índice de array debe ser entero",
                         "Usa un índice entero",
                         "arr[0]");
            }
            if (arr_ty && arr_ty->kind == TY_ARRAY)
                n->resolved_type = arr_ty->base;
            else if (arr_ty && arr_ty->kind == TY_PTR)
                n->resolved_type = arr_ty->base;
            else
                n->resolved_type = make_builtin(tc, TY_INT);
            return n->resolved_type;
        }

        case NODE_MEMBER_EXPR: {
            Type *obj_type = tc_check_expr(tc, n->member.object);
            /* Dereference pointer for arrow operator */
            if (obj_type && obj_type->kind == TY_PTR && n->member.arrow)
                obj_type = obj_type->base;

            if (!obj_type || (obj_type->kind != TY_STRUCT && obj_type->kind != TY_UNION)) {
                tc_error(tc, n, ERR_TYPE_MISMATCH,
                         "acceso a campo en tipo que no es struct/union", "", "");
                n->resolved_type = make_builtin(tc, TY_INT);
                return n->resolved_type;
            }
            /* Walk the field list looking for member.field */
            StructField *sf = obj_type->fields;
            while (sf) {
                if (sf->name && strcmp(sf->name, n->member.field) == 0) {
                    n->resolved_type = sf->type;
                    return n->resolved_type;
                }
                sf = sf->next;
            }
            /* Field not found — compose error message */
            char errmsg[256];
            snprintf(errmsg, sizeof(errmsg),
                     "campo '%s' no existe en struct '%s'",
                     n->member.field,
                     obj_type->name ? obj_type->name : "<anonimo>");
            tc_error(tc, n, ERR_UNDEFINED_SYMBOL, errmsg, "", "");
            n->resolved_type = make_builtin(tc, TY_INT);
            return n->resolved_type;
        }

        case NODE_SIZEOF_EXPR:
            if (!n->szof.is_type)
                tc_check_expr(tc, n->szof.expr);
            n->resolved_type = make_builtin(tc, TY_ULONG);
            return n->resolved_type;

        case NODE_CAST_EXPR:
            tc_check_expr(tc, n->cast.expr);
            n->resolved_type = n->cast.to;
            return n->resolved_type;

        default:
            return NULL;
    }
}

/* ═══════════════════════════════════════════════════════════════
   STATEMENT TYPE CHECKING
   ═══════════════════════════════════════════════════════════════ */

static void tc_check_stmt(TypeChecker *tc, ASTNode *n) {
    if (!n) return;

    switch (n->kind) {
        case NODE_EXPR_STMT:
            tc_check_expr(tc, n->expr_stmt.expr);
            break;

        case NODE_BLOCK:
            symtab_push_scope(tc->syms);
            for (uint32_t i = 0; i < n->block.n_stmts; i++)
                tc_check_stmt(tc, n->block.stmts[i]);
            symtab_pop_scope(tc->syms);
            break;

        case NODE_RETURN_STMT: {
            Type *ret_ty = tc_check_expr(tc, n->ret.value);
            if (tc->current_func_ret) {
                if (n->ret.value && ret_ty &&
                    !tc_types_compatible(tc->current_func_ret, ret_ty)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Tipo de retorno incompatible: función "
                             "devuelve '%s', se encontró '%s'",
                             type_name(tc->current_func_ret),
                             type_name(ret_ty));
                    tc_error(tc, n, ERR_TYPE_MISMATCH, msg,
                             "Verifica el tipo de retorno de la función",
                             "return (ReturnType)expr;");
                }
            }
            break;
        }

        case NODE_IF_STMT:
            tc_check_expr(tc, n->if_stmt.cond);
            tc_check_stmt(tc, n->if_stmt.then_br);
            if (n->if_stmt.else_br)
                tc_check_stmt(tc, n->if_stmt.else_br);
            break;

        case NODE_WHILE_STMT:
            tc_check_expr(tc, n->while_stmt.cond);
            {
                bool prev = tc->in_loop;
                tc->in_loop = true;
                tc_check_stmt(tc, n->while_stmt.body);
                tc->in_loop = prev;
            }
            break;

        case NODE_FOR_STMT:
            symtab_push_scope(tc->syms);
            tc_check_stmt(tc, n->for_stmt.init);
            tc_check_expr(tc, n->for_stmt.cond);
            tc_check_expr(tc, n->for_stmt.step);
            {
                bool prev = tc->in_loop;
                tc->in_loop = true;
                tc_check_stmt(tc, n->for_stmt.body);
                tc->in_loop = prev;
            }
            symtab_pop_scope(tc->syms);
            break;

        case NODE_DO_WHILE_STMT:
            {
                bool prev = tc->in_loop;
                tc->in_loop = true;
                tc_check_stmt(tc, n->do_while.body);
                tc->in_loop = prev;
            }
            tc_check_expr(tc, n->do_while.cond);
            break;

        case NODE_BREAK_STMT:
        case NODE_CONTINUE_STMT:
            if (!tc->in_loop) {
                tc_error(tc, n, ERR_INVALID_OPERATION,
                         (n->kind == NODE_BREAK_STMT)
                         ? "break fuera de bucle"
                         : "continue fuera de bucle",
                         "Coloca este statement dentro de for/while/do-while",
                         "while(cond) { break; }");
            }
            break;

        case NODE_VAR_DECL:
        case NODE_PARAM_DECL:
            tc_check_decl(tc, n);
            break;

        default:
            /* Handle function decls found inside blocks */
            tc_check_decl(tc, n);
            break;
    }
}

/* ═══════════════════════════════════════════════════════════════
   DECLARATION TYPE CHECKING
   ═══════════════════════════════════════════════════════════════ */

static void tc_check_decl(TypeChecker *tc, ASTNode *n) {
    if (!n) return;

    switch (n->kind) {
        case NODE_VAR_DECL:
        case NODE_PARAM_DECL: {
            const char *name = n->var_decl.name;
            Type       *ty   = n->var_decl.decl_type;

            /* Check initializer type compatibility */
            if (n->var_decl.init) {
                Type *init_ty = tc_check_expr(tc, n->var_decl.init);
                init_ty = tc_decay(tc, init_ty);
                if (ty && init_ty && !tc_types_compatible(ty, init_ty)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "'%s': tipo de inicializador '%s' "
                             "incompatible con '%s'",
                             name ? name : "?",
                             type_name(init_ty), type_name(ty));
                    tc_error(tc, n, ERR_TYPE_MISMATCH, msg,
                             "Asegúrate de que el tipo del inicializador coincida",
                             "int x = 42;  // no: int x = \"hola\";");
                }
            }

            /* Register symbol, detect redefinition */
            if (name && name[0]) {
                bool ok = symtab_define(tc->syms, name,
                                        (n->kind == NODE_PARAM_DECL)
                                        ? SYM_PARAM : SYM_VAR,
                                        ty, n);
                if (!ok) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Redefinición de '%s' en el mismo ámbito", name);
                    tc_error(tc, n, ERR_REDEFINITION, msg,
                             "Usa un nombre diferente o elimina la declaración duplicada",
                             "int x2 = 0;");
                }
            }
            break;
        }

        case NODE_FUNC_DECL:
        case NODE_FUNC_DEF: {
            const char *name    = n->func.name;
            Type       *ret     = n->func.ret_type;

            /* Build function type */
            Type *ft = ast_type_new(tc->arena, TY_FUNC);
            ft->ret      = ret;
            ft->variadic = n->func.params.variadic;

            /* Register in current scope */
            if (name && name[0]) {
                symtab_define(tc->syms, name, SYM_FUNC, ft, n);
            }

            /* Check body if this is a definition */
            if (n->kind == NODE_FUNC_DEF && n->func.body) {
                Type *prev_ret = tc->current_func_ret;
                tc->current_func_ret = ret;

                symtab_push_scope(tc->syms);

                /* Register parameters */
                ParamList *pl = &n->func.params;
                for (uint32_t i = 0; i < pl->count; i++) {
                    ASTNode *param = pl->params[i];
                    if (param && param->var_decl.name) {
                        symtab_define(tc->syms, param->var_decl.name,
                                      SYM_PARAM,
                                      param->var_decl.decl_type, param);
                    }
                }

                tc_check_stmt(tc, n->func.body);

                symtab_pop_scope(tc->syms);
                tc->current_func_ret = prev_ret;
            }
            break;
        }

        case NODE_TYPEDEF_DECL:
            if (n->typedef_decl.alias && n->typedef_decl.alias[0]) {
                symtab_define(tc->syms, n->typedef_decl.alias,
                              SYM_TYPE, n->typedef_decl.of, n);
            }
            break;

        default:
            /* Statement context */
            tc_check_stmt(tc, n);
            break;
    }
}

/* ═══════════════════════════════════════════════════════════════
   ENTRY POINT
   ═══════════════════════════════════════════════════════════════ */

bool tc_check(TypeChecker *tc, ASTNode *translation_unit) {
    if (!tc || !translation_unit) return false;
    if (translation_unit->kind != NODE_TRANSLATION_UNIT) return false;

    /* Global scope */
    tc_register_builtins(tc);

    for (uint32_t i = 0; i < translation_unit->tu.n_decls; i++) {
        ASTNode *decl = translation_unit->tu.decls[i];
        tc_check_decl(tc, decl);
    }

    return !riglog_has_errors(tc->log);
}
