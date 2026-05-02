/* ============================================================
   RigCom v8.0 — include/ast_heal.h
   Self-Healing AST: Oracle Auto-Fix
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef AST_HEAL_H
#define AST_HEAL_H

#include <stdbool.h>
#include <stdint.h>
#include "oracle_ip.h"
#include "wsserver.h"
#include "ast.h"

typedef struct HealPatch {
    char     file[256];
    uint32_t insert_line;
    char     patch_text[512];
    char     reason[256];
    bool     applied;
    struct HealPatch *next;
} HealPatch;

typedef struct {
    HealPatch *patches;
    int        n_patches;
    int        n_applied;
    int        n_failed;
    char       patched_file[256];
} HealResult;

HealResult* ast_heal_generate(OracleIPSession *oracle,
                               const char *src_file,
                               ASTArena   *arena);
bool        ast_heal_apply   (HealResult *r,
                               const char *src_file,
                               const char *out_file);
void        ast_heal_emit    (HealResult *r, WsServer *srv,
                               const char *src_file);
void        ast_heal_free    (HealResult *r);

typedef struct { WsServer *srv; char file[512]; bool apply; } HealArg;
void* ast_heal_thread(void *arg);

#endif /* AST_HEAL_H */
