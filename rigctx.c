#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — src/rigctx.c
   Global compiler context: lifecycle, memory pool, config
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/rigctx.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

/* ── Lifecycle ──────────────────────────────────────────────── */
RigCtx* rigctx_new(const char *project_root) {
    RigCtx *ctx = calloc(1, sizeof(RigCtx));
    if (!ctx) return NULL;

    ctx->project_root = project_root ? strdup(project_root) : strdup(".");

    /* Memory pool init */
    ctx->mem.capacity = 256;
    ctx->mem.ptrs     = malloc(ctx->mem.capacity * sizeof(void *));
    ctx->mem.count    = 0;

    /* Build ID: timestamp-based */
    ctx->build_id = (uint64_t)time(NULL);

    return ctx;
}

void rigctx_free(RigCtx *ctx) {
    if (!ctx) return;
    rigctx_free_all(ctx);
    free(ctx->mem.ptrs);
    free(ctx->project_root);
    /* Config strings freed by alloc pool */
    free(ctx);
}

/* ── Allocation (tracked, freed on rigctx_free) ─────────────── */
void* rigctx_alloc(RigCtx *ctx, size_t size) {
    if (!ctx) return NULL;
    void *p = calloc(1, size);
    if (!p) return NULL;

    if (ctx->mem.count >= ctx->mem.capacity) {
        ctx->mem.capacity *= 2;
        void **tmp = realloc(ctx->mem.ptrs,
                             ctx->mem.capacity * sizeof(void *));
        if (!tmp) { free(p); return NULL; }
        ctx->mem.ptrs = tmp;
    }
    ctx->mem.ptrs[ctx->mem.count++] = p;
    return p;
}

char* rigctx_strdup(RigCtx *ctx, const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char  *d = rigctx_alloc(ctx, n);
    if (d) memcpy(d, s, n);
    return d;
}

void rigctx_free_all(RigCtx *ctx) {
    if (!ctx) return;
    for (uint32_t i = 0; i < ctx->mem.count; i++)
        free(ctx->mem.ptrs[i]);
    ctx->mem.count = 0;
}

/* ── Attach WebSocket bridge ────────────────────────────────── */
void rigctx_attach_ws_bridge(RigCtx *ctx, void *bridge) {
    if (ctx) ctx->ws = bridge;
}

/* ── Load config from TOML ──────────────────────────────────── */
bool rigctx_load_config(RigCtx *ctx, const char *toml_path) {
    if (!ctx || !toml_path) return false;

    if (!toml_parse_file(&ctx->toml, toml_path)) {
        fprintf(stderr, "[rigctx] Error al cargar config: %s\n",
                ctx->toml.errbuf);
        return false;
    }

    RigConfig *cfg = &ctx->config;

    /* Project metadata */
    const char *pname = toml_get_str(&ctx->toml, "project", "name",
                                      "rigcom-project");
    cfg->project_name    = rigctx_strdup(ctx, pname);
    const char *pver     = toml_get_str(&ctx->toml, "project", "version",
                                         "1.0.0");
    cfg->project_version = rigctx_strdup(ctx, pver);

    /* Build settings */
    const char *target   = toml_get_str(&ctx->toml, "build", "target",
                                         "aarch64-linux-android");
    cfg->target          = rigctx_strdup(ctx, target);

    const char *opt      = toml_get_str(&ctx->toml, "build", "optimize", "O2");
    cfg->optimize        = rigctx_strdup(ctx, opt);

    const char *out      = toml_get_str(&ctx->toml, "build", "output",
                                         "rigcom-out");
    cfg->output_exec     = rigctx_strdup(ctx, out);

    cfg->use_native_backend = toml_get_bool(&ctx->toml, "build",
                                             "native_backend", false);
    cfg->n_cores         = (uint32_t)toml_get_int(&ctx->toml, "build",
                                                    "cores", 4);
    cfg->enable_bootstrap = toml_get_bool(&ctx->toml, "build",
                                           "bootstrap", false);
    cfg->enable_bench    = toml_get_bool(&ctx->toml, "build",
                                          "bench", false);

    /* Sources */
    uint32_t ns = toml_get_array_len(&ctx->toml, "build", "sources");
    if (ns == 0) {
        /* Default: all .c in src/ */
        cfg->sources   = rigctx_alloc(ctx, sizeof(char *));
        cfg->sources[0] = rigctx_strdup(ctx, "src/*.c");
        cfg->n_sources  = 1;
    } else {
        cfg->sources   = rigctx_alloc(ctx, ns * sizeof(char *));
        cfg->n_sources  = ns;
        for (uint32_t i = 0; i < ns; i++) {
            const char *s = toml_get_array_elem(&ctx->toml, "build",
                                                  "sources", i);
            cfg->sources[i] = rigctx_strdup(ctx, s);
        }
    }

    /* Defines */
    uint32_t nd = toml_get_array_len(&ctx->toml, "build", "defines");
    cfg->defines   = rigctx_alloc(ctx, (nd + 1) * sizeof(char *));
    cfg->n_defines  = nd;
    for (uint32_t i = 0; i < nd; i++) {
        const char *d = toml_get_array_elem(&ctx->toml, "build",
                                              "defines", i);
        cfg->defines[i] = rigctx_strdup(ctx, d);
    }

    /* Include dirs */
    uint32_t ni = toml_get_array_len(&ctx->toml, "build", "include_dirs");
    cfg->include_dirs   = rigctx_alloc(ctx, (ni + 1) * sizeof(char *));
    cfg->n_include_dirs  = ni;
    for (uint32_t i = 0; i < ni; i++) {
        const char *d = toml_get_array_elem(&ctx->toml, "build",
                                              "include_dirs", i);
        cfg->include_dirs[i] = rigctx_strdup(ctx, d);
    }

    return true;
}

