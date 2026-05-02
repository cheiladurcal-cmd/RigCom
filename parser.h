/* ============================================================
   RigCom v8.0 — include/parser.h
   Recursive-descent parser for C / .rigc
   ============================================================ */
#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"
#include "error.h"

typedef struct {
    Lexer        *lx;
    RigErrorLog  *log;
    ASTArena     *arena;
    const char   *file;
    Token         current;
    Token         lookahead;
    bool          panic_mode;   /* error recovery flag */
    uint32_t      error_count;
} Parser;

/* ── Lifecycle ──────────────────────────────────────────────── */
void     parser_init (Parser *p, Lexer *lx, RigErrorLog *log,
                      ASTArena *arena, const char *file);
ASTNode* parser_parse(Parser *p);            /* returns translation unit */

/* ── Sub-parsers (exposed for testing) ─────────────────────── */
ASTNode* parse_declaration(Parser *p);
ASTNode* parse_statement  (Parser *p);
ASTNode* parse_expression (Parser *p);

#endif /* PARSER_H */
