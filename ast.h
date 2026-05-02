/* ============================================================
   RigCom v8.0 — include/ast.h
   Abstract Syntax Tree: nodes, types, arena allocator
   Author: Richard Felipe Urbina
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef AST_H
#define AST_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════
   TYPE SYSTEM
   ═══════════════════════════════════════════════════════════════ */

typedef enum {
    TY_VOID = 0,
    TY_BOOL,
    TY_CHAR,    TY_SHORT,   TY_INT,    TY_LONG,
    TY_UCHAR,   TY_USHORT,  TY_UINT,   TY_ULONG,
    TY_FLOAT,   TY_DOUBLE,
    TY_PTR,
    TY_ARRAY,
    TY_FUNC,
    TY_STRUCT,
    TY_UNION,
    TY_ENUM,
    TY_TYPEDEF_REF,
    TY_UNKNOWN,
} TypeKind;

typedef struct Type Type;

/* ── Struct field descriptor ── */
typedef struct StructField {
    char               *name;
    Type               *type;
    uint32_t            offset;   /* byte offset within struct */
    struct StructField *next;
} StructField;

struct Type {
    TypeKind     kind;
    char        *name;        /* struct/union/typedef name */
    Type        *base;        /* ptr/array element type   */
    uint32_t     array_len;   /* TY_ARRAY: element count  */
    Type       **params;      /* TY_FUNC: param types     */
    uint32_t     n_params;
    Type        *ret;         /* TY_FUNC: return type     */
    bool         is_const;
    bool         variadic;    /* TY_FUNC: has ...         */
    /* TY_STRUCT / TY_UNION: field table */
    StructField *fields;      /* linked list of fields    */
    uint32_t     n_fields;
    uint32_t     total_size;  /* sizeof in bytes          */
};

/* ═══════════════════════════════════════════════════════════════
   AST NODE KINDS
   ═══════════════════════════════════════════════════════════════ */

typedef enum {
    /* ── Literals ── */
    NODE_INT_LIT = 0,
    NODE_FLOAT_LIT,
    NODE_STRING_LIT,
    NODE_CHAR_LIT,

    /* ── Expressions ── */
    NODE_IDENT_EXPR,
    NODE_BINARY_EXPR,
    NODE_UNARY_EXPR,
    NODE_ASSIGN_EXPR,
    NODE_TERNARY_EXPR,
    NODE_CALL_EXPR,
    NODE_INDEX_EXPR,
    NODE_MEMBER_EXPR,
    NODE_ADDR_EXPR,
    NODE_DEREF_EXPR,
    NODE_SIZEOF_EXPR,
    NODE_CAST_EXPR,

    /* ── Statements ── */
    NODE_EXPR_STMT,
    NODE_BLOCK,
    NODE_RETURN_STMT,
    NODE_IF_STMT,
    NODE_WHILE_STMT,
    NODE_FOR_STMT,
    NODE_DO_WHILE_STMT,
    NODE_BREAK_STMT,
    NODE_CONTINUE_STMT,

    /* ── Declarations ── */
    NODE_VAR_DECL,
    NODE_PARAM_DECL,
    NODE_FUNC_DECL,
    NODE_FUNC_DEF,
    NODE_TYPEDEF_DECL,
    NODE_TRANSLATION_UNIT,

    NODE_KIND_COUNT,
} NodeKind;

/* ═══════════════════════════════════════════════════════════════
   PARAMETER LIST  (used inside function decl/def)
   ═══════════════════════════════════════════════════════════════ */

typedef struct ASTNode ASTNode;

typedef struct {
    ASTNode **params;
    uint32_t  count;
    uint32_t  capacity;
    bool      variadic;
} ParamList;

/* ═══════════════════════════════════════════════════════════════
   AST NODE  (tagged union)
   ═══════════════════════════════════════════════════════════════ */

struct ASTNode {
    NodeKind kind;
    uint32_t line;
    uint32_t col;
    Type    *resolved_type;   /* filled by type-checker */

    union {
        /* ── INT / CHAR literal ── */
        struct { int64_t  value; }                 int_lit;

        /* ── FLOAT literal ── */
        struct { double   value; }                 float_lit;

        /* ── STRING literal ── */
        struct { char    *value; uint32_t len; }   str_lit;

        /* ── IDENTIFIER ── */
        struct { char    *name; }                  ident;

        /* ── BINARY expression ── */
        struct {
            int      op;      /* TokenKind cast to int */
            ASTNode *left;
            ASTNode *right;
        } binary;

