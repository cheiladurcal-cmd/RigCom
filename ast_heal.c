/* ============================================================
   RigCom v8.0 — src/ast_heal.c
   Self-Healing AST: Oracle Auto-Fix
   Lee diagnósticos del Oracle (leaks, null-risks) y:
   1. Genera nodos NODE_CALL_EXPR para free() en el AST
   2. Reescribe el .c con los parches insertados
   3. Emite diff de parches via WebSocket
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/ast_heal.h"
#include "../include/oracle_ip.h"
#include "../include/wsserver.h"
#include "../include/rigctx.h"
#include "../include/error.h"
#include "../include/preproc.h"
#include "../include/lexer.h"
#include "../include/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ── Utilidades de texto ── */

/* Leer archivo completo en buffer heap-allocated */
static char* read_file_alloc(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 2);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\n'; buf[rd+1] = '\0';
    fclose(f);
    if (out_len) *out_len = rd;
    return buf;
}

/* Añadir parche a la lista */
static void add_patch(HealResult *r, const char *file,
                      uint32_t line, const char *text,
                      const char *reason) {
    HealPatch *p = calloc(1, sizeof(HealPatch));
    if (!p) return;
    strncpy(p->file,       file,   sizeof(p->file)-1);
    strncpy(p->patch_text, text,   sizeof(p->patch_text)-1);
    strncpy(p->reason,     reason, sizeof(p->reason)-1);
    p->insert_line = line;
    p->next        = r->patches;
    r->patches     = p;
    r->n_patches++;
}

/* ── Búsqueda de la línea de cierre de función ── */

/* Dado el texto fuente y la línea de inicio de una función,
   localiza la llave } de cierre contando pares {}.
   Devuelve el número de línea (1-indexed) de la } de cierre.
   Si no encuentra, devuelve start_line + 50 como fallback. */
static uint32_t find_fn_close_line(const char *src,
                                    uint32_t start_line) {
    uint32_t cur_line = 1;
    int depth = 0;
    bool in_string = false;
    bool in_char   = false;
    bool in_lcomm  = false; /* // */
    bool in_bcomm  = false; /* block */

    for (const char *p = src; *p; p++) {
        if (*p == '\n') {
            cur_line++;
            in_lcomm = false;
            continue;
        }
        if (cur_line < start_line) continue;

        /* Comentarios */
        if (!in_string && !in_char && !in_bcomm && !in_lcomm) {
            if (*p == '/' && *(p+1) == '/') { in_lcomm = true; continue; }
            if (*p == '/' && *(p+1) == '*') { in_bcomm = true; p++; continue; }
        }
        if (in_bcomm) {
            if (*p == '*' && *(p+1) == '/') { in_bcomm = false; p++; }
            continue;
        }
        if (in_lcomm) continue;

        /* Strings / chars */
        if (!in_char && *p == '"' && (p == src || *(p-1) != '\\'))
            in_string = !in_string;
        if (!in_string && *p == '\'' && (p == src || *(p-1) != '\\'))
            in_char = !in_char;
        if (in_string || in_char) continue;

        if (*p == '{') depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0 && cur_line >= start_line)
                return cur_line;
        }
    }
    return start_line + 50;
}

/* ── Generación de parches desde Oracle diagnósticos ── */

HealResult* ast_heal_generate(OracleIPSession *oracle,
                               const char *src_file,
                               ASTArena   *arena) {
    (void)arena; /* Reservado para mutación AST futura */

    HealResult *r = calloc(1, sizeof(HealResult));
    if (!r) return NULL;

    /* Leer fuente para localizar cierres de función */
    size_t src_len = 0;
    char *src = read_file_alloc(src_file, &src_len);

    for (OracleFnDiag *fn = oracle->fns; fn; fn = fn->next) {
        /* Solo procesar funciones del archivo objetivo */
        if (strcmp(fn->file, src_file) != 0) continue;

        for (OraclePtr *ptr = fn->ptrs; ptr; ptr = ptr->next) {
            /* ── Fix 1: Memory leak — ptr sin free() ── */
            if (!ptr->freed && ptr->alloc_line > 0) {
                /* Insertar free(var) antes del } de cierre */
                uint32_t close_line = src
                    ? find_fn_close_line(src, fn->line)
                    : fn->line + 20;

                char patch[512], reason[256];
                snprintf(patch,  sizeof(patch),
                         "    free(%s); /* HEAL: leak detectado en línea %u */",
                         ptr->name, ptr->alloc_line);
                snprintf(reason, sizeof(reason),
                         "Leak: '%s' asignado en línea %u sin free() en %s()",
                         ptr->name, ptr->alloc_line, fn->fn_name);
                add_patch(r, src_file, close_line, patch, reason);
            }

            /* ── Fix 2: Null-risk — ptr usado sin null-check ── */
            if (!ptr->null_checked && ptr->alloc_line > 0) {
                /* Insertar if(!ptr) return NULL; justo después del malloc */
                char patch[512], reason[256];
                snprintf(patch, sizeof(patch),
                         "    if (!%s) return NULL; /* HEAL: null-check */",
                         ptr->name);
                snprintf(reason, sizeof(reason),
                         "Null-risk: '%s' en %s() sin comprobación de NULL",
                         ptr->name, fn->fn_name);
                add_patch(r, src_file, ptr->alloc_line + 1, patch, reason);
            }
        }
    }

    free(src);
    strncpy(r->patched_file, src_file, sizeof(r->patched_file)-1);
    return r;
}

