/* ============================================================
   RigCom v8.0 — src/parser.c
   Recursive-descent parser for C / .rigc
   Produces a full AST from the token stream.
   ============================================================ */
#include "../include/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Forward declarations ───────────────────────────────────── */
static ASTNode* parse_decl_or_stmt      (Parser *p);
static ASTNode* parse_func_or_var_decl  (Parser *p);
static ASTNode* parse_block             (Parser *p);
static ASTNode* parse_stmt_inner        (Parser *p);
static ASTNode* parse_expr_prec         (Parser *p, int min_prec);
static ASTNode* parse_unary             (Parser *p);
static ASTNode* parse_postfix           (Parser *p, ASTNode *base);
static ASTNode* parse_primary           (Parser *p);
static Type*    parse_type_specifier    (Parser *p);
static Type*    parse_type_suffix       (Parser *p, Type *base);
static char*    parse_declarator_name   (Parser *p, Type **out_type, Type *base);
static ParamList parse_param_list       (Parser *p);

/* ── Init ───────────────────────────────────────────────────── */
void parser_init(Parser *p, Lexer *lx, RigErrorLog *log,
                  ASTArena *arena, const char *file) {
    p->lx          = lx;
    p->log         = log;
    p->arena       = arena;
    p->file        = file;
    p->panic_mode  = false;
    p->error_count = 0;
    /* Prime the lookahead */
    p->current    = lexer_next(lx);
    p->lookahead  = lexer_next(lx);
}

/* ── Token consumption ──────────────────────────────────────── */
static Token advance(Parser *p) {
    Token t     = p->current;
    p->current  = p->lookahead;
    p->lookahead = lexer_next(p->lx);
    return t;
}

static Token peek(const Parser *p) __attribute__((unused));
static Token peek(const Parser *p)  { return p->current;  }
static Token peek2(const Parser *p) { return p->lookahead; }

static bool check(const Parser *p, TokenKind k) {
    return p->current.kind == k;
}

static bool match_tok(Parser *p, TokenKind k) {
    if (p->current.kind == k) { advance(p); return true; }
    return false;
}

static Token expect(Parser *p, TokenKind k, const char *msg) {
    if (p->current.kind == k) return advance(p);
    /* Error recovery */
    if (!p->panic_mode) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Se esperaba %s; se encontró '%.*s'",
                 msg, (int)p->current.len, p->current.start);
        riglog_add(p->log, ERR_SYNTAX, p->file,
                   p->current.line, p->current.col,
                   buf, "", "Revisa la sintaxis", "");
        p->error_count++;
        p->panic_mode = true;
    }
    return p->current;
}

/* ── Synchronize after error ────────────────────────────────── */
static void synchronize(Parser *p) {
    p->panic_mode = false;
    while (!check(p, TOK_EOF)) {
        if (p->current.kind == TOK_SEMI) { advance(p); return; }
        switch (p->current.kind) {
            case TOK_KW_INT:    case TOK_KW_CHAR:
            case TOK_KW_FLOAT:  case TOK_KW_DOUBLE:
            case TOK_KW_VOID:   case TOK_KW_LONG:
            case TOK_KW_STRUCT: case TOK_KW_RETURN:
            case TOK_KW_IF:     case TOK_KW_WHILE:
            case TOK_KW_FOR:    case TOK_RBRACE:
                return;
            default: break;
        }
        advance(p);
    }
}

/* ── Intern token text into arena ───────────────────────────── */
static char* tok_intern(Parser *p, Token t) {
    return ast_intern(p->arena, t.start, t.len);
}

/* ── Node factories ─────────────────────────────────────────── */
static ASTNode* new_node(Parser *p, NodeKind k) {
    return ast_node_new(p->arena, k, p->current.line, p->current.col);
}
static ASTNode* new_node_at(Parser *p, NodeKind k, Token t) {
    return ast_node_new(p->arena, k, t.line, t.col);
}

/* ── Type allocation ────────────────────────────────────────── */
static Type* new_type(Parser *p, TypeKind k) {
    return ast_type_new(p->arena, k);
}