        /* ── UNARY expression ── */
        struct {
            int      op;
            ASTNode *operand;
            bool     postfix;
        } unary;

        /* ── ASSIGN expression ── */
        struct {
            int      op;
            ASTNode *lhs;
            ASTNode *rhs;
        } assign;

        /* ── TERNARY expression ── */
        struct {
            ASTNode *cond;
            ASTNode *then_expr;
            ASTNode *else_expr;
        } ternary;

        /* ── CALL expression ── */
        struct {
            ASTNode  *callee;
            ASTNode **args;
            uint32_t  n_args;
            uint32_t  cap;
        } call;

        /* ── INDEX expression ── */
        struct {
            ASTNode *array;
            ASTNode *index;
        } idx;

        /* ── MEMBER expression (. and ->) ── */
        struct {
            ASTNode *object;
            char    *field;
            bool     arrow;
        } member;

        /* ── SIZEOF expression ── */
        struct {
            ASTNode *expr;
            Type    *of_type;
            bool     is_type;
        } szof;

        /* ── CAST expression ── */
        struct {
            Type    *to;
            ASTNode *expr;
        } cast;

        /* ── ADDR / DEREF ── (reuses unary.operand) */

        /* ── EXPR statement ── */
        struct { ASTNode *expr; } expr_stmt;

        /* ── BLOCK ── */
        struct {
            ASTNode **stmts;
            uint32_t  n_stmts;
            uint32_t  cap;
        } block;

        /* ── RETURN statement ── */
        struct { ASTNode *value; } ret;

        /* ── IF statement ── */
        struct {
            ASTNode *cond;
            ASTNode *then_br;
            ASTNode *else_br;
        } if_stmt;

        /* ── WHILE statement ── */
        struct {
            ASTNode *cond;
            ASTNode *body;
        } while_stmt;

        /* ── FOR statement ── */
        struct {
            ASTNode *init;
            ASTNode *cond;
            ASTNode *step;
            ASTNode *body;
        } for_stmt;

        /* ── DO-WHILE statement ── */
        struct {
            ASTNode *body;
            ASTNode *cond;
        } do_while;

        /* ── VAR / PARAM declaration ── */
        struct {
            char    *name;
            Type    *decl_type;
            ASTNode *init;
            bool     is_static;
            bool     is_extern;
        } var_decl;

        /* ── FUNCTION decl / def ── */
        struct {
            char     *name;
            Type     *ret_type;
            ParamList params;
            ASTNode  *body;     /* NULL = declaration only */
            bool      is_static;
            bool      is_inline;
        } func;

        /* ── TYPEDEF declaration ── */
        struct {
            char *alias;
            Type *of;
        } typedef_decl;

        /* ── TRANSLATION UNIT ── */
        struct {
            ASTNode **decls;
            uint32_t  n_decls;
            uint32_t  cap;
        } tu;
    };
};

/* ═══════════════════════════════════════════════════════════════
   ARENA ALLOCATOR  (Bump-Pointer, O(1) free)
   ═══════════════════════════════════════════════════════════════ */

#define ARENA_BLOCK_SIZE  (65536u)

typedef struct ASTBlock ASTBlock;
struct ASTBlock {
    uint8_t  *data;
    size_t    used;
    size_t    cap;
    ASTBlock *next;
};

typedef struct {
    /* Node bump-pointer chain */
    ASTBlock *node_cur;
    ASTBlock *node_first;
    uint32_t  count;

    /* Type bump-pointer chain */
    ASTBlock *type_cur;
    ASTBlock *type_first;
    uint32_t  type_count;

    /* String intern buffer (already linear — kept as-is) */
    char     *str_buf;
    size_t    str_pos;
    size_t    str_cap;
} ASTArena;

/* ── Lifecycle ── */
ASTArena* ast_arena_new (void);
void      ast_arena_free(ASTArena *a);

/* ── Allocation ── */
ASTNode* ast_node_new (ASTArena *a, NodeKind kind, uint32_t line, uint32_t col);
Type*    ast_type_new (ASTArena *a, TypeKind kind);
char*    ast_intern   (ASTArena *a, const char *s, size_t len);

/* ── Type predicates ── */
bool     type_is_integer (const Type *t);
bool     type_is_float   (const Type *t);
bool     type_is_numeric (const Type *t);
bool     type_is_pointer (const Type *t);
bool     type_compatible (const Type *a, const Type *b);
uint32_t type_size       (const Type *t);
const char* type_name    (const Type *t);

/* ── Auto-Header Generator ── */
char* ast_generate_header(ASTNode *tu, const char *filename);

#endif /* AST_H */
