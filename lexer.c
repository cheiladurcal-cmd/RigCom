/* ============================================================
   RigCom v8.0 — src/lexer.c
   C11 / .rigc tokenizer
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/lexer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* ── Keyword table ──────────────────────────────────────────── */
typedef struct { const char *word; TokenKind kind; } KwEntry;
static const KwEntry KW_TABLE[] = {
    { "int",      TOK_KW_INT      }, { "char",     TOK_KW_CHAR     },
    { "float",    TOK_KW_FLOAT    }, { "double",   TOK_KW_DOUBLE   },
    { "void",     TOK_KW_VOID     }, { "long",     TOK_KW_LONG     },
    { "short",    TOK_KW_SHORT    }, { "unsigned", TOK_KW_UNSIGNED },
    { "signed",   TOK_KW_SIGNED   }, { "const",    TOK_KW_CONST    },
    { "static",   TOK_KW_STATIC   }, { "extern",   TOK_KW_EXTERN   },
    { "inline",   TOK_KW_INLINE   }, { "volatile", TOK_KW_VOLATILE },
    { "register", TOK_KW_REGISTER }, { "auto",     TOK_KW_AUTO     },
    { "struct",   TOK_KW_STRUCT   }, { "union",    TOK_KW_UNION    },
    { "enum",     TOK_KW_ENUM     }, { "typedef",  TOK_KW_TYPEDEF  },
    { "sizeof",   TOK_KW_SIZEOF   }, { "return",   TOK_KW_RETURN   },
    { "if",       TOK_KW_IF       }, { "else",     TOK_KW_ELSE     },
    { "while",    TOK_KW_WHILE    }, { "for",      TOK_KW_FOR      },
    { "do",       TOK_KW_DO       }, { "break",    TOK_KW_BREAK    },
    { "continue", TOK_KW_CONTINUE }, { "goto",     TOK_KW_GOTO     },
    { "switch",   TOK_KW_SWITCH   }, { "case",     TOK_KW_CASE     },
    { "default",  TOK_KW_DEFAULT  }, { "_Bool",    TOK_KW_BOOL     },
    { "bool",     TOK_KW_BOOL     }, { "true",     TOK_KW_TRUE     },
    { "false",    TOK_KW_FALSE    }, { "NULL",     TOK_KW_NULL     },
    { "nullptr",  TOK_KW_NULL     }, { "restrict", TOK_KW_RESTRICT },
    { NULL, 0 }
};

/* ── Lifecycle ──────────────────────────────────────────────── */
void lexer_init(Lexer *lx, const char *src, size_t len, const char *file) {
    lx->src       = src;
    lx->len       = len;
    lx->pos       = 0;
    lx->line      = 1;
    lx->col       = 1;
    lx->file      = file ? file : "<unknown>";
    lx->has_error = false;
    lx->errbuf[0] = '\0';
}

/* ── Internal helpers ───────────────────────────────────────── */
static char cur(const Lexer *lx) {
    return (lx->pos < lx->len) ? lx->src[lx->pos] : '\0';
}
static char peek_ch(const Lexer *lx, size_t offset) {
    size_t p = lx->pos + offset;
    return (p < lx->len) ? lx->src[p] : '\0';
}
static char advance_ch(Lexer *lx) {
    char c = lx->src[lx->pos++];
    if (c == '\n') { lx->line++; lx->col = 1; }
    else            { lx->col++; }
    return c;
}
static bool match_ch(Lexer *lx, char expected) {
    if (lx->pos < lx->len && lx->src[lx->pos] == expected) {
        advance_ch(lx);
        return true;
    }
    return false;
}
static void skip_whitespace(Lexer *lx) {
    while (lx->pos < lx->len) {
        char c = cur(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance_ch(lx);
        } else if (c == '/' && peek_ch(lx,1) == '/') {
            /* Line comment */
            while (lx->pos < lx->len && cur(lx) != '\n')
                advance_ch(lx);
        } else if (c == '/' && peek_ch(lx,1) == '*') {
            /* Block comment */
            advance_ch(lx); advance_ch(lx);
            while (lx->pos + 1 < lx->len) {
                if (cur(lx) == '*' && peek_ch(lx,1) == '/') {
                    advance_ch(lx); advance_ch(lx);
                    break;
                }
                advance_ch(lx);
            }
        } else {
            break;
        }
    }
}
static void skip_preprocessor_line(Lexer *lx) {
    /* Skip # ... until newline (handles continuation with \) */
    while (lx->pos < lx->len) {
        char c = advance_ch(lx);
        if (c == '\n') break;
        if (c == '\\' && cur(lx) == '\n') advance_ch(lx);
    }
}