/* ── Type specifier ─────────────────────────────────────────── */
static Type* parse_type_specifier(Parser *p) {
    bool is_unsigned = false;
    bool is_const    = false;
    bool is_static   = false;
    bool is_extern   = false;

restart:
    if (match_tok(p, TOK_KW_CONST )) { is_const  = true; goto restart; }
    if (match_tok(p, TOK_KW_STATIC)) { is_static = true; (void)is_static; goto restart; }
    if (match_tok(p, TOK_KW_EXTERN)) { is_extern = true; (void)is_extern; goto restart; }
    if (match_tok(p, TOK_KW_INLINE)) { goto restart; }

    if (check(p, TOK_KW_UNSIGNED)) {
        advance(p);
        is_unsigned = true;
    }

    Type *t = NULL;
    Token tok = p->current;

    switch (tok.kind) {
        case TOK_KW_VOID:   advance(p); t = new_type(p, TY_VOID);   break;
        case TOK_KW_BOOL:   advance(p); t = new_type(p, TY_BOOL);   break;
        case TOK_KW_CHAR:   advance(p);
            t = new_type(p, is_unsigned ? TY_UCHAR : TY_CHAR);      break;
        case TOK_KW_SHORT:  advance(p);
            t = new_type(p, is_unsigned ? TY_USHORT : TY_SHORT);    break;
        case TOK_KW_INT:    advance(p);
            t = new_type(p, is_unsigned ? TY_UINT : TY_INT);        break;
        case TOK_KW_LONG:   advance(p);
            /* long long → LONG */
            if (check(p, TOK_KW_LONG)) advance(p);
            t = new_type(p, is_unsigned ? TY_ULONG : TY_LONG);      break;
        case TOK_KW_FLOAT:  advance(p); t = new_type(p, TY_FLOAT);  break;
        case TOK_KW_DOUBLE: advance(p); t = new_type(p, TY_DOUBLE); break;
        case TOK_KW_STRUCT: {
            advance(p);
            char *sname = NULL;
            if (check(p, TOK_IDENT)) {
                Token nt = advance(p);
                sname = tok_intern(p, nt);
            }
            t = new_type(p, TY_STRUCT);
            t->name = sname;
            /* Optional body — parse field declarations and populate fields */
            if (check(p, TOK_LBRACE)) {
                advance(p); /* skip { */
                uint32_t byte_offset = 0;
                while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                    /* Parse one field: type + name + ';' */
                    Type *ftype = parse_type_specifier(p);
                    if (!ftype) { match_tok(p, TOK_SEMI); continue; }
                    /* Handle pointer declarators */
                    while (match_tok(p, TOK_STAR)) {
                        Type *ptr = new_type(p, TY_PTR);
                        ptr->base = ftype;
                        ftype = ptr;
                    }
                    if (check(p, TOK_IDENT) || check(p, TOK_COLON)) {
                        /* Campo nombrado o bitfield anónimo */
                        char *fname_str = NULL;
                        if (check(p, TOK_IDENT)) {
                            Token fname = advance(p);
                            fname_str = tok_intern(p, fname);
                        }
                        /* Bitfield — campo : N (C11 §6.7.2.1) */
                        uint32_t bit_width = 0;
                        bool is_bitfield = false;
                        if (match_tok(p, TOK_COLON)) {
                            is_bitfield = true;
                            Token bw = advance(p); /* integer literal */
                            if (bw.kind == TOK_INT_LIT)
                                bit_width = (uint32_t)bw.int_val;
                        }
                        /* Append StructField to the linked list */
                        StructField *sf = calloc(1, sizeof(StructField));
                        sf->name   = fname_str ? fname_str : (char*)"";
                        sf->type   = ftype;
                        sf->offset = byte_offset;
                        sf->next   = NULL;
                        /* Size: bitfield rounds up to storage unit */
                        uint32_t fsz = 4;
                        if (is_bitfield) {
                            fsz = (bit_width + 7u) / 8u;
                            if (fsz == 0) fsz = 0; /* zero-width = padding only */
                        } else {
                            if (ftype->kind == TY_CHAR  || ftype->kind == TY_UCHAR)  fsz = 1;
                            else if (ftype->kind == TY_SHORT || ftype->kind == TY_USHORT) fsz = 2;
                            else if (ftype->kind == TY_LONG  || ftype->kind == TY_ULONG
                                     || ftype->kind == TY_DOUBLE || ftype->kind == TY_PTR) fsz = 8;
                            else if (ftype->kind == TY_FLOAT) fsz = 4;
                        }
                        byte_offset += fsz;
                        /* Link into t->fields */
                        if (!t->fields) {
                            t->fields = sf;
                        } else {
                            StructField *cur = t->fields;
                            while (cur->next) cur = cur->next;
                            cur->next = sf;
                        }
                        t->n_fields++;
                        /* Optional array suffix */
                        if (match_tok(p, TOK_LBRACKET)) {
                            parse_expr_prec(p, 0);
                            match_tok(p, TOK_RBRACKET);
                        }
                    }
                    match_tok(p, TOK_SEMI);
                }
                t->total_size = byte_offset;
                match_tok(p, TOK_RBRACE);
            }
            break;
        }
        case TOK_KW_UNION: {
            advance(p);
            char *uname = NULL;
            if (check(p, TOK_IDENT)) {
                Token nt = advance(p);
                uname = tok_intern(p, nt);
            }
            t = new_type(p, TY_UNION);
            t->name = uname;
            if (check(p, TOK_LBRACE)) {
                advance(p);
                while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF))
                    parse_decl_or_stmt(p);
                match_tok(p, TOK_RBRACE);
            }
            break;
        }
        case TOK_KW_ENUM: {
            advance(p);
            if (check(p, TOK_IDENT)) advance(p);
            t = new_type(p, TY_ENUM);
            if (check(p, TOK_LBRACE)) {
                advance(p);
                while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                    if (check(p, TOK_IDENT)) advance(p);
                    if (match_tok(p, TOK_EQ)) parse_expr_prec(p, 0);
                    match_tok(p, TOK_COMMA);
                }
                match_tok(p, TOK_RBRACE);
            }
            break;
        }
        case TOK_IDENT: {
            /* typedef reference */
            Token nt = advance(p);
            t = new_type(p, TY_TYPEDEF_REF);
            t->name = tok_intern(p, nt);
            break;
        }
        default:
            if (is_unsigned) {
                /* "unsigned" alone → unsigned int */
                t = new_type(p, TY_UINT);
            } else {
                t = new_type(p, TY_UNKNOWN);
            }
            break;
    }

    if (t) t->is_const = is_const;
    return t;
}

