/* ============================================================
   RigCom v8.0 — include/symtable.h
   Symbol table: hash map + nested scopes
   ============================================================ */
#ifndef SYMTABLE_H
#define SYMTABLE_H

#include "ast.h"
#include <stdint.h>
#include <stdbool.h>

#define SYMTAB_BUCKETS 256u

typedef enum {
    SYM_VAR   = 0,
    SYM_FUNC  = 1,
    SYM_TYPE  = 2,
    SYM_PARAM = 3,
    SYM_ENUM_CONST = 4,
} SymKind;

typedef struct Symbol {
    char          *name;
    SymKind        kind;
    Type          *type;
    int64_t        enum_val;      /* SYM_ENUM_CONST */
    uint32_t       scope_depth;
    ASTNode       *decl_node;
    struct Symbol *chain;         /* hash bucket chain */
} Symbol;

typedef struct Scope Scope;
struct Scope {
    Symbol  *buckets[SYMTAB_BUCKETS];
    Scope   *parent;
    uint32_t depth;
};

typedef struct {
    Scope    *current;
    uint32_t  depth;
} SymTable;

/* ── Lifecycle ──────────────────────────────────────────────── */
SymTable* symtab_new  (void);
void      symtab_free (SymTable *st);

/* ── Scope management ───────────────────────────────────────── */
void symtab_push_scope(SymTable *st);
void symtab_pop_scope (SymTable *st);

/* ── Symbol operations ──────────────────────────────────────── */
bool    symtab_define       (SymTable *st, const char *name, SymKind kind,
                              Type *type, ASTNode *node);
Symbol* symtab_lookup       (SymTable *st, const char *name);  /* all scopes */
Symbol* symtab_lookup_local (SymTable *st, const char *name);  /* current scope only */

#endif /* SYMTABLE_H */
