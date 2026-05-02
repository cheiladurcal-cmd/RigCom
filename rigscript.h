/* ============================================================
   RigCom v8.0 — include/rigscript.h
   RigScript — Lenguaje moderno (TypeScript/Swift syntax)
   que compila a RigIR SSA — hereda todo el backend ARM64.
   Soporta @export → generación automática JNI Bridge.
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef RIGSCRIPT_H
#define RIGSCRIPT_H

#include "rigir.h"
#include "rigctx.h"
#include "error.h"
#include "frontend.h"
#include <stdbool.h>
#include <stdint.h>

/* ── Token kinds de RigScript ────────────────────────────── */
typedef enum {
    RS_TOK_EOF = 0,
    RS_TOK_IDENT,
    RS_TOK_INT_LIT,
    RS_TOK_FLOAT_LIT,
    RS_TOK_STR_LIT,
    RS_TOK_BOOL_LIT,    /* true / false */

    /* Palabras clave */
    RS_TOK_LET,         /* let x: i32 = ...   */
    RS_TOK_CONST,       /* const PI = 3.14     */
    RS_TOK_FN,          /* fn add(a: i32) i32  */
    RS_TOK_RETURN,
    RS_TOK_IF,
    RS_TOK_ELSE,
    RS_TOK_WHILE,
    RS_TOK_FOR,
    RS_TOK_IN,
    RS_TOK_STRUCT,
    RS_TOK_IMPORT,
    RS_TOK_EXPORT,      /* @export annotation  */
    RS_TOK_ASYNC,
    RS_TOK_AWAIT,
    RS_TOK_TYPE,

    /* Tipos primitivos */
    RS_TOK_I8, RS_TOK_I16, RS_TOK_I32, RS_TOK_I64,
    RS_TOK_U8, RS_TOK_U16, RS_TOK_U32, RS_TOK_U64,
    RS_TOK_F32, RS_TOK_F64,
    RS_TOK_BOOL_TY,
    RS_TOK_STR_TY,
    RS_TOK_VOID,

    /* Operadores */
    RS_TOK_PLUS, RS_TOK_MINUS, RS_TOK_STAR, RS_TOK_SLASH,
    RS_TOK_PERCENT, RS_TOK_AMP, RS_TOK_PIPE, RS_TOK_CARET,
    RS_TOK_BANG, RS_TOK_TILDE,
    RS_TOK_EQ, RS_TOK_EQEQ, RS_TOK_BANGEQ,
    RS_TOK_LT, RS_TOK_GT, RS_TOK_LTEQ, RS_TOK_GTEQ,
    RS_TOK_AMPAMP, RS_TOK_PIPEPIPE,
    RS_TOK_ARROW,       /* -> */
    RS_TOK_FATARROW,    /* => */
    RS_TOK_DOTDOT,      /* .. (range) */

    /* Delimitadores */
    RS_TOK_LPAREN, RS_TOK_RPAREN,
    RS_TOK_LBRACE, RS_TOK_RBRACE,
    RS_TOK_LBRACKET, RS_TOK_RBRACKET,
    RS_TOK_COLON, RS_TOK_SEMICOLON, RS_TOK_COMMA, RS_TOK_DOT,
    RS_TOK_HASH,        /* # para anotaciones */

    RS_TOK_UNKNOWN,
} RSTokenKind;

typedef struct {
    RSTokenKind kind;
    const char *start;
    uint32_t    len;
    uint32_t    line;
    union {
        int64_t  ival;
        double   fval;
    };
} RSToken;

/* ── Lexer RigScript ─────────────────────────────────────── */
typedef struct {
    const char *src;
    size_t      src_len;
    size_t      pos;
    uint32_t    line;
    RSToken     cur;
} RSLexer;

void    rslexer_init(RSLexer *lx, const char *src, size_t len);
RSToken rslexer_next(RSLexer *lx);
RSToken rslexer_peek(RSLexer *lx);

/* ── Tipo RigScript ──────────────────────────────────────── */
typedef enum {
    RS_TY_I8, RS_TY_I16, RS_TY_I32, RS_TY_I64,
    RS_TY_U8, RS_TY_U16, RS_TY_U32, RS_TY_U64,
    RS_TY_F32, RS_TY_F64,
    RS_TY_BOOL, RS_TY_STR, RS_TY_VOID,
    RS_TY_STRUCT, RS_TY_PTR, RS_TY_ARRAY,
    RS_TY_UNKNOWN,
} RSTypeKind;

/* ── Nodo AST de RigScript ───────────────────────────────── */
typedef enum {
    RS_NODE_MODULE,
    RS_NODE_FN_DEF,
    RS_NODE_FN_PARAM,
    RS_NODE_LET,
    RS_NODE_CONST,
    RS_NODE_RETURN,
    RS_NODE_IF,
    RS_NODE_WHILE,
    RS_NODE_FOR_IN,
    RS_NODE_BLOCK,
    RS_NODE_CALL,
    RS_NODE_IDENT,
    RS_NODE_INT_LIT,
    RS_NODE_FLOAT_LIT,
    RS_NODE_STR_LIT,
    RS_NODE_BOOL_LIT,
    RS_NODE_BINARY,
    RS_NODE_UNARY,
    RS_NODE_ASSIGN,
    RS_NODE_STRUCT_DEF,
    RS_NODE_FIELD_ACCESS,
    RS_NODE_ARRAY_IDX,
    RS_NODE_RANGE,
} RSNodeKind;

typedef struct RSNode RSNode;
struct RSNode {
    RSNodeKind  kind;
    uint32_t    line;
    RSTypeKind  type;      /* tipo inferido */
    bool        exported;  /* @export annotation */
    char        name[128]; /* fn name / var name */
    RSNode     *left;
    RSNode     *right;
    RSNode     *body;
    RSNode     *cond;
    RSNode     *else_br;
    RSNode     *params;    /* linked via ->next */
    RSNode     *next;
    int64_t     ival;
    double      fval;
    int         op;        /* RSTokenKind del operador */
    RSTypeKind  ret_type;
    RSTypeKind  decl_type;
    RSNode     *args;
    uint32_t    n_args;
};

/* ── Parser RigScript ────────────────────────────────────── */
typedef struct {
    RSLexer     lx;
    RSToken     cur;
    RSToken     peek_tok;
    bool        has_peek;
    RigErrorLog *log;
    const char  *filename;
    uint32_t    error_count;
} RSParser;

void    rsparser_init(RSParser *p, const char *src, size_t len,
                       RigErrorLog *log, const char *filename);
RSNode* rsparser_parse(RSParser *p);

/* ── Compilador RigScript → RigIR ───────────────────────── */
IRModule* rigscript_compile_to_ir(RigCtx *ctx, const char *src_path,
                                   RigErrorLog *log);
char*     rigscript_ast_json(RigCtx *ctx, const char *src_path,
                              RigErrorLog *log);
char*     rigscript_generate_header(RigCtx *ctx, const char *src_path,
                                     RigErrorLog *log);

/* ── JNI Auto-Bridge ─────────────────────────────────────── */
/* Genera .java + wrapper C para funciones @export */
bool rigscript_gen_jni_bridge(const char *src_path,
                               const char *java_package,
                               const char *out_java,
                               const char *out_c_wrapper,
                               RigErrorLog *log);

/* Registrar en la vtable de frontends */
void rigscript_register_frontend(void);

#endif /* RIGSCRIPT_H */