/* ── Type suffix: pointer / array decorators ────────────────── */
static Type* parse_type_suffix(Parser *p, Type *base) {
    /* Pointer */
    while (check(p, TOK_STAR)) {
        advance(p);
        bool is_const = false;
        if (check(p, TOK_KW_CONST)) { advance(p); is_const = true; }
        Type *ptr = new_type(p, TY_PTR);
        ptr->base     = base;
        ptr->is_const = is_const;
        base = ptr;
    }
    return base;
}

/* ── Declarator: parse *name[N] / (*name) etc ───────────────── */
static char* parse_declarator_name(Parser *p, Type **out_type, Type *base) {
    base = parse_type_suffix(p, base);

    /* Group: (*name) */
    if (check(p, TOK_LPAREN)) {
        /* Could be function pointer; simplified: skip */
        advance(p);
        char *name = NULL;
        if (check(p, TOK_STAR)) {
            advance(p);
            Type *ptr = new_type(p, TY_PTR);
            ptr->base = base;
            base = ptr;
        }
        if (check(p, TOK_IDENT)) {
            Token nt = advance(p);
            name = tok_intern(p, nt);
        }
        match_tok(p, TOK_RPAREN);
        /* Array suffix */
        while (check(p, TOK_LBRACKET)) {
            advance(p);
            uint32_t len = 0;
            if (check(p, TOK_INT_LIT)) {
                Token at = advance(p);
                len = (uint32_t)at.int_val;
            }
            match_tok(p, TOK_RBRACKET);
            Type *arr = new_type(p, TY_ARRAY);
            arr->base      = base;
            arr->array_len = len;
            base = arr;
        }
        *out_type = base;
        return name;
    }

    /* Direct name */
    char *name = NULL;
    if (check(p, TOK_IDENT)) {
        Token nt = advance(p);
        name = tok_intern(p, nt);
    }

    /* Array suffix: name[N] */
    while (check(p, TOK_LBRACKET)) {
        advance(p);
        uint32_t len = 0;
        if (check(p, TOK_INT_LIT)) {
            Token at = advance(p);
            len = (uint32_t)at.int_val;
        } else if (!check(p, TOK_RBRACKET)) {
            /* constant expr: just skip for now */
            ASTNode *_ = parse_expr_prec(p, 0);
            (void)_;
        }
        match_tok(p, TOK_RBRACKET);
        Type *arr = new_type(p, TY_ARRAY);
        arr->base      = base;
        arr->array_len = len;
        base = arr;
    }

    *out_type = base;
    return name;
}

/* ── Parameter list ─────────────────────────────────────────── */
static ParamList parse_param_list(Parser *p) {
    ParamList pl;
    memset(&pl, 0, sizeof(pl));
    pl.capacity = 8;
    pl.params   = malloc(pl.capacity * sizeof(ASTNode *));

    expect(p, TOK_LPAREN, "(");
    if (check(p, TOK_RPAREN)) { advance(p); return pl; }

    /* void */
    if (check(p, TOK_KW_VOID) && peek2(p).kind == TOK_RPAREN) {
        advance(p); advance(p); return pl;
    }

    do {
        if (check(p, TOK_ELLIPSIS)) {
            advance(p);
            pl.variadic = true;
            break;
        }
        Type *base = parse_type_specifier(p);
        Type *t;
        char *name = parse_declarator_name(p, &t, base);

        ASTNode *param = new_node(p, NODE_PARAM_DECL);
        param->var_decl.name      = name ? name : (char*)"";
        param->var_decl.decl_type = t;
        param->var_decl.init      = NULL;

        if (pl.count >= pl.capacity) {
            pl.capacity *= 2;
            ASTNode **tmp = realloc(pl.params, pl.capacity * sizeof(ASTNode *));
            if (tmp) pl.params = tmp;
        }
        pl.params[pl.count++] = param;

    } while (match_tok(p, TOK_COMMA));

    expect(p, TOK_RPAREN, ")");
    return pl;
}

/* ── Top-level parse ────────────────────────────────────────── */
ASTNode* parser_parse(Parser *p) {
    ASTNode *tu = ast_node_new(p->arena, NODE_TRANSLATION_UNIT, 1, 1);
    if (!tu) return NULL;

    tu->tu.cap    = 256;
    tu->tu.decls  = malloc(tu->tu.cap * sizeof(ASTNode *));
    tu->tu.n_decls = 0;

    while (!check(p, TOK_EOF)) {
        if (p->panic_mode) synchronize(p);

        /* Skip preprocessor lines (#include, #define, etc.) */
        if (check(p, TOK_HASH)) {
            while (!check(p, TOK_EOF) &&
                   p->current.line == p->lx->line - 1) advance(p);
            /* skip to end of line */
            uint32_t cur_line = p->current.line;
            while (!check(p, TOK_EOF) && p->current.line == cur_line)
                advance(p);
            continue;
        }

        /* typedef */
        if (check(p, TOK_KW_TYPEDEF)) {
            advance(p);
            Type *base = parse_type_specifier(p);
            Type *t;
            char *name = parse_declarator_name(p, &t, base);
            match_tok(p, TOK_SEMI);
            ASTNode *td = new_node(p, NODE_TYPEDEF_DECL);
            td->typedef_decl.alias = name ? name : (char*)"";
            td->typedef_decl.of    = t;
            if (tu->tu.n_decls < tu->tu.cap)
                tu->tu.decls[tu->tu.n_decls++] = td;
            continue;
        }

        ASTNode *decl = parse_func_or_var_decl(p);
        if (!decl) { synchronize(p); continue; }
        if (tu->tu.n_decls >= tu->tu.cap) {
            tu->tu.cap *= 2;
            ASTNode **tmp = realloc(tu->tu.decls,
                                     tu->tu.cap * sizeof(ASTNode *));
            if (tmp) tu->tu.decls = tmp;
        }
        tu->tu.decls[tu->tu.n_decls++] = decl;
    }
    return tu;
}

