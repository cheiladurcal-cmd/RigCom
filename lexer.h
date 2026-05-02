/* ============================================================
   RigCom v8.0 — include/lexer.h
   Lexer: tokeniza C11 / .rigc
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef LEXER_H
#define LEXER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════
   TOKEN KINDS
   ═══════════════════════════════════════════════════════════════ */
typedef enum {
    /* ── Literals ── */
    TOK_INT_LIT    = 0,
    TOK_FLOAT_LIT,
    TOK_STRING_LIT,
    TOK_CHAR_LIT,

    /* ── Identifier / keywords ── */
    TOK_IDENT,

    /* ── C keywords ── */
    TOK_KW_INT,     TOK_KW_CHAR,    TOK_KW_FLOAT,   TOK_KW_DOUBLE,
    TOK_KW_VOID,    TOK_KW_LONG,    TOK_KW_SHORT,   TOK_KW_UNSIGNED,
    TOK_KW_SIGNED,  TOK_KW_CONST,   TOK_KW_STATIC,  TOK_KW_EXTERN,
    TOK_KW_INLINE,  TOK_KW_VOLATILE,TOK_KW_REGISTER,TOK_KW_AUTO,
    TOK_KW_STRUCT,  TOK_KW_UNION,   TOK_KW_ENUM,    TOK_KW_TYPEDEF,
    TOK_KW_SIZEOF,  TOK_KW_RETURN,  TOK_KW_IF,      TOK_KW_ELSE,
    TOK_KW_WHILE,   TOK_KW_FOR,     TOK_KW_DO,      TOK_KW_BREAK,
    TOK_KW_CONTINUE,TOK_KW_GOTO,    TOK_KW_SWITCH,  TOK_KW_CASE,
    TOK_KW_DEFAULT, TOK_KW_BOOL,    TOK_KW_TRUE,    TOK_KW_FALSE,
    TOK_KW_NULL,    TOK_KW_RESTRICT,

    /* ── Punctuation / delimiters ── */
    TOK_LPAREN,     /* ( */
    TOK_RPAREN,     /* ) */
    TOK_LBRACE,     /* { */
    TOK_RBRACE,     /* } */
    TOK_LBRACKET,   /* [ */
    TOK_RBRACKET,   /* ] */
    TOK_SEMI,       /* ; */
    TOK_COLON,      /* : */
    TOK_COMMA,      /* , */
    TOK_DOT,        /* . */
    TOK_ARROW,      /* -> */
    TOK_ELLIPSIS,   /* ... */
    TOK_HASH,       /* # */

    /* ── Arithmetic operators ── */
    TOK_PLUS,       /* + */
    TOK_MINUS,      /* - */
    TOK_STAR,       /* * */
    TOK_SLASH,      /* / */
    TOK_PERCENT,    /* % */

    /* ── Bitwise ── */
    TOK_AMP,        /* & */
    TOK_PIPE,       /* | */
    TOK_CARET,      /* ^ */
    TOK_TILDE,      /* ~ */
    TOK_LSHIFT,     /* << */
    TOK_RSHIFT,     /* >> */

    /* ── Comparison ── */
    TOK_EQ_EQ,      /* == */
    TOK_BANG_EQ,    /* != */
    TOK_LT,         /* < */
    TOK_LT_EQ,      /* <= */
    TOK_GT,         /* > */
    TOK_GT_EQ,      /* >= */

    /* ── Logical ── */
    TOK_AMP_AMP,    /* && */
    TOK_PIPE_PIPE,  /* || */
    TOK_BANG,       /* ! */

    /* ── Assignment ── */
    TOK_EQ,         /* = */
    TOK_PLUS_EQ,    /* += */
    TOK_MINUS_EQ,   /* -= */
    TOK_STAR_EQ,    /* *= */
    TOK_SLASH_EQ,   /* /= */
    TOK_PERCENT_EQ, /* %= */
    TOK_AMP_EQ,     /* &= */
    TOK_PIPE_EQ,    /* |= */
    TOK_CARET_EQ,   /* ^= */
    TOK_LSHIFT_EQ,  /* <<= */
    TOK_RSHIFT_EQ,  /* >>= */

    /* ── Increment / decrement ── */
    TOK_PLUS_PLUS,  /* ++ */
    TOK_MINUS_MINUS,/* -- */

    /* ── Ternary ── */
    TOK_QUESTION,   /* ? */

    /* ── Special ── */
    TOK_EOF,
    TOK_ERROR,
    TOK_COUNT,
} TokenKind;

/* ═══════════════════════════════════════════════════════════════
   TOKEN
   ═══════════════════════════════════════════════════════════════ */
typedef struct {
    TokenKind   kind;
    const char *start;   /* pointer into source buffer */
    uint32_t    len;
    uint32_t    line;
    uint32_t    col;
    union {
        int64_t  int_val;
        double   float_val;
    };
} Token;

/* ═══════════════════════════════════════════════════════════════
   LEXER
   ═══════════════════════════════════════════════════════════════ */
typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
    uint32_t    line;
    uint32_t    col;
    const char *file;
    bool        has_error;
    char        errbuf[256];
} Lexer;

/* ── Lifecycle ── */
void  lexer_init (Lexer *lx, const char *src, size_t len, const char *file);

/* ── Token production ── */
Token lexer_next (Lexer *lx);    /* consume next token          */
Token lexer_peek (Lexer *lx);    /* peek without consuming      */

/* ── Utilities ── */
const char* tok_kind_name(TokenKind k);
bool        tok_is_keyword(TokenKind k);
bool        tok_is_type_start(TokenKind k);  /* int, char, struct, etc. */

#endif /* LEXER_H */