static Token make_tok(TokenKind kind, const char *start, uint32_t len,
                       uint32_t line, uint32_t col) {
    Token t;
    t.kind      = kind;
    t.start     = start;
    t.len       = len;
    t.line      = line;
    t.col       = col;
    t.int_val   = 0;
    return t;
}

static Token error_tok(Lexer *lx, const char *msg,
                        const char *start, uint32_t line, uint32_t col) {
    lx->has_error = true;
    snprintf(lx->errbuf, sizeof(lx->errbuf), "%s", msg);
    return make_tok(TOK_ERROR, start, 1, line, col);
}

/* ── Lex integer / float literal ────────────────────────────── */
static Token lex_number(Lexer *lx) {
    const char *start = lx->src + lx->pos;
    uint32_t    line  = lx->line;
    uint32_t    col   = lx->col;
    bool        is_float = false;
    int64_t     ival  = 0;
    double      fval  = 0.0;

    if (cur(lx) == '0' && (peek_ch(lx,1) == 'x' || peek_ch(lx,1) == 'X')) {
        /* Hexadecimal */
        advance_ch(lx); advance_ch(lx);
        while (lx->pos < lx->len && isxdigit((unsigned char)cur(lx)))
            advance_ch(lx);
        char tmp[64];
        size_t tlen = (size_t)(lx->src + lx->pos - start);
        if (tlen >= sizeof(tmp)) tlen = sizeof(tmp)-1;
        memcpy(tmp, start, tlen); tmp[tlen]='\0';
        ival = (int64_t)strtoull(tmp, NULL, 16);
    } else {
        /* Decimal / float */
        while (lx->pos < lx->len && isdigit((unsigned char)cur(lx)))
            advance_ch(lx);
        if (cur(lx) == '.' && isdigit((unsigned char)peek_ch(lx,1))) {
            is_float = true;
            advance_ch(lx);
            while (lx->pos < lx->len && isdigit((unsigned char)cur(lx)))
                advance_ch(lx);
        }
        if (cur(lx) == 'e' || cur(lx) == 'E') {
            is_float = true;
            advance_ch(lx);
            if (cur(lx) == '+' || cur(lx) == '-') advance_ch(lx);
            while (lx->pos < lx->len && isdigit((unsigned char)cur(lx)))
                advance_ch(lx);
        }
        /* Suffixes: u U l L f F */
        while (cur(lx)=='u'||cur(lx)=='U'||cur(lx)=='l'||cur(lx)=='L'||
               cur(lx)=='f'||cur(lx)=='F') {
            if (cur(lx)=='f'||cur(lx)=='F') is_float=true;
            advance_ch(lx);
        }
        char tmp[64];
        size_t tlen = (size_t)(lx->src + lx->pos - start);
        if (tlen >= sizeof(tmp)) tlen = sizeof(tmp)-1;
        memcpy(tmp, start, tlen); tmp[tlen]='\0';
        if (is_float) fval = strtod(tmp, NULL);
        else          ival = (int64_t)strtoll(tmp, NULL, 10);
    }

    uint32_t toklen = (uint32_t)(lx->src + lx->pos - start);
    Token t = make_tok(is_float ? TOK_FLOAT_LIT : TOK_INT_LIT,
                       start, toklen, line, col);
    if (is_float) t.float_val = fval;
    else          t.int_val   = ival;
    return t;
}

/* ── Lex string literal ─────────────────────────────────────── */
static Token lex_string(Lexer *lx) {
    const char *start = lx->src + lx->pos;
    uint32_t    line  = lx->line;
    uint32_t    col   = lx->col;
    advance_ch(lx); /* consume opening " */
    while (lx->pos < lx->len && cur(lx) != '"') {
        if (cur(lx) == '\\') advance_ch(lx);
        if (lx->pos < lx->len) advance_ch(lx);
    }
    if (lx->pos < lx->len) advance_ch(lx); /* consume closing " */
    return make_tok(TOK_STRING_LIT, start,
                    (uint32_t)(lx->src + lx->pos - start), line, col);
}