/* ── Parse function definition OR global variable ───────────── */
static ASTNode* parse_func_or_var_decl(Parser *p) {
    bool is_static = false;
    bool is_inline = false;
    bool is_extern = false;

    /* Storage class + inline prefix */
    for (;;) {
        if (check(p, TOK_KW_STATIC)) { is_static = true; advance(p); continue; }
        if (check(p, TOK_KW_INLINE)) { is_inline = true; advance(p); continue; }
        if (check(p, TOK_KW_EXTERN)) { is_extern = true; (void)is_extern; advance(p); continue; }
        break;
    }

    Type *base = parse_type_specifier(p);
    Type *t;
    char *name = parse_declarator_name(p, &t, base);

    if (!name) {
        /* Bare struct/enum def ending in ';' */
        match_tok(p, TOK_SEMI);
        return NULL;
    }

    /* Function: name followed by '(' */
    if (check(p, TOK_LPAREN)) {
        /* Re-parse params */
        ParamList pl = parse_param_list(p);
        ASTNode *fn  = new_node(p, NODE_FUNC_DEF);
        fn->func.name      = name;
        fn->func.ret_type  = t;
        fn->func.params    = pl;
        fn->func.is_static = is_static;
        fn->func.is_inline = is_inline;

        /* Check for declaration (ends with ;) vs definition ({ body }) */
        if (check(p, TOK_SEMI)) {
            advance(p);
            fn->kind       = NODE_FUNC_DECL;
            fn->func.body  = NULL;
        } else if (check(p, TOK_LBRACE)) {
            fn->func.body = parse_block(p);
        } else {
            /* Empty/forward declaration */
            match_tok(p, TOK_SEMI);
            fn->kind      = NODE_FUNC_DECL;
            fn->func.body = NULL;
        }
        return fn;
    }

    /* Variable declaration (possibly list: int a, b, c; ) */
    ASTNode *var = new_node(p, NODE_VAR_DECL);
    var->var_decl.name      = name;
    var->var_decl.decl_type = t;
    var->var_decl.is_static = is_static;

    if (match_tok(p, TOK_EQ)) {
        /* Handle array initializer list */
        if (check(p, TOK_LBRACE)) {
            advance(p);
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                parse_expr_prec(p, 0);
                match_tok(p, TOK_COMMA);
            }
            match_tok(p, TOK_RBRACE);
            var->var_decl.init = NULL; /* simplified */
        } else {
            var->var_decl.init = parse_expr_prec(p, 0);
        }
    }
    /* Skip comma-separated declarators for simplicity */
    while (check(p, TOK_COMMA)) {
        advance(p);
        Type *t2; char *n2 = parse_declarator_name(p, &t2, base);
        if (match_tok(p, TOK_EQ)) parse_expr_prec(p, 0);
        (void)n2; (void)t2;
    }
    match_tok(p, TOK_SEMI);
    return var;
}

/* ── Block: { statement* } ──────────────────────────────────── */
static ASTNode* parse_block(Parser *p) {
    ASTNode *blk = new_node(p, NODE_BLOCK);
    blk->block.cap = 32;
    blk->block.n_stmts    = 0;
    blk->block.stmts    = malloc(blk->block.cap * sizeof(ASTNode *));

    expect(p, TOK_LBRACE, "{");

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (p->panic_mode) synchronize(p);
        ASTNode *s = parse_decl_or_stmt(p);
        if (!s) continue;
        if (blk->block.n_stmts >= blk->block.cap) {
            blk->block.cap *= 2;
            ASTNode **tmp = realloc(blk->block.stmts,
                                     blk->block.cap * sizeof(ASTNode *));
            if (tmp) blk->block.stmts = tmp;
        }
        blk->block.stmts[blk->block.n_stmts++] = s;
    }
    expect(p, TOK_RBRACE, "}");
    return blk;
}

