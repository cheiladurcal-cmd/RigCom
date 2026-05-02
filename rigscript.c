#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — src/rigscript.c
   RigScript: Lexer + Parser + Compiler → RigIR SSA
   Sintaxis moderna (TypeScript/Swift), rendimiento de C.
   Auto-JNI: funciones @export generan .java + C wrappers.
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/rigscript.h"
#include "../include/rigir.h"
#include "../include/error.h"
#include "../include/frontend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ══════════════════════════════════════════════════════════
   LEXER
   ══════════════════════════════════════════════════════════ */
static bool rs_is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_' || c == '@';
}
static bool rs_is_ident(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

void rslexer_init(RSLexer *lx, const char *src, size_t len) {
    lx->src = src; lx->src_len = len; lx->pos = 0; lx->line = 1;
    memset(&lx->cur, 0, sizeof(lx->cur));
}

static RSToken rs_make(RSTokenKind k, const char *s, uint32_t l, uint32_t line) {
    RSToken t; memset(&t, 0, sizeof(t));
    t.kind = k; t.start = s; t.len = l; t.line = line;
    return t;
}

static struct { const char *kw; RSTokenKind k; } rs_keywords[] = {
    {"let",    RS_TOK_LET},    {"const",  RS_TOK_CONST},
    {"fn",     RS_TOK_FN},     {"return", RS_TOK_RETURN},
    {"if",     RS_TOK_IF},     {"else",   RS_TOK_ELSE},
    {"while",  RS_TOK_WHILE},  {"for",    RS_TOK_FOR},
    {"in",     RS_TOK_IN},     {"struct", RS_TOK_STRUCT},
    {"import", RS_TOK_IMPORT}, {"export", RS_TOK_EXPORT},
    {"async",  RS_TOK_ASYNC},  {"await",  RS_TOK_AWAIT},
    {"type",   RS_TOK_TYPE},   {"true",   RS_TOK_BOOL_LIT},
    {"false",  RS_TOK_BOOL_LIT},
    {"i8",     RS_TOK_I8},     {"i16",    RS_TOK_I16},
    {"i32",    RS_TOK_I32},    {"i64",    RS_TOK_I64},
    {"u8",     RS_TOK_U8},     {"u16",    RS_TOK_U16},
    {"u32",    RS_TOK_U32},    {"u64",    RS_TOK_U64},
    {"f32",    RS_TOK_F32},    {"f64",    RS_TOK_F64},
    {"bool",   RS_TOK_BOOL_TY},{"str",    RS_TOK_STR_TY},
    {"void",   RS_TOK_VOID},
    {NULL, 0}
};

RSToken rslexer_next(RSLexer *lx) {
    const char *s = lx->src;
    size_t      n = lx->src_len;

    /* Skip whitespace + line comments */
    while (lx->pos < n) {
        char c = s[lx->pos];
        if (c == '\n') { lx->line++; lx->pos++; }
        else if (isspace((unsigned char)c)) { lx->pos++; }
        else if (c == '/' && lx->pos+1 < n && s[lx->pos+1] == '/') {
            while (lx->pos < n && s[lx->pos] != '\n') lx->pos++;
        } else if (c == '/' && lx->pos+1 < n && s[lx->pos+1] == '*') {
            lx->pos += 2;
            while (lx->pos+1 < n &&
                   !(s[lx->pos]=='*' && s[lx->pos+1]=='/')) {
                if (s[lx->pos] == '\n') lx->line++;
                lx->pos++;
            }
            lx->pos += 2;
        } else break;
    }

    if (lx->pos >= n) return rs_make(RS_TOK_EOF, s+lx->pos, 0, lx->line);

    uint32_t line = lx->line;
    size_t   p    = lx->pos;
    char     c    = s[p];

    /* Identifiers / keywords */
    if (rs_is_ident_start(c)) {
        /* @export annotation */
        bool at = (c == '@');
        if (at) { p++; }
        size_t start = p; (void)start;
        while (p < n && rs_is_ident(s[p])) p++;
        uint32_t len = (uint32_t)(p - (at ? lx->pos+1 : lx->pos));
        const char *id_start = s + (at ? lx->pos+1 : lx->pos);

        /* Check keywords */
        RSTokenKind kk = RS_TOK_IDENT;
        for (int ki = 0; rs_keywords[ki].kw; ki++) {
            size_t kwl = strlen(rs_keywords[ki].kw);
            if ((size_t)len == kwl &&
                strncmp(id_start, rs_keywords[ki].kw, kwl) == 0) {
                kk = rs_keywords[ki].k; break;
            }
        }
        if (at && kk == RS_TOK_EXPORT) {
            RSToken t = rs_make(RS_TOK_EXPORT, s+lx->pos, (uint32_t)(p-lx->pos), line);
            lx->pos = p;
            return t;
        }
        RSToken t = rs_make(kk, id_start, len, line);
        if (kk == RS_TOK_BOOL_LIT)
            t.ival = (strncmp(id_start, "true", 4) == 0) ? 1 : 0;
        lx->pos = p;
        return t;
    }

    /* Integer / float literal */
    if (isdigit((unsigned char)c)) {
        size_t start = p; (void)start;
        bool is_float = false;
        while (p < n && isdigit((unsigned char)s[p])) p++;
        if (p < n && s[p] == '.') { is_float = true; p++; while (p < n && isdigit((unsigned char)s[p])) p++; }
        RSToken t = rs_make(is_float ? RS_TOK_FLOAT_LIT : RS_TOK_INT_LIT,
                            s+start, (uint32_t)(p-start), line);
        if (is_float) t.fval = strtod(s+start, NULL);
        else          t.ival = strtoll(s+start, NULL, 10);
        lx->pos = p;
        return t;
    }

    /* String literal */
    if (c == '"') {
        p++;
        size_t start = p; (void)start;
        while (p < n && s[p] != '"') {
            if (s[p] == '\\') p++;
            if (s[p] == '\n') lx->line++;
            p++;
        }
        RSToken t = rs_make(RS_TOK_STR_LIT, s+start, (uint32_t)(p-start), line);
        if (p < n) p++; /* skip closing " */
        lx->pos = p;
        return t;
    }

    /* Two-char operators */
    lx->pos = p+1;
    if (p+1 < n) {
        char c2 = s[p+1];
        if (c=='-' && c2=='>') { lx->pos=p+2; return rs_make(RS_TOK_ARROW,     s+p,2,line); }
        if (c=='=' && c2=='>') { lx->pos=p+2; return rs_make(RS_TOK_FATARROW,  s+p,2,line); }
        if (c=='=' && c2=='=') { lx->pos=p+2; return rs_make(RS_TOK_EQEQ,      s+p,2,line); }
        if (c=='!' && c2=='=') { lx->pos=p+2; return rs_make(RS_TOK_BANGEQ,    s+p,2,line); }
        if (c=='<' && c2=='=') { lx->pos=p+2; return rs_make(RS_TOK_LTEQ,      s+p,2,line); }
        if (c=='>' && c2=='=') { lx->pos=p+2; return rs_make(RS_TOK_GTEQ,      s+p,2,line); }
        if (c=='&' && c2=='&') { lx->pos=p+2; return rs_make(RS_TOK_AMPAMP,    s+p,2,line); }
        if (c=='|' && c2=='|') { lx->pos=p+2; return rs_make(RS_TOK_PIPEPIPE,  s+p,2,line); }
        if (c=='.' && c2=='.') { lx->pos=p+2; return rs_make(RS_TOK_DOTDOT,    s+p,2,line); }
    }

    /* Single-char operators */
    static const char singles[] = "+-*/%&|^!~=<>(){}[]:;,.#";
    static const RSTokenKind sk[] = {
        RS_TOK_PLUS,RS_TOK_MINUS,RS_TOK_STAR,RS_TOK_SLASH,RS_TOK_PERCENT,
        RS_TOK_AMP,RS_TOK_PIPE,RS_TOK_CARET,RS_TOK_BANG,RS_TOK_TILDE,
        RS_TOK_EQ,RS_TOK_LT,RS_TOK_GT,
        RS_TOK_LPAREN,RS_TOK_RPAREN,RS_TOK_LBRACE,RS_TOK_RBRACE,
        RS_TOK_LBRACKET,RS_TOK_RBRACKET,
        RS_TOK_COLON,RS_TOK_SEMICOLON,RS_TOK_COMMA,RS_TOK_DOT,RS_TOK_HASH
    };
    const char *pos = strchr(singles, c);
    if (pos) return rs_make(sk[pos - singles], s+p, 1, line);

    return rs_make(RS_TOK_UNKNOWN, s+p, 1, line);
}

RSToken rslexer_peek(RSLexer *lx) {
    size_t save_pos = lx->pos;
    uint32_t save_line = lx->line;
    RSToken t = rslexer_next(lx);
    lx->pos = save_pos;
    lx->line = save_line;
    return t;
}

/* ══════════════════════════════════════════════════════════
   PARSER — Recursive Descent
   ══════════════════════════════════════════════════════════ */

/* Pool de nodos */
static RSNode* rs_node_new(RSParser *p, RSNodeKind k) {
    RSNode *n = calloc(1, sizeof(RSNode));
    if (!n) return NULL;
    n->kind = k;
    n->line = p->cur.line;
    return n;
}

static RSToken rs_advance(RSParser *p) {
    p->cur = rslexer_next(&p->lx);
    return p->cur;
}

static bool rs_check(RSParser *p, RSTokenKind k) {
    return p->cur.kind == k;
}

static bool rs_eat(RSParser *p, RSTokenKind k) {
    if (p->cur.kind == k) { rs_advance(p); return true; }
    return false;
}

static RSTypeKind rs_parse_type(RSParser *p) {
    switch (p->cur.kind) {
    case RS_TOK_I8:  rs_advance(p); return RS_TY_I8;
    case RS_TOK_I16: rs_advance(p); return RS_TY_I16;
    case RS_TOK_I32: rs_advance(p); return RS_TY_I32;
    case RS_TOK_I64: rs_advance(p); return RS_TY_I64;
    case RS_TOK_U8:  rs_advance(p); return RS_TY_U8;
    case RS_TOK_U16: rs_advance(p); return RS_TY_U16;
    case RS_TOK_U32: rs_advance(p); return RS_TY_U32;
    case RS_TOK_U64: rs_advance(p); return RS_TY_U64;
    case RS_TOK_F32: rs_advance(p); return RS_TY_F32;
    case RS_TOK_F64: rs_advance(p); return RS_TY_F64;
    case RS_TOK_BOOL_TY: rs_advance(p); return RS_TY_BOOL;
    case RS_TOK_STR_TY:  rs_advance(p); return RS_TY_STR;
    case RS_TOK_VOID:    rs_advance(p); return RS_TY_VOID;
    default: return RS_TY_I64; /* default */
    }
}

static RSNode* rs_parse_expr(RSParser *p);
static RSNode* rs_parse_stmt(RSParser *p);
static RSNode* rs_parse_block(RSParser *p);

static RSNode* rs_parse_primary(RSParser *p) {
    RSNode *n;
    switch (p->cur.kind) {
    case RS_TOK_INT_LIT:
        n = rs_node_new(p, RS_NODE_INT_LIT);
        if(n){ n->ival = p->cur.ival; n->type = RS_TY_I64; }
        rs_advance(p); return n;
    case RS_TOK_FLOAT_LIT:
        n = rs_node_new(p, RS_NODE_FLOAT_LIT);
        if(n){ n->fval = p->cur.fval; n->type = RS_TY_F64; }
        rs_advance(p); return n;
    case RS_TOK_STR_LIT:
        n = rs_node_new(p, RS_NODE_STR_LIT);
        if(n){ strncpy(n->name, p->cur.start,
                       p->cur.len < 127 ? p->cur.len : 127); n->type = RS_TY_STR; }
        rs_advance(p); return n;
    case RS_TOK_BOOL_LIT:
        n = rs_node_new(p, RS_NODE_BOOL_LIT);
        if(n){ n->ival = p->cur.ival; n->type = RS_TY_BOOL; }
        rs_advance(p); return n;
    case RS_TOK_IDENT: {
        n = rs_node_new(p, RS_NODE_IDENT);
        if(n) strncpy(n->name, p->cur.start,
                      p->cur.len < 127 ? p->cur.len : 127);
        rs_advance(p);
        /* Call? */
        if (rs_check(p, RS_TOK_LPAREN)) {
            rs_advance(p);
            RSNode *call = rs_node_new(p, RS_NODE_CALL);
            if (call) { strncpy(call->name, n->name, 127); free(n); n = call; }
            RSNode *first = NULL, **cur_arg = &first;
            uint32_t na = 0;
            while (!rs_check(p, RS_TOK_RPAREN) && !rs_check(p, RS_TOK_EOF)) {
                RSNode *arg = rs_parse_expr(p);
                *cur_arg = arg; if(arg) cur_arg = &arg->next;
                na++;
                if (!rs_eat(p, RS_TOK_COMMA)) break;
            }
            rs_eat(p, RS_TOK_RPAREN);
            if (n) { n->args = first; n->n_args = na; }
        }
        return n;
    }
    case RS_TOK_LPAREN:
        rs_advance(p);
        n = rs_parse_expr(p);
        rs_eat(p, RS_TOK_RPAREN);
        return n;
    default:
        n = rs_node_new(p, RS_NODE_INT_LIT);
        rs_advance(p); return n;
    }
}

static RSNode* rs_parse_unary(RSParser *p) {
    if (rs_check(p, RS_TOK_MINUS) || rs_check(p, RS_TOK_BANG)) {
        RSToken op = p->cur; rs_advance(p);
        RSNode *n = rs_node_new(p, RS_NODE_UNARY);
        if(n){ n->op = op.kind; n->left = rs_parse_unary(p); }
        return n;
    }
    return rs_parse_primary(p);
}

static RSNode* rs_parse_binary_prec(RSParser *p, int min_prec) {
    RSNode *lhs = rs_parse_unary(p);
    while (true) {
        int prec = 0;
        RSTokenKind op = p->cur.kind;
        switch(op) {
        case RS_TOK_PIPEPIPE: prec=1; break;
        case RS_TOK_AMPAMP:   prec=2; break;
        case RS_TOK_EQEQ: case RS_TOK_BANGEQ: prec=3; break;
        case RS_TOK_LT: case RS_TOK_GT:
        case RS_TOK_LTEQ: case RS_TOK_GTEQ: prec=4; break;
        case RS_TOK_PLUS: case RS_TOK_MINUS: prec=5; break;
        case RS_TOK_STAR: case RS_TOK_SLASH:
        case RS_TOK_PERCENT: prec=6; break;
        default: prec=0; break;
        }
        if (prec < min_prec) break;
        rs_advance(p);
        RSNode *rhs = rs_parse_binary_prec(p, prec+1);
        RSNode *bin = rs_node_new(p, RS_NODE_BINARY);
        if(bin){ bin->op = op; bin->left = lhs; bin->right = rhs; }
        lhs = bin;
    }
    return lhs;
}

static RSNode* rs_parse_expr(RSParser *p) {
    RSNode *lhs = rs_parse_binary_prec(p, 1);
    if (rs_check(p, RS_TOK_EQ)) {
        rs_advance(p);
        RSNode *rhs = rs_parse_expr(p);
        RSNode *a = rs_node_new(p, RS_NODE_ASSIGN);
        if(a){ a->left = lhs; a->right = rhs; }
        return a;
    }
    return lhs;
}

static RSNode* rs_parse_stmt(RSParser *p) {
    RSNode *n;

    /* let x: T = expr; */
    if (rs_check(p, RS_TOK_LET) || rs_check(p, RS_TOK_CONST)) {
        bool is_const = rs_check(p, RS_TOK_CONST);
        rs_advance(p);
        n = rs_node_new(p, RS_NODE_LET);
        if(n){
            if (rs_check(p, RS_TOK_IDENT))
                strncpy(n->name, p->cur.start,
                        p->cur.len < 127 ? p->cur.len : 127);
            rs_advance(p);
            n->decl_type = RS_TY_I64;
            if (rs_eat(p, RS_TOK_COLON)) n->decl_type = rs_parse_type(p);
            if (rs_eat(p, RS_TOK_EQ))    n->right = rs_parse_expr(p);
            n->type = n->decl_type;
            (void)is_const;
        }
        rs_eat(p, RS_TOK_SEMICOLON);
        return n;
    }

    /* return expr; */
    if (rs_check(p, RS_TOK_RETURN)) {
        rs_advance(p);
        n = rs_node_new(p, RS_NODE_RETURN);
        if(n && !rs_check(p, RS_TOK_SEMICOLON))
            n->left = rs_parse_expr(p);
        rs_eat(p, RS_TOK_SEMICOLON);
        return n;
    }

    /* if cond { } else { } */
    if (rs_check(p, RS_TOK_IF)) {
        rs_advance(p);
        n = rs_node_new(p, RS_NODE_IF);
        if(n){
            rs_eat(p, RS_TOK_LPAREN);
            n->cond = rs_parse_expr(p);
            rs_eat(p, RS_TOK_RPAREN);
            n->body = rs_parse_block(p);
            if (rs_eat(p, RS_TOK_ELSE))
                n->else_br = rs_check(p, RS_TOK_IF) ? rs_parse_stmt(p) : rs_parse_block(p);
        }
        return n;
    }

    /* while cond { } */
    if (rs_check(p, RS_TOK_WHILE)) {
        rs_advance(p);
        n = rs_node_new(p, RS_NODE_WHILE);
        if(n){
            rs_eat(p, RS_TOK_LPAREN);
            n->cond = rs_parse_expr(p);
            rs_eat(p, RS_TOK_RPAREN);
            n->body = rs_parse_block(p);
        }
        return n;
    }

    /* Block */
    if (rs_check(p, RS_TOK_LBRACE))
        return rs_parse_block(p);

    /* Expression statement */
    n = rs_parse_expr(p);
    rs_eat(p, RS_TOK_SEMICOLON);
    return n;
}

static RSNode* rs_parse_block(RSParser *p) {
    rs_eat(p, RS_TOK_LBRACE);
    RSNode *block = rs_node_new(p, RS_NODE_BLOCK);
    RSNode *first = NULL, **cur = &first;
    while (!rs_check(p, RS_TOK_RBRACE) && !rs_check(p, RS_TOK_EOF)) {
        RSNode *s = rs_parse_stmt(p);
        *cur = s; if(s) cur = &s->next;
    }
    rs_eat(p, RS_TOK_RBRACE);
    if(block) block->body = first;
    return block;
}

void rsparser_init(RSParser *p, const char *src, size_t len,
                   RigErrorLog *log, const char *filename) {
    rslexer_init(&p->lx, src, len);
    p->log = log; p->filename = filename;
    p->error_count = 0; p->has_peek = false;
    rs_advance(p); /* prime the first token */
}

RSNode* rsparser_parse(RSParser *p) {
    RSNode *mod = rs_node_new(p, RS_NODE_MODULE);
    RSNode *first = NULL, **cur_decl = &first;

    while (!rs_check(p, RS_TOK_EOF)) {
        bool exported = false;

        /* @export annotation */
        if (rs_check(p, RS_TOK_EXPORT)) {
            exported = true;
            rs_advance(p);
        }

        /* fn name(params) -> RetType { body } */
        if (rs_check(p, RS_TOK_FN)) {
            rs_advance(p);
            RSNode *fn = rs_node_new(p, RS_NODE_FN_DEF);
            if (!fn) break;
            fn->exported = exported;

            if (rs_check(p, RS_TOK_IDENT)) {
                strncpy(fn->name, p->cur.start,
                        p->cur.len < 127 ? p->cur.len : 127);
                rs_advance(p);
            }

            /* Parameters */
            rs_eat(p, RS_TOK_LPAREN);
            RSNode *fp = NULL, **fpc = &fp;
            while (!rs_check(p, RS_TOK_RPAREN) && !rs_check(p, RS_TOK_EOF)) {
                RSNode *pm = rs_node_new(p, RS_NODE_FN_PARAM);
                if (pm) {
                    if (rs_check(p, RS_TOK_IDENT)) {
                        strncpy(pm->name, p->cur.start,
                                p->cur.len < 127 ? p->cur.len : 127);
                        rs_advance(p);
                    }
                    if (rs_eat(p, RS_TOK_COLON))
                        pm->decl_type = rs_parse_type(p);
                    *fpc = pm; fpc = &pm->next;
                }
                if (!rs_eat(p, RS_TOK_COMMA)) break;
            }
            rs_eat(p, RS_TOK_RPAREN);
            fn->params = fp;

            /* Return type: -> T */
            fn->ret_type = RS_TY_VOID;
            if (rs_eat(p, RS_TOK_ARROW))
                fn->ret_type = rs_parse_type(p);

            fn->body = rs_parse_block(p);
            *cur_decl = fn; cur_decl = &fn->next;
            continue;
        }

        /* struct Foo { ... } */
        if (rs_check(p, RS_TOK_STRUCT)) {
            rs_advance(p);
            RSNode *st = rs_node_new(p, RS_NODE_STRUCT_DEF);
            if (st && rs_check(p, RS_TOK_IDENT)) {
                strncpy(st->name, p->cur.start,
                        p->cur.len < 127 ? p->cur.len : 127);
                rs_advance(p);
            }
            /* skip body for now */
            if (rs_check(p, RS_TOK_LBRACE)) rs_parse_block(p);
            if (st) { *cur_decl = st; cur_decl = &st->next; }
            continue;
        }

        /* Top-level let/const */
        if (rs_check(p, RS_TOK_LET) || rs_check(p, RS_TOK_CONST)) {
            RSNode *s = rs_parse_stmt(p);
            if (s) { s->exported = exported; *cur_decl = s; cur_decl = &s->next; }
            continue;
        }

        /* Skip unknown top-level tokens */
        rs_advance(p);
    }

    if (mod) mod->body = first;
    return mod;
}

/* ══════════════════════════════════════════════════════════
   LOWERING: RSNode → RigIR SSA
   ══════════════════════════════════════════════════════════ */

static IRType rs_ty_to_irtype(RSTypeKind k) {
    switch (k) {
    case RS_TY_I8:  return IRTY_I8;
    case RS_TY_I16: return IRTY_I16;
    case RS_TY_I32: return IRTY_I32;
    case RS_TY_I64: return IRTY_I64;
    case RS_TY_U8:  return IRTY_U8;
    case RS_TY_U16: return IRTY_U16;
    case RS_TY_U32: return IRTY_U32;
    case RS_TY_U64: return IRTY_U64;
    case RS_TY_F32: return IRTY_F32;
    case RS_TY_F64: return IRTY_F64;
    case RS_TY_BOOL: return IRTY_I1;
    case RS_TY_STR:  return IRTY_PTR;
    default:         return IRTY_I64;
    }
}

static IROp rs_binop_to_irop(int op_tok) {
    switch ((RSTokenKind)op_tok) {
    case RS_TOK_PLUS:   return IR_ADD;
    case RS_TOK_MINUS:  return IR_SUB;
    case RS_TOK_STAR:   return IR_MUL;
    case RS_TOK_SLASH:  return IR_DIV;
    case RS_TOK_PERCENT:return IR_MOD;
    case RS_TOK_AMP:    return IR_AND;
    case RS_TOK_PIPE:   return IR_OR;
    case RS_TOK_CARET:  return IR_XOR;
    case RS_TOK_EQEQ:   return IR_CMP_EQ;
    case RS_TOK_BANGEQ: return IR_CMP_NE;
    case RS_TOK_LT:     return IR_CMP_LT;
    case RS_TOK_LTEQ:   return IR_CMP_LE;
    case RS_TOK_GT:     return IR_CMP_GT;
    case RS_TOK_GTEQ:   return IR_CMP_GE;
    default:             return IR_ADD;
    }
}

static IRVal rs_lower_expr(IRFunc *f, RSNode *n) {
    if (!n) return IRV_NONE;
    switch (n->kind) {
    case RS_NODE_INT_LIT:   return ir_emit_imm(f, n->ival, IRTY_I64);
    case RS_NODE_FLOAT_LIT: return ir_emit_fimm(f, n->fval);
    case RS_NODE_BOOL_LIT:  return ir_emit_imm(f, n->ival, IRTY_I1);
    case RS_NODE_STR_LIT:   return ir_emit_imm(f, 0, IRTY_PTR);
    case RS_NODE_IDENT:     return ir_emit_imm(f, 0, IRTY_I64);
    case RS_NODE_BINARY: {
        IRVal lhs = rs_lower_expr(f, n->left);
        IRVal rhs = rs_lower_expr(f, n->right);
        IROp  op  = rs_binop_to_irop(n->op);
        IRType ty = (lhs.type == IRTY_F32 || lhs.type == IRTY_F64) ? lhs.type : IRTY_I64;
        return ir_emit_bin(f, op, lhs, rhs, ty);
    }
    case RS_NODE_UNARY: {
        IRVal val = rs_lower_expr(f, n->left);
        if (n->op == RS_TOK_MINUS) return ir_emit_un(f, IR_NEG, val, IRTY_I64);
        if (n->op == RS_TOK_BANG)  return ir_emit_un(f, IR_NOT, val, IRTY_I1);
        return val;
    }
    case RS_NODE_CALL: {
        IRVal args[16]; uint32_t na = 0;
        RSNode *a = n->args;
        while (a && na < 16) { args[na++] = rs_lower_expr(f, a); a = a->next; }
        return ir_emit_call(f, n->name, args, na, IRTY_I64);
    }
    case RS_NODE_ASSIGN: {
        IRVal rhs = rs_lower_expr(f, n->right);
        return ir_emit_mov(f, rhs);
    }
    default: return IRV_NONE;
    }
}

static void rs_lower_block(IRFunc *f, RSNode *block);

static void rs_lower_stmt(IRFunc *f, RSNode *n) {
    if (!n) return;
    switch (n->kind) {
    case RS_NODE_LET: {
        IRType ty = rs_ty_to_irtype(n->decl_type);
        IRVal slot = ir_emit_alloca(f, ty, 1);
        if (n->right) ir_emit_store(f, rs_lower_expr(f, n->right), slot);
        break;
    }
    case RS_NODE_RETURN:
        if (n->left) ir_emit_ret(f, rs_lower_expr(f, n->left));
        else ir_emit_ret_void(f);
        break;
    case RS_NODE_IF: {
        static uint32_t id_ctr = 0; uint32_t id = ++id_ctr;
        char then_l[32], else_l[32], end_l[32];
        snprintf(then_l, 32, "rs_if_then_%u", id);
        snprintf(else_l, 32, "rs_if_else_%u", id);
        snprintf(end_l,  32, "rs_if_end_%u",  id);
        IRVal cond = rs_lower_expr(f, n->cond);
        ir_emit_cond_br(f, cond, then_l, n->else_br ? else_l : end_l);
        irfunc_set_block(f, irfunc_new_block(f, then_l));
        rs_lower_block(f, n->body);
        ir_emit_br(f, end_l);
        if (n->else_br) {
            irfunc_set_block(f, irfunc_new_block(f, else_l));
            rs_lower_stmt(f, n->else_br);
            ir_emit_br(f, end_l);
        }
        irfunc_set_block(f, irfunc_new_block(f, end_l));
        break;
    }
    case RS_NODE_WHILE: {
        static uint32_t wid = 0; uint32_t id = ++wid;
        char head[32], body[32], exit[32];
        snprintf(head, 32, "rs_wh_head_%u", id);
        snprintf(body, 32, "rs_wh_body_%u", id);
        snprintf(exit, 32, "rs_wh_exit_%u", id);
        ir_emit_br(f, head);
        irfunc_set_block(f, irfunc_new_block(f, head));
        ir_emit_cond_br(f, rs_lower_expr(f, n->cond), body, exit);
        irfunc_set_block(f, irfunc_new_block(f, body));
        rs_lower_block(f, n->body);
        ir_emit_br(f, head);
        irfunc_set_block(f, irfunc_new_block(f, exit));
        break;
    }
    case RS_NODE_BLOCK:
        rs_lower_block(f, n);
        break;
    default:
        rs_lower_expr(f, n);
        break;
    }
}

static void rs_lower_block(IRFunc *f, RSNode *block) {
    if (!block) return;
    RSNode *s = block->body;
    while (s) { rs_lower_stmt(f, s); s = s->next; }
}

static void rs_lower_fn(IRModule *m, RSNode *fn) {
    if (!fn || fn->kind != RS_NODE_FN_DEF) return;
    IRType ret = rs_ty_to_irtype(fn->ret_type);
    IRFunc *f = irmod_add_func(m, fn->name[0] ? fn->name : "rs_anon", ret);
    if (!f) return;

    /* Parameters */
    RSNode *pm = fn->params;
    uint32_t npar = 0;
    while (pm) { npar++; pm = pm->next; }
    if (npar > 0) {
        f->params   = malloc(npar * sizeof(IRVal));
        f->n_params = npar;
        pm = fn->params;
        for (uint32_t i = 0; pm && i < npar; i++, pm = pm->next)
            f->params[i] = ir_next_vreg(f, rs_ty_to_irtype(pm->decl_type));
    }

    irfunc_set_block(f, irfunc_new_block(f, "entry"));
    rs_lower_block(f, fn->body);

    IRBlock *cur = f->current;
    if (cur && (cur->n_instrs == 0 ||
        (cur->instrs[cur->n_instrs-1].op != IR_RET &&
         cur->instrs[cur->n_instrs-1].op != IR_BR  &&
         cur->instrs[cur->n_instrs-1].op != IR_COND_BR)))
        ir_emit_ret_void(f);
}

/* ══════════════════════════════════════════════════════════
   FRONTEND ENTRY POINTS
   ══════════════════════════════════════════════════════════ */

IRModule* rigscript_compile_to_ir(RigCtx *ctx, const char *src_path,
                                   RigErrorLog *log) {
    FILE *fp = fopen(src_path, "r");
    if (!fp) { riglog_add(log, ERR_FILE_NOT_FOUND, src_path, 0, 0,
                          "No se encontro el archivo RigScript", "", "", ""); return NULL; }
    fseek(fp, 0, SEEK_END); long fsz = ftell(fp); rewind(fp);
    char *src = malloc((size_t)fsz + 1);
    if (!src) { fclose(fp); return NULL; }
    size_t nr = fread(src, 1, (size_t)fsz, fp); src[nr] = '\0'; fclose(fp);
    (void)ctx;

    RSParser ps;
    rsparser_init(&ps, src, nr, log, src_path);
    RSNode *mod = rsparser_parse(&ps);

    IRModule *m = irmod_new();
    if (mod) {
        RSNode *decl = mod->body;
        while (decl) {
            if (decl->kind == RS_NODE_FN_DEF) rs_lower_fn(m, decl);
            decl = decl->next;
        }
    }

    free(src);
    return m;
}

char* rigscript_ast_json(RigCtx *ctx, const char *src_path, RigErrorLog *log) {
    FILE *fp = fopen(src_path, "r");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long fsz = ftell(fp); rewind(fp);
    char *src = malloc((size_t)fsz + 1);
    if (!src) { fclose(fp); return NULL; }
    size_t nr = fread(src, 1, (size_t)fsz, fp); src[nr] = '\0'; fclose(fp);
    (void)ctx; (void)log;

    RSParser ps; RigErrorLog tmp = {0};
    rsparser_init(&ps, src, nr, &tmp, src_path);
    RSNode *mod = rsparser_parse(&ps);

    size_t bufsz = 65536;
    char *buf = malloc(bufsz);
    if (!buf) { free(src); return NULL; }
    char *p = buf; size_t rem = bufsz; int n;
    n = snprintf(p, rem, "{\"file\":\"%s\",\"lang\":\"RigScript\",\"functions\":[", src_path);
    p+=n; rem-=(size_t)n;

    bool first = true;
    if (mod) {
        RSNode *d = mod->body;
        while (d && rem > 256) {
            if (d->kind == RS_NODE_FN_DEF) {
                int npar = 0; RSNode *pm = d->params;
                while (pm) { npar++; pm = pm->next; }
                n = snprintf(p, rem,
                    "%s{\"name\":\"%s\",\"line\":%u,\"exported\":%s,"
                    "\"n_params\":%d,\"ret_type\":\"%s\"}",
                    first ? "" : ",", d->name, d->line,
                    d->exported ? "true" : "false",
                    npar,
                    d->ret_type == RS_TY_VOID ? "void" :
                    d->ret_type == RS_TY_F64  ? "f64"  :
                    d->ret_type == RS_TY_I64  ? "i64"  : "other");
                p+=n; rem-=(size_t)n; first=false;
            }
            d = d->next;
        }
    }
    snprintf(p, rem, "]}");
    free(src);
    return buf;
}

char* rigscript_generate_header(RigCtx *ctx, const char *src_path, RigErrorLog *log) {
    FILE *fp = fopen(src_path, "r");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long fsz = ftell(fp); rewind(fp);
    char *src = malloc((size_t)fsz + 1);
    if (!src) { fclose(fp); return NULL; }
    size_t nr = fread(src, 1, (size_t)fsz, fp); src[nr] = '\0'; fclose(fp);
    (void)ctx; (void)log;

    RSParser ps; RigErrorLog tmp = {0};
    rsparser_init(&ps, src, nr, &tmp, src_path);
    RSNode *mod = rsparser_parse(&ps);

    size_t bufsz = 32768;
    char *buf = malloc(bufsz);
    if (!buf) { free(src); return NULL; }

    char guard[128];
    const char *base = strrchr(src_path, '/');
    base = base ? base+1 : src_path;
    snprintf(guard, sizeof(guard), "RIGSCRIPT_%s_H", base);
    for (char *g = guard; *g; g++) {
        if (*g == '.') *g = '_';
        *g = (char)toupper((unsigned char)*g);
    }

    int pos = snprintf(buf, bufsz,
        "/* Auto-generado por RigCom v8.0 — RigScript JNI Bridge */\n"
        "#ifndef %s\n#define %s\n\n"
        "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n",
        guard, guard);

    if (mod) {
        RSNode *d = mod->body;
        while (d && (size_t)pos < bufsz-256) {
            if (d->kind == RS_NODE_FN_DEF && d->exported) {
                const char *rtype = d->ret_type == RS_TY_VOID ? "void" :
                                    d->ret_type == RS_TY_I64  ? "int64_t" :
                                    d->ret_type == RS_TY_F64  ? "double"  : "int64_t";
                pos += snprintf(buf+pos, bufsz-(size_t)pos,
                    "/* @export */ %s %s(", rtype, d->name);
                RSNode *pm = d->params; bool fp2 = true;
                while (pm) {
                    const char *pty = pm->decl_type == RS_TY_F64 ? "double" :
                                      pm->decl_type == RS_TY_F32 ? "float"  : "int64_t";
                    pos += snprintf(buf+pos, bufsz-(size_t)pos, "%s%s %s",
                                    fp2?"":", ", pty, pm->name);
                    fp2 = false; pm = pm->next;
                }
                pos += snprintf(buf+pos, bufsz-(size_t)pos, ");\n");
            }
            d = d->next;
        }
    }

    snprintf(buf+pos, bufsz-(size_t)pos,
        "\n#ifdef __cplusplus\n}\n#endif\n\n#endif /* %s */\n", guard);

    free(src);
    return buf;
}

/* ══════════════════════════════════════════════════════════
   AUTO-JNI BRIDGE GENERATOR
   ══════════════════════════════════════════════════════════ */
bool rigscript_gen_jni_bridge(const char *src_path,
                               const char *java_package,
                               const char *out_java,
                               const char *out_c_wrapper,
                               RigErrorLog *log) {
    FILE *fp = fopen(src_path, "r");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END); long fsz = ftell(fp); rewind(fp);
    char *src = malloc((size_t)fsz + 1);
    if (!src) { fclose(fp); return false; }
    size_t nr = fread(src, 1, (size_t)fsz, fp); src[nr] = '\0'; fclose(fp);

    RigErrorLog tmp = {0};
    RSParser ps;
    rsparser_init(&ps, src, nr, &tmp, src_path);
    RSNode *mod = rsparser_parse(&ps);
    (void)log;

    /* Class name from src path */
    const char *base = strrchr(src_path, '/');
    base = base ? base+1 : src_path;
    char class_name[128];
    strncpy(class_name, base, sizeof(class_name)-1);
    char *dot = strrchr(class_name, '.');
    if (dot) *dot = '\0';
    /* Capitalize */
    if (class_name[0]) class_name[0] = (char)toupper((unsigned char)class_name[0]);

    /* Generate Java class */
    FILE *jf = fopen(out_java, "w");
    if (jf) {
        fprintf(jf, "/* Auto-generado por RigCom v8.0 Auto-JNI Bridge */\n");
        if (java_package && java_package[0])
            fprintf(jf, "package %s;\n\n", java_package);
        fprintf(jf, "public class %s {\n", class_name);
        fprintf(jf, "    static { System.loadLibrary(\"rigscript\"); }\n\n");

        if (mod) {
            RSNode *d = mod->body;
            while (d) {
                if (d->kind == RS_NODE_FN_DEF && d->exported) {
                    const char *jtype = d->ret_type == RS_TY_VOID ? "void" :
                                        d->ret_type == RS_TY_I64  ? "long"   :
                                        d->ret_type == RS_TY_F64  ? "double" : "long";
                    fprintf(jf, "    public static native %s %s(", jtype, d->name);
                    RSNode *pm = d->params; bool first = true;
                    while (pm) {
                        const char *jpty = pm->decl_type == RS_TY_F64 ? "double" :
                                           pm->decl_type == RS_TY_F32 ? "float"  : "long";
                        fprintf(jf, "%s%s %s", first?"":", ", jpty, pm->name);
                        first = false; pm = pm->next;
                    }
                    fprintf(jf, ");\n");
                }
                d = d->next;
            }
        }
        fprintf(jf, "}\n");
        fclose(jf);
    }

    /* Generate C wrapper */
    FILE *cf = fopen(out_c_wrapper, "w");
    if (cf) {
        fprintf(cf, "/* Auto-generado por RigCom v8.0 Auto-JNI Bridge */\n");
        fprintf(cf, "#include <jni.h>\n");
        fprintf(cf, "#include <stdint.h>\n\n");

        /* Forward declarations */
        if (mod) {
            RSNode *d = mod->body;
            while (d) {
                if (d->kind == RS_NODE_FN_DEF && d->exported) {
                    const char *rtype = d->ret_type == RS_TY_VOID ? "void" :
                                        d->ret_type == RS_TY_I64  ? "int64_t" :
                                        d->ret_type == RS_TY_F64  ? "double"  : "int64_t";
                    fprintf(cf, "extern %s %s(", rtype, d->name);
                    RSNode *pm = d->params; bool first = true;
                    while (pm) {
                        const char *pty = pm->decl_type == RS_TY_F64 ? "double" :
                                          pm->decl_type == RS_TY_F32 ? "float"  : "int64_t";
                        fprintf(cf, "%s%s %s", first?"":", ", pty, pm->name);
                        first = false; pm = pm->next;
                    }
                    fprintf(cf, ");\n");
                }
                d = d->next;
            }
        }

        /* JNI wrappers */
        char jni_class[256];
        if (java_package && java_package[0]) {
            char pkg_esc[128]; strncpy(pkg_esc, java_package, 127);
            for (char *c = pkg_esc; *c; c++) if (*c == '.') *c = '_';
            snprintf(jni_class, sizeof(jni_class), "%s_%s", pkg_esc, class_name);
        } else {
            strncpy(jni_class, class_name, sizeof(jni_class)-1);
        }

        if (mod) {
            RSNode *d = mod->body;
            while (d) {
                if (d->kind == RS_NODE_FN_DEF && d->exported) {
                    const char *jrtype = d->ret_type == RS_TY_VOID ? "void" :
                                         d->ret_type == RS_TY_I64  ? "jlong"   :
                                         d->ret_type == RS_TY_F64  ? "jdouble" : "jlong";
                    fprintf(cf, "\nJNIEXPORT %s JNICALL\n", jrtype);
                    fprintf(cf, "Java_%s_%s(JNIEnv *env, jclass cls", jni_class, d->name);
                    RSNode *pm = d->params;
                    while (pm) {
                        const char *jpty = pm->decl_type == RS_TY_F64 ? "jdouble" :
                                           pm->decl_type == RS_TY_F32 ? "jfloat"  : "jlong";
                        fprintf(cf, ", %s %s", jpty, pm->name);
                        pm = pm->next;
                    }
                    fprintf(cf, ") {\n    (void)env; (void)cls;\n");
                    if (d->ret_type != RS_TY_VOID) fprintf(cf, "    return ");
                    else fprintf(cf, "    ");
                    fprintf(cf, "%s(", d->name);
                    pm = d->params; bool first = true;
                    while (pm) {
                        fprintf(cf, "%s%s", first?"":", ", pm->name);
                        first = false; pm = pm->next;
                    }
                    fprintf(cf, ");\n}\n");
                }
                d = d->next;
            }
        }
        fclose(cf);
    }

    free(src);
    return true;
}

/* ── Registrar en vtable ─────────────────────────────────── */
void rigscript_register_frontend(void) {
    frontend_register((LanguageFrontend){
        .extension         = ".rigc",
        .display_name      = "RigScript",
        .compile_to_ir     = rigscript_compile_to_ir,
        .generate_ast_json = rigscript_ast_json,
        .generate_header   = rigscript_generate_header,
    });
    frontend_register((LanguageFrontend){
        .extension         = ".rs",
        .display_name      = "RigScript (.rs)",
        .compile_to_ir     = rigscript_compile_to_ir,
        .generate_ast_json = rigscript_ast_json,
        .generate_header   = rigscript_generate_header,
    });
}
