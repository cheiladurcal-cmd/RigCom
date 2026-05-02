/* ============================================================
   RigCom v8.0 — src/ast.c
   AST arena allocator + type helpers
   ============================================================ */
#include "../include/ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Internal: new bump block ───────────────────────────────── */
static ASTBlock* arena_block_new(size_t min_cap) {
    size_t cap = ARENA_BLOCK_SIZE;
    if (cap < min_cap) cap = min_cap;
    ASTBlock *b = malloc(sizeof(ASTBlock));
    if (!b) return NULL;
    b->data = malloc(cap);
    if (!b->data) { free(b); return NULL; }
    b->used = 0;
    b->cap  = cap;
    b->next = NULL;
    return b;
}

/* ── Internal: bump-alloc aligned + zeroed ──────────────────── */
static void* arena_bump(ASTBlock **cur_ptr, ASTBlock **first_ptr, size_t sz) {
    size_t aligned = (sz + 7u) & ~7u;
    ASTBlock *cur  = *cur_ptr;
    if (!cur || cur->used + aligned > cur->cap) {
        ASTBlock *nb = arena_block_new(aligned);
        if (!nb) return NULL;
        if (cur) cur->next = nb;
        else     *first_ptr = nb;
        *cur_ptr = nb;
        cur = nb;
    }
    void *ptr = cur->data + cur->used;
    cur->used += aligned;
    memset(ptr, 0, sz);
    return ptr;
}

/* ── Internal: free a block chain ───────────────────────────── */
static void arena_chain_free(ASTBlock *b) {
    while (b) { ASTBlock *nx = b->next; free(b->data); free(b); b = nx; }
}

/* ── Arena creation ─────────────────────────────────────────── */
ASTArena* ast_arena_new(void) {
    ASTArena *a = calloc(1, sizeof(ASTArena));
    if (!a) return NULL;

    a->node_cur   = arena_block_new(ARENA_BLOCK_SIZE);
    a->node_first = a->node_cur;
    a->type_cur   = arena_block_new(ARENA_BLOCK_SIZE);
    a->type_first = a->type_cur;
    a->str_cap    = 65536;
    a->str_buf    = malloc(a->str_cap);
    a->str_pos    = 0;

    if (!a->node_cur || !a->type_cur || !a->str_buf) {
        arena_chain_free(a->node_first);
        arena_chain_free(a->type_first);
        free(a->str_buf);
        free(a);
        return NULL;
    }
    return a;
}

/* ── Arena destruction — O(N_blocks) not O(N_nodes) ─────────── */
void ast_arena_free(ASTArena *a) {
    if (!a) return;
    arena_chain_free(a->node_first);
    arena_chain_free(a->type_first);
    free(a->str_buf);
    free(a);
}

/* ── Allocate AST node — bump pointer ───────────────────────── */
ASTNode* ast_node_new(ASTArena *a, NodeKind kind, uint32_t line, uint32_t col) {
    ASTNode *n = arena_bump(&a->node_cur, &a->node_first, sizeof(ASTNode));
    if (!n) return NULL;
    n->kind = kind;
    n->line = line;
    n->col  = col;
    a->count++;
    return n;
}

/* ── Allocate Type — bump pointer ───────────────────────────── */
Type* ast_type_new(ASTArena *a, TypeKind kind) {
    Type *t = arena_bump(&a->type_cur, &a->type_first, sizeof(Type));
    if (!t) return NULL;
    t->kind = kind;
    a->type_count++;
    return t;
}

/* ── Intern a string into the arena ────────────────────────── */
char* ast_intern(ASTArena *a, const char *s, size_t len) {
    if (!s) return NULL;
    /* Check if string fits */
    if (a->str_pos + len + 1 >= a->str_cap) {
        size_t new_cap = a->str_cap * 2 + len + 1;
        char *tmp = realloc(a->str_buf, new_cap);
        if (!tmp) return NULL;
        a->str_buf = tmp;
        a->str_cap = new_cap;
    }
    char *result = a->str_buf + a->str_pos;
    memcpy(result, s, len);
    result[len] = '\0';
    a->str_pos += len + 1;
    return result;
}

/* ── Type predicates ────────────────────────────────────────── */
bool type_is_integer(const Type *t) {
    if (!t) return false;
    return t->kind == TY_CHAR  || t->kind == TY_SHORT ||
           t->kind == TY_INT   || t->kind == TY_LONG  ||
           t->kind == TY_UCHAR || t->kind == TY_USHORT||
           t->kind == TY_UINT  || t->kind == TY_ULONG ||
           t->kind == TY_BOOL;
}

bool type_is_float(const Type *t) {
    if (!t) return false;
    return t->kind == TY_FLOAT || t->kind == TY_DOUBLE;
}

bool type_is_numeric(const Type *t) {
    return type_is_integer(t) || type_is_float(t);
}

bool type_is_pointer(const Type *t) {
    if (!t) return false;
    return t->kind == TY_PTR || t->kind == TY_ARRAY;
}