/* ── Parse statement or local declaration ───────────────────── */
static ASTNode* parse_decl_or_stmt(Parser *p) {
    /* Local declaration? */
    TokenKind k = p->current.kind;
    bool is_type_kw = tok_is_type_start(k);
    bool is_storage = (k == TOK_KW_STATIC || k == TOK_KW_EXTERN ||
                       k == TOK_KW_INLINE || k == TOK_KW_CONST  ||
                       k == TOK_KW_TYPEDEF);

    if (is_type_kw || is_storage) {
        /* Could also be typedef inside a block */
        if (k == TOK_KW_TYPEDEF) {
            advance(p);
            Type *base = parse_type_specifier(p);
            Type *t; char *nm = parse_declarator_name(p, &t, base);
            match_tok(p, TOK_SEMI);
            ASTNode *td = new_node(p, NODE_TYPEDEF_DECL);
            td->typedef_decl.alias = nm ? nm : (char*)"";
            td->typedef_decl.of    = t;
            return td;
        }
        /* Local variable declaration */
        bool is_static = false;
        bool is_const  = false;
        for(;;) {
            if (check(p, TOK_KW_STATIC)) { is_static=true; advance(p); continue; }
            if (check(p, TOK_KW_EXTERN)) { advance(p); continue; }
            if (check(p, TOK_KW_INLINE)) { advance(p); continue; }
            if (check(p, TOK_KW_CONST))  { is_const =true; advance(p); continue; }
            break;
        }
        Type *base = parse_type_specifier(p);
        if (base && is_const) base->is_const = true;
        Type *t; char *nm = parse_declarator_name(p, &t, base);
        if (!nm) { match_tok(p, TOK_SEMI); return NULL; }

        /* Might be a function pointer typedef var or a func call? */
        if (check(p, TOK_LPAREN)) {
            /* func call through pointer — treat as expression stmt */
            /* Back-track not possible; emit a call node */
            ASTNode *callee = new_node(p, NODE_IDENT_EXPR);
            callee->ident.name = nm;
            ParamList args;
            memset(&args, 0, sizeof(args));
            /* parse as call expression but simpler */
            advance(p); /* consume '(' */
            ASTNode *call_node = new_node(p, NODE_CALL_EXPR);
            call_node->call.callee = callee;
            call_node->call.cap    = 8;
            call_node->call.args   = malloc(8 * sizeof(ASTNode*));
            call_node->call.n_args = 0;
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                ASTNode *arg = parse_expr_prec(p, 0);
                if (call_node->call.n_args < call_node->call.cap)
                    call_node->call.args[call_node->call.n_args++] = arg;
                match_tok(p, TOK_COMMA);
            }
            match_tok(p, TOK_RPAREN);
            match_tok(p, TOK_SEMI);
            ASTNode *es = new_node(p, NODE_EXPR_STMT);
            es->expr_stmt.expr = call_node;
            return es;
        }

        ASTNode *var = new_node(p, NODE_VAR_DECL);
        var->var_decl.name      = nm;
        var->var_decl.decl_type = t;
        var->var_decl.is_static = is_static;

        if (match_tok(p, TOK_EQ)) {
            if (check(p, TOK_LBRACE)) {
                advance(p);
                while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                    parse_expr_prec(p, 0);
                    match_tok(p, TOK_COMMA);
                }
                match_tok(p, TOK_RBRACE);
            } else {
                var->var_decl.init = parse_expr_prec(p, 0);
            }
        }
        /* Multi-declarator: int a=1, b=2; */
        while (check(p, TOK_COMMA)) {
            advance(p);
            Type *t2; char *n2 = parse_declarator_name(p, &t2, base);
            if (n2 && match_tok(p, TOK_EQ)) parse_expr_prec(p, 0);
            (void)n2; (void)t2;
        }
        match_tok(p, TOK_SEMI);
        return var;
    }

    /* Identifier that might be a typedef type (heuristic) */
    if (k == TOK_IDENT && peek2(p).kind != TOK_LPAREN &&
        peek2(p).kind != TOK_EQ    &&
        peek2(p).kind != TOK_SEMI  &&
        peek2(p).kind != TOK_DOT   &&
        peek2(p).kind != TOK_ARROW &&
        peek2(p).kind != TOK_LBRACKET &&
        peek2(p).kind != TOK_PLUS_PLUS &&
        peek2(p).kind != TOK_MINUS_MINUS) {
        /* Could be typedef-based declaration: MyType var; */
        Token next = peek2(p);
        if (next.kind == TOK_IDENT || next.kind == TOK_STAR) {
            Type *base = parse_type_specifier(p);
            Type *t; char *nm = parse_declarator_name(p, &t, base);
            ASTNode *var = new_node(p, NODE_VAR_DECL);
            var->var_decl.name      = nm ? nm : (char*)"";
            var->var_decl.decl_type = t;
            if (match_tok(p, TOK_EQ))
                var->var_decl.init = parse_expr_prec(p, 0);
            match_tok(p, TOK_SEMI);
            return var;
        }
    }

    return parse_stmt_inner(p);
}