/* ── Source file discovery ──────────────────────────────────── */
// Expands src/*.c glob patterns into concrete file paths
uint32_t rigctx_find_sources(RigCtx *ctx, char ***out_paths) {
    if (!ctx || !out_paths) return 0;

    uint32_t  cap   = 256;
    uint32_t  count = 0;
    char    **paths = malloc(cap * sizeof(char *));
    if (!paths) { *out_paths = NULL; return 0; }

    RigConfig *cfg = &ctx->config;

    for (uint32_t si = 0; si < cfg->n_sources; si++) {
        const char *pat = cfg->sources[si];

        /* Find last '/' to split dir / glob */
        const char *slash = strrchr(pat, '/');
        char dir_buf[512];
        const char *glob_pat;

        if (slash) {
            size_t dlen = (size_t)(slash - pat);
            if (dlen >= sizeof(dir_buf) - 1) dlen = sizeof(dir_buf) - 2;
            memcpy(dir_buf, pat, dlen);
            dir_buf[dlen] = '\0';
            glob_pat = slash + 1;
        } else {
            strcpy(dir_buf, ".");
            glob_pat = pat;
        }

        /* Find suffix: e.g., "*.c" → ".c" */
        const char *suffix = NULL;
        if (glob_pat[0] == '*') {
            suffix = glob_pat + 1; /* e.g., ".c" */
        }

        DIR *d = opendir(dir_buf);
        if (!d) {
            /* Treat as literal path */
            if (count < cap) {
                paths[count++] = strdup(pat);
            }
            continue;
        }

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            const char *name = ent->d_name;
            if (name[0] == '.') continue;

            bool match = false;
            if (!suffix) {
                match = (strcmp(name, glob_pat) == 0);
            } else {
                size_t nlen = strlen(name);
                size_t slen = strlen(suffix);
                if (nlen >= slen &&
                    strcmp(name + nlen - slen, suffix) == 0)
                    match = true;
            }

            if (match) {
                char full[1024];
                snprintf(full, sizeof(full), "%s/%s", dir_buf, name);

                /* Verify it's a regular file */
                struct stat st;
                if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
                    continue;

                if (count >= cap) {
                    cap *= 2;
                    char **tmp = realloc(paths, cap * sizeof(char *));
                    if (!tmp) { closedir(d); goto done; }
                    paths = tmp;
                }
                paths[count++] = strdup(full);
            }
        }
        closedir(d);
    }

done:
    *out_paths = paths;
    return count;
}

/* ── WebSocket bridge (attach & emit) ───────────────────────── */
void rigctx_attach_ws(RigCtx *ctx, struct WsServer *ws) {
    if (ctx) ctx->ws = ws;
}

void rigctx_ws_emit(RigCtx *ctx, const char *json_event) {
    if (!ctx || !ctx->ws || !json_event) return;
    extern void ws_broadcast(struct WsServer *srv, const char *text, size_t len);
    ws_broadcast(ctx->ws, json_event, strlen(json_event));
}

void rigctx_ws_phase(RigCtx *ctx, const char *phase,
                      const char *file, uint32_t file_idx) {
    if (!ctx) return;
    snprintf(ctx->current_phase, sizeof(ctx->current_phase), "%s", phase);
    if (!ctx->ws) return;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"ev\":\"phase\",\"phase\":\"%s\",\"file\":\"%s\","
             "\"idx\":%u,\"total\":%u}",
             phase, file ? file : "", file_idx, ctx->files_total);
    rigctx_ws_emit(ctx, buf);
}

void rigctx_ws_emit_errors(RigCtx *ctx, RigErrorLog *log) {
    if (!ctx || !ctx->ws || !log) return;
    /* riglog_to_json declared in error.h */;
    char *json = riglog_to_json(log);
    if (!json) return;
    char *buf = malloc(strlen(json) + 64);
    if (buf) {
        sprintf(buf, "{\"ev\":\"errors\",\"errors\":%s}", json);
        rigctx_ws_emit(ctx, buf);
        free(buf);
    }
    free(json);
}