bool type_compatible(const Type *a, const Type *b) {
    if (!a || !b) return false;
    if (a->kind == b->kind) return true;
    /* Numeric promotion */
    if (type_is_numeric(a) && type_is_numeric(b)) return true;
    /* Pointer <-> array */
    if (type_is_pointer(a) && type_is_pointer(b)) return true;
    /* NULL pointer */
    if (type_is_pointer(a) && type_is_integer(b)) return true;
    if (type_is_integer(a) && type_is_pointer(b)) return true;
    return false;
}

uint32_t type_size(const Type *t) {
    if (!t) return 0;
    switch (t->kind) {
        case TY_VOID:   return 0;
        case TY_BOOL:   return 1;
        case TY_CHAR:
        case TY_UCHAR:  return 1;
        case TY_SHORT:
        case TY_USHORT: return 2;
        case TY_INT:
        case TY_UINT:
        case TY_FLOAT:  return 4;
        case TY_LONG:
        case TY_ULONG:
        case TY_DOUBLE:
        case TY_PTR:    return 8;
        case TY_ARRAY:
            return t->array_len * type_size(t->base);
        default:        return 8; /* struct: approximate */
    }
}

const char* type_name(const Type *t) {
    if (!t) return "<null>";
    static const char *names[] = {
        "void", "bool",
        "char", "short", "int", "long",
        "unsigned char", "unsigned short", "unsigned int", "unsigned long",
        "float", "double",
        "pointer", "array",
        "function", "struct", "union", "enum",
        "<typedef>", "<unknown>"
    };
    if ((int)t->kind < (int)(sizeof(names)/sizeof(names[0])))
        return names[t->kind];
    return "<type>";
}

/* =========================================================================
   AUTO-HEADER GENERATOR (C → H)
   ========================================================================= */
#include <stdarg.h>

typedef struct { char *buf; size_t pos; size_t cap; } HBuilder;
static void hb_printf(HBuilder *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (b->pos + n + 1 >= b->cap) {
        b->cap = (b->cap + n + 1024) * 2;
        b->buf = realloc(b->buf, b->cap);
    }
    va_start(ap, fmt);
    vsnprintf(b->buf + b->pos, b->cap - b->pos, fmt, ap);
    va_end(ap);
    b->pos += n;
}

static void type_to_c(Type *t, HBuilder *hb) {
    if (!t) return;
    if (t->is_const) hb_printf(hb, "const ");
    if (t->kind == TY_PTR || t->kind == TY_ARRAY) {
        type_to_c(t->base, hb);
        hb_printf(hb, t->kind == TY_PTR ? "*" : "[]");
        return;
    }
    if (t->kind == TY_STRUCT) hb_printf(hb, "struct %s ", t->name ? t->name : "");
    else if (t->kind == TY_UNION) hb_printf(hb, "union %s ", t->name ? t->name : "");
    else if (t->kind == TY_ENUM) hb_printf(hb, "enum %s ", t->name ? t->name : "");
    else if (t->kind == TY_TYPEDEF_REF) hb_printf(hb, "%s ", t->name);
    else hb_printf(hb, "%s ", type_name(t));
}

char* ast_generate_header(ASTNode *tu, const char *filename) {
    if (!tu || tu->kind != NODE_TRANSLATION_UNIT) return NULL;
    HBuilder hb = { malloc(1024), 0, 1024 };
    hb.buf[0] = '\0';

    /* Extraer nombre base para el #ifndef (ej: src/main.c -> MAIN_H) */
    char def_name[128];
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    int i = 0;
    while (base[i] && base[i] != '.' && i < 120) {
        def_name[i] = (base[i] >= 'a' && base[i] <= 'z') ? base[i] - 32 : base[i];
        i++;
    }
    def_name[i] = '\0';

    hb_printf(&hb, "/* Auto-generado por RigCom v8.0 */\n");
    hb_printf(&hb, "#ifndef %s_H\n#define %s_H\n\n", def_name, def_name);

    for (uint32_t d = 0; d < tu->tu.n_decls; d++) {
        ASTNode *node = tu->tu.decls[d];

        /* Exportar Funciones Públicas (no estáticas) */
        if (node->kind == NODE_FUNC_DEF && !node->func.is_static) {
            type_to_c(node->func.ret_type, &hb);
            hb_printf(&hb, "%s(", node->func.name);
            ParamList *pl = &node->func.params;
            if (pl->count == 0) hb_printf(&hb, "void");
            for (uint32_t p = 0; p < pl->count; p++) {
                type_to_c(pl->params[p]->var_decl.decl_type, &hb);
                hb_printf(&hb, "%s", pl->params[p]->var_decl.name);
                if (p < pl->count - 1) hb_printf(&hb, ", ");
            }
            if (pl->variadic) hb_printf(&hb, ", ...");
            hb_printf(&hb, ");\n\n");
        }

        /* Exportar Typedefs */
        if (node->kind == NODE_TYPEDEF_DECL) {
            hb_printf(&hb, "typedef ");
            type_to_c(node->typedef_decl.of, &hb);
            hb_printf(&hb, "%s;\n\n", node->typedef_decl.alias);
        }
    }

    hb_printf(&hb, "#endif /* %s_H */\n", def_name);
    return hb.buf;
}