/* ── Statement parser ───────────────────────────────────────── */
static ASTNode* parse_stmt_inner(Parser *p) {
    Token t = p->current;

    /* { block } */
    if (t.kind == TOK_LBRACE) return parse_block(p);

    /* return */
    if (t.kind == TOK_KW_RETURN) {
        advance(p);
        ASTNode *ret = new_node_at(p, NODE_RETURN_STMT, t);
        if (!check(p, TOK_SEMI))
            ret->ret.value = parse_expr_prec(p, 0);
        match_tok(p, TOK_SEMI);
        return ret;
    }

    /* if */
    if (t.kind == TOK_KW_IF) {
        advance(p);
        ASTNode *ifs = new_node_at(p, NODE_IF_STMT, t);
        expect(p, TOK_LPAREN, "(");
        ifs->if_stmt.cond    = parse_expr_prec(p, 0);
        expect(p, TOK_RPAREN, ")");
        ifs->if_stmt.then_br = parse_stmt_inner(p);
        if (match_tok(p, TOK_KW_ELSE))
            ifs->if_stmt.else_br = parse_stmt_inner(p);
        return ifs;
    }

    /* while */
    if (t.kind == TOK_KW_WHILE) {
        advance(p);
        ASTNode *ws = new_node_at(p, NODE_WHILE_STMT, t);
        expect(p, TOK_LPAREN, "(");
        ws->while_stmt.cond = parse_expr_prec(p, 0);
        expect(p, TOK_RPAREN, ")");
        ws->while_stmt.body = parse_stmt_inner(p);
        return ws;
    }

    /* for */
    if (t.kind == TOK_KW_FOR) {
        advance(p);
        ASTNode *fs = new_node_at(p, NODE_FOR_STMT, t);
        expect(p, TOK_LPAREN, "(");
        if (!check(p, TOK_SEMI))
            fs->for_stmt.init = parse_decl_or_stmt(p);
        else
            match_tok(p, TOK_SEMI);
        if (!check(p, TOK_SEMI))
            fs->for_stmt.cond = parse_expr_prec(p, 0);
        match_tok(p, TOK_SEMI);
        if (!check(p, TOK_RPAREN))
            fs->for_stmt.step = parse_expr_prec(p, 0);
        expect(p, TOK_RPAREN, ")");
        fs->for_stmt.body = parse_stmt_inner(p);
        return fs;
    }

    /* do-while */
    if (t.kind == TOK_KW_DO) {
        advance(p);
        ASTNode *dw = new_node_at(p, NODE_DO_WHILE_STMT, t);
        dw->do_while.body = parse_stmt_inner(p);
        expect(p, TOK_KW_WHILE, "while");
        expect(p, TOK_LPAREN,   "(");
        dw->do_while.cond = parse_expr_prec(p, 0);
        expect(p, TOK_RPAREN, ")");
        match_tok(p, TOK_SEMI);
        return dw;
    }

    /* break */
    if (t.kind == TOK_KW_BREAK) {
        advance(p); match_tok(p, TOK_SEMI);
        return new_node_at(p, NODE_BREAK_STMT, t);
    }

    /* continue */
    if (t.kind == TOK_KW_CONTINUE) {
        advance(p); match_tok(p, TOK_SEMI);
        return new_node_at(p, NODE_CONTINUE_STMT, t);
    }

    /* semicolon (empty statement) */
    if (t.kind == TOK_SEMI) {
        advance(p);
        return new_node_at(p, NODE_EXPR_STMT, t);
    }

    /* expression statement */
    ASTNode *es = new_node_at(p, NODE_EXPR_STMT, t);
    es->expr_stmt.expr = parse_expr_prec(p, 0);
    match_tok(p, TOK_SEMI);
    return es;
}

/* ── Operator precedence table ──────────────────────────────── */
typedef struct { int prec; bool right_assoc; } OpInfo;

static OpInfo binary_op_info(TokenKind k) {
    switch (k) {
        case TOK_STAR:     case TOK_SLASH:    case TOK_PERCENT:   return (OpInfo){12, false};
        case TOK_PLUS:     case TOK_MINUS:                        return (OpInfo){11, false};
        case TOK_LSHIFT:   case TOK_RSHIFT:                       return (OpInfo){10, false};
        case TOK_LT:       case TOK_LT_EQ:
        case TOK_GT:       case TOK_GT_EQ:                        return (OpInfo){9,  false};
        case TOK_EQ_EQ:    case TOK_BANG_EQ:                      return (OpInfo){8,  false};
        case TOK_AMP:                                              return (OpInfo){7,  false};
        case TOK_CARET:                                            return (OpInfo){6,  false};
        case TOK_PIPE:                                             return (OpInfo){5,  false};
        case TOK_AMP_AMP:                                         return (OpInfo){4,  false};
        case TOK_PIPE_PIPE:                                        return (OpInfo){3,  false};
        /* Assignment */
        case TOK_EQ:       case TOK_PLUS_EQ:  case TOK_MINUS_EQ:
        case TOK_STAR_EQ:  case TOK_SLASH_EQ: case TOK_PERCENT_EQ:
        case TOK_AMP_EQ:   case TOK_PIPE_EQ:  case TOK_CARET_EQ:  return (OpInfo){1,  true};
        default:                                                   return (OpInfo){0,  false};
    }
}

static bool is_assign_op(TokenKind k) {
    return k == TOK_EQ       || k == TOK_PLUS_EQ  || k == TOK_MINUS_EQ ||
           k == TOK_STAR_EQ  || k == TOK_SLASH_EQ || k == TOK_PERCENT_EQ ||
           k == TOK_AMP_EQ   || k == TOK_PIPE_EQ  || k == TOK_CARET_EQ;
}

/* ── Expression parser (Pratt / precedence climbing) ────────── */
ASTNode* parse_expression(Parser *p) { return parse_expr_prec(p, 0); }

