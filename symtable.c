/* ============================================================
   RigCom v8.0 — src/symtable.c
   Symbol table: FNV-1a hash + nested scopes
   ============================================================ */
#include "../include/symtable.h"
#include <stdlib.h>
#include <string.h>

/* ── FNV-1a 32-bit hash ─────────────────────────────────────── */
static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h % SYMTAB_BUCKETS;
}

/* ── Scope ──────────────────────────────────────────────────── */
static Scope* scope_new(Scope *parent, uint32_t depth) {
    Scope *sc = calloc(1, sizeof(Scope));
    if (!sc) return NULL;
    sc->parent = parent;
    sc->depth  = depth;
    return sc;
}

static void scope_free(Scope *sc) {
    if (!sc) return;
    for (uint32_t i = 0; i < SYMTAB_BUCKETS; i++) {
        Symbol *sym = sc->buckets[i];
        while (sym) {
            Symbol *next = sym->chain;
            free(sym->name);
            free(sym);
            sym = next;
        }
    }
    free(sc);
}

/* ── SymTable lifecycle ─────────────────────────────────────── */
SymTable* symtab_new(void) {
    SymTable *st = malloc(sizeof(SymTable));
    if (!st) return NULL;
    st->depth   = 0;
    st->current = scope_new(NULL, 0);
    if (!st->current) { free(st); return NULL; }
    return st;
}

void symtab_free(SymTable *st) {
    if (!st) return;
    Scope *sc = st->current;
    while (sc) {
        Scope *parent = sc->parent;
        scope_free(sc);
        sc = parent;
    }
    free(st);
}

/* ── Scope push / pop ───────────────────────────────────────── */
void symtab_push_scope(SymTable *st) {
    if (!st) return;
    Scope *sc = scope_new(st->current, ++st->depth);
    if (!sc) return;
    st->current = sc;
}

void symtab_pop_scope(SymTable *st) {
    if (!st || !st->current) return;
    Scope *old = st->current;
    st->current = old->parent;
    st->depth--;
    scope_free(old);
}

/* ── Define symbol in current scope ────────────────────────── */
bool symtab_define(SymTable *st, const char *name, SymKind kind,
                    Type *type, ASTNode *node) {
    if (!st || !name) return false;
    uint32_t h = fnv1a(name);

    /* Check for redefinition in current scope */
    Symbol *sym = st->current->buckets[h];
    while (sym) {
        if (strcmp(sym->name, name) == 0) return false; /* redefinition */
        sym = sym->chain;
    }

    sym = calloc(1, sizeof(Symbol));
    if (!sym) return false;
    sym->name        = malloc(strlen(name) + 1);
    if (!sym->name)  { free(sym); return false; }
    strcpy(sym->name, name);
    sym->kind        = kind;
    sym->type        = type;
    sym->decl_node   = node;
    sym->scope_depth = st->depth;

    /* Insert at head of bucket chain */
    sym->chain                 = st->current->buckets[h];
    st->current->buckets[h]   = sym;
    return true;
}

/* ── Lookup (walk scope stack outward) ──────────────────────── */
Symbol* symtab_lookup(SymTable *st, const char *name) {
    if (!st || !name) return NULL;
    uint32_t h = fnv1a(name);
    Scope *sc = st->current;
    while (sc) {
        Symbol *sym = sc->buckets[h];
        while (sym) {
            if (strcmp(sym->name, name) == 0) return sym;
            sym = sym->chain;
        }
        sc = sc->parent;
    }
    return NULL;
}

/* ── Lookup local scope only ────────────────────────────────── */
Symbol* symtab_lookup_local(SymTable *st, const char *name) {
    if (!st || !name) return NULL;
    uint32_t h = fnv1a(name);
    Symbol *sym = st->current->buckets[h];
    while (sym) {
        if (strcmp(sym->name, name) == 0) return sym;
        sym = sym->chain;
    }
    return NULL;
}