/* ── Lex char literal ───────────────────────────────────────── */
static Token lex_char(Lexer *lx) {
    const char *start = lx->src + lx->pos;
    uint32_t    line  = lx->line;
    uint32_t    col   = lx->col;
    advance_ch(lx); /* consume ' */
    if (cur(lx) == '\\') advance_ch(lx);
    if (lx->pos < lx->len) advance_ch(lx);
    int64_t val = (start[1] == '\\') ? (int64_t)start[2] : (int64_t)start[1];
    if (lx->pos < lx->len && cur(lx) == '\'') advance_ch(lx);
    Token t = make_tok(TOK_CHAR_LIT, start,
                       (uint32_t)(lx->src + lx->pos - start), line, col);
    t.int_val = val;
    return t;
}

/* ── Lex identifier / keyword ───────────────────────────────── */
static Token lex_ident(Lexer *lx) {
    const char *start = lx->src + lx->pos;
    uint32_t    line  = lx->line;
    uint32_t    col   = lx->col;

    while (lx->pos < lx->len) {
        char c = cur(lx);
        if (!isalnum((unsigned char)c) && c != '_') break;
        advance_ch(lx);
    }
    uint32_t len = (uint32_t)(lx->src + lx->pos - start);

    /* Keyword match */
    for (int i = 0; KW_TABLE[i].word; i++) {
        if ((uint32_t)strlen(KW_TABLE[i].word) == len &&
            memcmp(KW_TABLE[i].word, start, len) == 0) {
            return make_tok(KW_TABLE[i].kind, start, len, line, col);
        }
    }
    return make_tok(TOK_IDENT, start, len, line, col);
}

/* ═══════════════════════════════════════════════════════════════
   lexer_next — produce the next token
   ═══════════════════════════════════════════════════════════════ */
Token lexer_next(Lexer *lx) {
    skip_whitespace(lx);

    if (lx->pos >= lx->len)
        return make_tok(TOK_EOF, lx->src + lx->pos, 0, lx->line, lx->col);

    const char *start = lx->src + lx->pos;
    uint32_t    line  = lx->line;
    uint32_t    col   = lx->col;
    char c = cur(lx);

    /* Numbers */
    if (isdigit((unsigned char)c))
        return lex_number(lx);

    /* Float starting with dot */
    if (c == '.' && isdigit((unsigned char)peek_ch(lx,1)))
        return lex_number(lx);

    /* Strings */
    if (c == '"') return lex_string(lx);
    if (c == '\'') return lex_char(lx);

    /* Identifiers / keywords */
    if (isalpha((unsigned char)c) || c == '_')
        return lex_ident(lx);

    /* Preprocessor directives — skip */
    if (c == '#') {
        advance_ch(lx);
        skip_preprocessor_line(lx);
        return lexer_next(lx); /* recurse past directive */
    }

    /* Operators and punctuation */
    advance_ch(lx);

#define MAKE(k) make_tok(k, start, (uint32_t)(lx->src+lx->pos-start), line, col)
#define MATCH2(expected, k2, k1) \
    (match_ch(lx, expected) ? MAKE(k2) : MAKE(k1))

    switch (c) {
        case '(': return MAKE(TOK_LPAREN);
        case ')': return MAKE(TOK_RPAREN);
        case '{': return MAKE(TOK_LBRACE);
        case '}': return MAKE(TOK_RBRACE);
        case '[': return MAKE(TOK_LBRACKET);
        case ']': return MAKE(TOK_RBRACKET);
        case ';': return MAKE(TOK_SEMI);
        case ':': return MAKE(TOK_COLON);
        case ',': return MAKE(TOK_COMMA);
        case '~': return MAKE(TOK_TILDE);
        case '?': return MAKE(TOK_QUESTION);

        case '.':
            if (cur(lx) == '.' && peek_ch(lx,1) == '.') {
                advance_ch(lx); advance_ch(lx);
                return MAKE(TOK_ELLIPSIS);
            }
            return MAKE(TOK_DOT);

        case '+':
            if (match_ch(lx,'+')) return MAKE(TOK_PLUS_PLUS);
            if (match_ch(lx,'=')) return MAKE(TOK_PLUS_EQ);
            return MAKE(TOK_PLUS);

        case '-':
            if (match_ch(lx,'-')) return MAKE(TOK_MINUS_MINUS);
            if (match_ch(lx,'=')) return MAKE(TOK_MINUS_EQ);
            if (match_ch(lx,'>')) return MAKE(TOK_ARROW);
            return MAKE(TOK_MINUS);

        case '*':
            return MATCH2('=', TOK_STAR_EQ, TOK_STAR);

        case '/':
            return MATCH2('=', TOK_SLASH_EQ, TOK_SLASH);

        case '%':
            return MATCH2('=', TOK_PERCENT_EQ, TOK_PERCENT);

        case '&':
            if (match_ch(lx,'&')) return MAKE(TOK_AMP_AMP);
            if (match_ch(lx,'=')) return MAKE(TOK_AMP_EQ);
            return MAKE(TOK_AMP);

        case '|':
            if (match_ch(lx,'|')) return MAKE(TOK_PIPE_PIPE);
            if (match_ch(lx,'=')) return MAKE(TOK_PIPE_EQ);
            return MAKE(TOK_PIPE);

        case '^':
            return MATCH2('=', TOK_CARET_EQ, TOK_CARET);

        case '!':
            return MATCH2('=', TOK_BANG_EQ, TOK_BANG);

        case '=':
            return MATCH2('=', TOK_EQ_EQ, TOK_EQ);

        case '<':
            if (match_ch(lx,'<')) {
                if (match_ch(lx,'=')) return MAKE(TOK_LSHIFT_EQ);
                return MAKE(TOK_LSHIFT);
            }
            return MATCH2('=', TOK_LT_EQ, TOK_LT);

        case '>':
            if (match_ch(lx,'>')) {
                if (match_ch(lx,'=')) return MAKE(TOK_RSHIFT_EQ);
                return MAKE(TOK_RSHIFT);
            }
            return MATCH2('=', TOK_GT_EQ, TOK_GT);

        default: {
            char msg[64];
            snprintf(msg, sizeof(msg), "Carácter inesperado: '%c' (0x%02x)", c, (unsigned char)c);
            return error_tok(lx, msg, start, line, col);
        }
    }
#undef MAKE
#undef MATCH2
}