static ASTNode* parse_expr_prec(Parser *p, int min_prec) {
    ASTNode *left = parse_unary(p);
    if (!left) return NULL;

    for (;;) {
        TokenKind k = p->current.kind;

        /* Ternary: cond ? then : else  (prec = 2) */
        if (k == TOK_QUESTION && min_prec <= 2) {
            Token qt = advance(p);
            ASTNode *tern = new_node_at(p, NODE_TERNARY_EXPR, qt);
            tern->ternary.cond      = left;
            tern->ternary.then_expr = parse_expr_prec(p, 0);
            expect(p, TOK_COLON, ":");
            tern->ternary.else_expr = parse_expr_prec(p, 2);
            left = tern;
            continue;
        }

        OpInfo oi = binary_op_info(k);
        if (oi.prec <= min_prec) break;

        Token op_tok = advance(p);
        int next_prec = oi.right_assoc ? oi.prec - 1 : oi.prec;

        ASTNode *right = parse_expr_prec(p, next_prec);
        if (!right) break;

        if (is_assign_op(k)) {
            ASTNode *asgn = new_node_at(p, NODE_ASSIGN_EXPR, op_tok);
            asgn->assign.op  = (int)k;
            asgn->assign.lhs = left;
            asgn->assign.rhs = right;
            left = asgn;
        } else {
            ASTNode *bin = new_node_at(p, NODE_BINARY_EXPR, op_tok);
            bin->binary.op    = (int)k;
            bin->binary.left  = left;
            bin->binary.right = right;
            left = bin;
        }
    }
    return left;
}

/* ── Unary expression ───────────────────────────────────────── */
static ASTNode* parse_unary(Parser *p) {
    Token t = p->current;

    switch (t.kind) {
        case TOK_BANG: {
            advance(p);
            ASTNode *u = new_node_at(p, NODE_UNARY_EXPR, t);
            u->unary.op      = (int)TOK_BANG;
            u->unary.operand = parse_unary(p);
            return u;
        }
        case TOK_MINUS: {
            advance(p);
            ASTNode *u = new_node_at(p, NODE_UNARY_EXPR, t);
            u->unary.op      = (int)TOK_MINUS;
            u->unary.operand = parse_unary(p);
            return u;
        }
        case TOK_PLUS: {
            advance(p);
            return parse_unary(p); /* unary + is no-op */
        }
        case TOK_TILDE: {
            advance(p);
            ASTNode *u = new_node_at(p, NODE_UNARY_EXPR, t);
            u->unary.op      = (int)TOK_TILDE;
            u->unary.operand = parse_unary(p);
            return u;
        }
        case TOK_AMP: {
            advance(p);
            ASTNode *u = new_node_at(p, NODE_ADDR_EXPR, t);
            u->unary.op      = (int)TOK_AMP;
            u->unary.operand = parse_unary(p);
            return u;
        }
        case TOK_STAR: {
            advance(p);
            ASTNode *u = new_node_at(p, NODE_DEREF_EXPR, t);
            u->unary.op      = (int)TOK_STAR;
            u->unary.operand = parse_unary(p);
            return u;
        }
        case TOK_PLUS_PLUS: {
            advance(p);
            ASTNode *u = new_node_at(p, NODE_UNARY_EXPR, t);
            u->unary.op      = (int)TOK_PLUS_PLUS;
            u->unary.operand = parse_unary(p);
            u->unary.postfix = false;
            return u;
        }
        case TOK_MINUS_MINUS: {
            advance(p);
            ASTNode *u = new_node_at(p, NODE_UNARY_EXPR, t);
            u->unary.op      = (int)TOK_MINUS_MINUS;
            u->unary.operand = parse_unary(p);
            u->unary.postfix = false;
            return u;
        }
        case TOK_KW_SIZEOF: {
            advance(p);
            ASTNode *sz = new_node_at(p, NODE_SIZEOF_EXPR, t);
            if (check(p, TOK_LPAREN)) {
                advance(p);
                if (tok_is_type_start(p->current.kind)) {
                    sz->szof.of_type = parse_type_specifier(p);
                    sz->szof.is_type = true;
                } else {
                    sz->szof.expr    = parse_expr_prec(p, 0);
                    sz->szof.is_type = false;
                }
                expect(p, TOK_RPAREN, ")");
            } else {
                sz->szof.expr    = parse_unary(p);
                sz->szof.is_type = false;
            }
            return sz;
        }
        /* Cast: (type)expr */
        case TOK_LPAREN: {
            /* Lookahead to decide if it's a cast or grouped expr */
            if (tok_is_type_start(peek2(p).kind)) {
                advance(p); /* consume '(' */
                Type *base = parse_type_specifier(p);
                Type *to;
                /* Consume pointer stars and optional name */
                while (check(p, TOK_STAR)) {
                    advance(p);
                    Type *ptr = new_type(p, TY_PTR);
                    ptr->base = base;
                    base = ptr;
                }
                to = base;
                expect(p, TOK_RPAREN, ")");

                /* Compound literal: (Type){...} (C11 §6.5.2.5) */
                if (check(p, TOK_LBRACE)) {
                    advance(p);
                    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                        if (check(p, TOK_DOT)) {
                            advance(p);
                            if (check(p, TOK_IDENT)) advance(p);
                            match_tok(p, TOK_EQ);
                        } else if (check(p, TOK_LBRACKET)) {
                            advance(p); parse_expr_prec(p, 0);
                            match_tok(p, TOK_RBRACKET); match_tok(p, TOK_EQ);
                        }
                        parse_expr_prec(p, 0);
                        match_tok(p, TOK_COMMA);
                    }
                    match_tok(p, TOK_RBRACE);
                    ASTNode *cl = new_node_at(p, NODE_CAST_EXPR, t);
                    cl->cast.to   = to;
                    cl->cast.expr = NULL;
                    return cl;
                }
                ASTNode *cast_node = new_node_at(p, NODE_CAST_EXPR, t);
                cast_node->cast.to   = to;
                cast_node->cast.expr = parse_unary(p);
                return cast_node;
            }
            break;
        }
        default: break;
    }

    return parse_postfix(p, parse_primary(p));
}