/* ── Aplicación de parches al texto fuente ── */

bool ast_heal_apply(HealResult *r,
                     const char *src_file,
                     const char *out_file) {
    if (!r || r->n_patches == 0) return true;

    size_t src_len = 0;
    char *src = read_file_alloc(src_file, &src_len);
    if (!src) return false;

    /* Ordenar parches por línea descendente (insertar desde abajo) */
    /* Insertion sort simple — n_patches generalmente < 32 */
    HealPatch *sorted[256]; uint32_t n = 0;
    for (HealPatch *p = r->patches; p && n < 256; p = p->next)
        sorted[n++] = p;
    /* bubble sort desc por línea */
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = i+1; j < n; j++)
            if (sorted[j]->insert_line > sorted[i]->insert_line) {
                HealPatch *tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp;
            }

    /* Construir array de líneas del fuente */
    /* Máx 65536 líneas */
    char *lines[65536]; uint32_t n_lines = 0;
    char *cursor = src;
    while (*cursor && n_lines < 65535) {
        lines[n_lines++] = cursor;
        char *nl = strchr(cursor, '\n');
        if (!nl) break;
        cursor = nl + 1;
    }

    /* Construir buffer de salida con parches insertados */
    size_t out_sz = src_len + (size_t)n * 512 + 1024;
    char *out = malloc(out_sz);
    if (!out) { free(src); return false; }
    out[0] = '\0';
    size_t wp = 0;

    for (uint32_t li = 0; li < n_lines; li++) {
        uint32_t line_no = li + 1; /* 1-indexed */

        /* Calcular longitud de esta línea */
        size_t ll = strlen(lines[li]);
        char *nl = strchr(lines[li], '\n');
        if (nl) ll = (size_t)(nl - lines[li]) + 1;

        if (wp + ll < out_sz) {
            memcpy(out + wp, lines[li], ll);
            wp += ll;
        }

        /* Insertar parches que van DESPUÉS de esta línea */
        for (uint32_t pi = 0; pi < n; pi++) {
            if (sorted[pi]->insert_line == line_no &&
                !sorted[pi]->applied) {
                size_t pl = strlen(sorted[pi]->patch_text);
                if (wp + pl + 2 < out_sz) {
                    memcpy(out + wp, sorted[pi]->patch_text, pl);
                    wp += pl;
                    out[wp++] = '\n';
                }
                sorted[pi]->applied = true;
                r->n_applied++;
            }
        }
    }
    out[wp] = '\0';

    /* Determinar archivo de salida */
    const char *dst = out_file ? out_file : src_file;

    /* Backup si sobreescribimos el original */
    if (!out_file) {
        char bak[520];
        snprintf(bak, sizeof(bak), "%s.bak", src_file);
        rename(src_file, bak);
    }

    FILE *f = fopen(dst, "w");
    bool ok = false;
    if (f) {
        ok = (fwrite(out, 1, wp, f) == wp);
        fclose(f);
    }

    strncpy(r->patched_file, dst, sizeof(r->patched_file)-1);
    free(src); free(out);
    return ok;
}

/* ── Emisión WebSocket ── */