/* ── Peek without consuming ─────────────────────────────────── */
Token lexer_peek(Lexer *lx) {
    size_t   save_pos  = lx->pos;
    uint32_t save_line = lx->line;
    uint32_t save_col  = lx->col;
    Token t = lexer_next(lx);
    lx->pos  = save_pos;
    lx->line = save_line;
    lx->col  = save_col;
    return t;
}

/* ── Utilities ──────────────────────────────────────────────── */
const char* tok_kind_name(TokenKind k) {
    static const char *names[] = {
        "INT_LIT","FLOAT_LIT","STRING_LIT","CHAR_LIT","IDENT",
        "int","char","float","double","void","long","short","unsigned",
        "signed","const","static","extern","inline","volatile","register",
        "auto","struct","union","enum","typedef","sizeof","return",
        "if","else","while","for","do","break","continue","goto",
        "switch","case","default","bool","true","false","NULL","restrict",
        "(",")","{","}","[","]",";",":",",",".","->","...","#",
        "+","-","*","/","%","&","|","^","~","<<",">>",
        "==","!=","<","<=",">",">=","&&","||","!",
        "=","+=","-=","*=","/=","%=","&=","|=","^=","<<=",">>=",
        "++","--","?","EOF","ERROR"
    };
    if ((int)k >= 0 && k < TOK_COUNT)
        return names[(int)k];
    return "?";
}

bool tok_is_keyword(TokenKind k) {
    return k >= TOK_KW_INT && k <= TOK_KW_RESTRICT;
}

bool tok_is_type_start(TokenKind k) {
    switch (k) {
        case TOK_KW_INT:    case TOK_KW_CHAR:   case TOK_KW_FLOAT:
        case TOK_KW_DOUBLE: case TOK_KW_VOID:   case TOK_KW_LONG:
        case TOK_KW_SHORT:  case TOK_KW_UNSIGNED:case TOK_KW_SIGNED:
        case TOK_KW_CONST:  case TOK_KW_STRUCT: case TOK_KW_UNION:
        case TOK_KW_ENUM:   case TOK_KW_BOOL:   case TOK_KW_VOLATILE:
            return true;
        default:
            return false;
    }
}