/* ── Postfix: call, index, member, ++/-- ────────────────────── */
static ASTNode* parse_postfix(Parser *p, ASTNode *base) {
    if (!base) return NULL;
    for (;;) {
        Token t = p->current;
        switch (t.kind) {
            case TOK_LPAREN: {
                /* Function call */
                advance(p);
                ASTNode *call_node = new_node_at(p, NODE_CALL_EXPR, t);
                call_node->call.callee = base;
                call_node->call.cap    = 8;
                call_node->call.args   = malloc(8 * sizeof(ASTNode *));
                call_node->call.n_args = 0;
                while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                    ASTNode *arg = parse_expr_prec(p, 0);
                    if (call_node->call.n_args >= call_node->call.cap) {
                        call_node->call.cap *= 2;
                        ASTNode **tmp = realloc(call_node->call.args,
                            call_node->call.cap * sizeof(ASTNode *));
                        if (tmp) call_node->call.args = tmp;
                    }
                    call_node->call.args[call_node->call.n_args++] = arg;
                    if (!match_tok(p, TOK_COMMA)) break;
                }
                expect(p, TOK_RPAREN, ")");
                base = call_node;
                break;
            }
            case TOK_LBRACKET: {
                /* Array index */
                advance(p);
                ASTNode *idx_node = new_node_at(p, NODE_INDEX_EXPR, t);
                idx_node->idx.array = base;
                idx_node->idx.index = parse_expr_prec(p, 0);
                expect(p, TOK_RBRACKET, "]");
                base = idx_node;
                break;
            }
            case TOK_DOT:
            case TOK_ARROW: {
                bool arrow = (t.kind == TOK_ARROW);
                advance(p);
                ASTNode *mem = new_node_at(p, NODE_MEMBER_EXPR, t);
                mem->member.object = base;
                mem->member.arrow  = arrow;
                Token field_tok = expect(p, TOK_IDENT, "field name");
                mem->member.field  = tok_intern(p, field_tok);
                base = mem;
                break;
            }
            case TOK_PLUS_PLUS: {
                advance(p);
                ASTNode *u = new_node_at(p, NODE_UNARY_EXPR, t);
                u->unary.op      = (int)TOK_PLUS_PLUS;
                u->unary.operand = base;
                u->unary.postfix = true;
                base = u;
                break;
            }
            case TOK_MINUS_MINUS: {
                advance(p);
                ASTNode *u = new_node_at(p, NODE_UNARY_EXPR, t);
                u->unary.op      = (int)TOK_MINUS_MINUS;
                u->unary.operand = base;
                u->unary.postfix = true;
                base = u;
                break;
            }
            default:
                return base;
        }
    }
}

/* ── Primary expression ─────────────────────────────────────── */
static ASTNode* parse_primary(Parser *p) {
    Token t = advance(p);
    switch (t.kind) {
        case TOK_INT_LIT: {
            ASTNode *n = new_node_at(p, NODE_INT_LIT, t);
            n->int_lit.value = t.int_val;
            return n;
        }
        case TOK_FLOAT_LIT: {
            ASTNode *n = new_node_at(p, NODE_FLOAT_LIT, t);
            n->float_lit.value = t.float_val;
            return n;
        }
        case TOK_STRING_LIT: {
            ASTNode *n = new_node_at(p, NODE_STRING_LIT, t);
            n->str_lit.value = ast_intern(p->arena, t.start, t.len);
            n->str_lit.len   = t.len;
            return n;
        }
        case TOK_CHAR_LIT: {
            ASTNode *n = new_node_at(p, NODE_CHAR_LIT, t);
            n->int_lit.value = t.int_val;
            return n;
        }
        case TOK_KW_TRUE: {
            ASTNode *n = new_node_at(p, NODE_INT_LIT, t);
            n->int_lit.value = 1;
            return n;
        }
        case TOK_KW_FALSE:
        case TOK_KW_NULL: {
            ASTNode *n = new_node_at(p, NODE_INT_LIT, t);
            n->int_lit.value = 0;
            return n;
        }
        case TOK_IDENT: {
            ASTNode *n = new_node_at(p, NODE_IDENT_EXPR, t);
            n->ident.name = tok_intern(p, t);
            /* Might be followed by postfix — handled by caller */
            return n;
        }
        case TOK_LPAREN: {
            ASTNode *inner = parse_expr_prec(p, 0);
            expect(p, TOK_RPAREN, ")");
            return inner;
        }
        default: {
            if (!p->panic_mode) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "Expresión inesperada: '%.*s'",
                         (int)t.len, t.start);
                riglog_add(p->log, ERR_SYNTAX, p->file,
                           t.line, t.col, buf, "", "", "");
                p->error_count++;
                p->panic_mode = true;
            }
            /* Return dummy node to avoid NULL propagation */
            ASTNode *n = ast_node_new(p->arena, NODE_INT_LIT, t.line, t.col);
            return n;
        }
    }
}

/* ── Public sub-parsers ─────────────────────────────────────── */
ASTNode* parse_declaration(Parser *p) { return parse_func_or_var_decl(p); }
ASTNode* parse_statement  (Parser *p) { return parse_stmt_inner(p); }