void ast_heal_emit(HealResult *r, WsServer *srv,
                    const char *src_file) {
    if (!r || !srv) return;

    /* Emitir resumen */
    ws_broadcastf(srv,
        "{\"ev\":\"heal_summary\","
         "\"file\":\"%s\","
         "\"n_patches\":%u,"
         "\"n_applied\":%u,"
         "\"patched_file\":\"%s\"}",
        src_file, r->n_patches, r->n_applied, r->patched_file);

    /* Emitir cada parche individual */
    for (HealPatch *p = r->patches; p; p = p->next) {
        /* Escapar strings para JSON */
        char esc_patch[1024] = {0};
        char esc_reason[512] = {0};
        size_t ei = 0;
        for (const char *s = p->patch_text; *s && ei < 1020; s++) {
            if (*s == '"')       { esc_patch[ei++] = '\\'; esc_patch[ei++] = '"'; }
            else if (*s == '\\') { esc_patch[ei++] = '\\'; esc_patch[ei++] = '\\'; }
            else                 { esc_patch[ei++] = *s; }
        }
        ei = 0;
        for (const char *s = p->reason; *s && ei < 508; s++) {
            if (*s == '"')       { esc_reason[ei++] = '\\'; esc_reason[ei++] = '"'; }
            else if (*s == '\\') { esc_reason[ei++] = '\\'; esc_reason[ei++] = '\\'; }
            else                 { esc_reason[ei++] = *s; }
        }
        ws_broadcastf(srv,
            "{\"ev\":\"heal_patch\","
             "\"file\":\"%s\","
             "\"line\":%u,"
             "\"patch\":\"%s\","
             "\"reason\":\"%s\","
             "\"applied\":%s}",
            p->file, p->insert_line,
            esc_patch, esc_reason,
            p->applied ? "true" : "false");
    }
}

void ast_heal_free(HealResult *r) {
    if (!r) return;
    HealPatch *p = r->patches;
    while (p) { HealPatch *nx = p->next; free(p); p = nx; }
    free(r);
}

/* ── Thread principal ── */

void* ast_heal_thread(void *arg) {
    HealArg *ha  = (HealArg *)arg;
    WsServer *srv = ha->srv;

    ws_broadcastf(srv,
        "{\"ev\":\"heal_start\",\"file\":\"%s\"}", ha->file);

    /* 1. Leer archivo fuente */
    size_t raw_len = 0;
    char  *raw     = read_file_alloc(ha->file, &raw_len);
    if (!raw) {
        ws_broadcastf(srv,
            "{\"ev\":\"heal_error\",\"msg\":\"No se pudo leer %s\"}",
            ha->file);
        free(ha); return NULL;
    }

    /* 2. Preprocesar */
    RigCtx     *ctx    = rigctx_new(".");
    RigErrorLog errlog = {0};
    Preproc     pp;
    preproc_init(&pp, ctx, &errlog);
    preproc_add_path(&pp, ".");
    preproc_add_path(&pp, "include");

    char *src = preproc_run(&pp, raw, raw_len, ha->file);
    free(raw);
    if (!src) {
        ws_broadcastf(srv,
            "{\"ev\":\"heal_error\",\"msg\":\"Preproc fallido en %s\"}",
            ha->file);
        preproc_free(&pp); rigctx_free(ctx); free(ha);
        return NULL;
    }

    /* 3. Tokenizar */
    Lexer lx;
    lexer_init(&lx, src, strlen(src), ha->file);

    /* 4. Parsear */
    ASTArena *arena = ast_arena_new();
    Parser    parser;
    parser_init(&parser, &lx, &errlog, arena, ha->file);
    ASTNode  *tu = parser_parse(&parser);

    if (!tu || errlog.count > 0) {
        ws_broadcastf(srv,
            "{\"ev\":\"heal_error\",\"msg\":\"Parse fallido en %s (%u errores)\"}",
            ha->file, errlog.count);
        ast_arena_free(arena); preproc_free(&pp);
        rigctx_free(ctx); free(src); free(ha);
        return NULL;
    }

    /* 5. Correr Oracle */
    OracleIPSession *oracle = oracle_ip_analyze_ast(tu, ha->file);
    if (!oracle) {
        ws_broadcastf(srv,
            "{\"ev\":\"heal_error\",\"msg\":\"Oracle falló en %s\"}",
            ha->file);
        ast_arena_free(arena); preproc_free(&pp);
        rigctx_free(ctx); free(src); free(ha);
        return NULL;
    }

    if (oracle->total_leaks == 0 && oracle->total_null_risks == 0) {
        ws_broadcastf(srv,
            "{\"ev\":\"heal_clean\","
             "\"file\":\"%s\","
             "\"msg\":\"Sin problemas detectados. Código limpio.\"}",
            ha->file);
        oracle_ip_free(oracle);
        ast_arena_free(arena); preproc_free(&pp);
        rigctx_free(ctx); free(src); free(ha);
        return NULL;
    }

    /* 6. Generar parches */
    HealResult *result = ast_heal_generate(oracle, ha->file, arena);

    /* 7. Aplicar si se pidió */
    if (ha->apply && result && result->n_patches > 0)
        ast_heal_apply(result, ha->file, NULL);

    /* 8. Emitir */
    ast_heal_emit(result, srv, ha->file);

    ast_heal_free(result);
    oracle_ip_free(oracle);
    ast_arena_free(arena);
    preproc_free(&pp);
    rigctx_free(ctx);
    free(src);
    free(ha);
    return NULL;
}
