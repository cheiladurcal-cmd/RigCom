#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — src/preproc.c
   Autonomous C preprocessor — handles:
     · #define / #undef  (object-like + function-like)
     · #include <sys> and "local"  
     · #if / #ifdef / #ifndef / #elif / #else / #endif
     · __FILE__ / __LINE__ / __DATE__ / defined()
   ============================================================ */
#include "../include/preproc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

/* ── Output buffer (dynamic) ────────────────────────────────── */
typedef struct { char *data; size_t n; size_t cap; } Buf;

static void buf_push_char(Buf *b, char c) {
    if (b->n + 1 >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4096;
        b->data = realloc(b->data, b->cap);
    }
    b->data[b->n++] = c;
}
static void buf_push_str(Buf *b, const char *s) {
    while (*s) buf_push_char(b, *s++);
}
static void buf_push_strn(Buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) buf_push_char(b, s[i]);
}

/* ── String helpers ─────────────────────────────────────────── */
static char* strndup_safe(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (d) { memcpy(d, s, n); d[n] = '\0'; }
    return d;
}

static bool startswith(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static const char* skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static size_t ident_len(const char *s) {
    size_t n = 0;
    while (isalnum((unsigned char)s[n]) || s[n] == '_') n++;
    return n;
}

/* ── Macro lookup ───────────────────────────────────────────── */
static Macro* macro_find(Preproc *pp, const char *name, size_t len) {
    for (uint32_t i = 0; i < pp->n_macros; i++) {
        if (strlen(pp->macros[i].name) == len &&
            memcmp(pp->macros[i].name, name, len) == 0)
            return &pp->macros[i];
    }
    return NULL;
}

static void macro_define(Preproc *pp, const char *name, const char *body) {
    /* Override if exists */
    size_t nlen = strlen(name);
    Macro *existing = macro_find(pp, name, nlen);
    if (existing) { free(existing->body); existing->body = strdup(body); return; }
    if (pp->n_macros >= 4096) return;
    Macro *m = &pp->macros[pp->n_macros++];
    m->name       = strdup(name);
    m->body       = strdup(body);
    m->params     = NULL;
    m->n_params   = 0;
    m->func_like  = false;
}

static void macro_undef(Preproc *pp, const char *name) {
    size_t nlen = strlen(name);
    for (uint32_t i = 0; i < pp->n_macros; i++) {
        if (strlen(pp->macros[i].name) == nlen &&
            memcmp(pp->macros[i].name, name, nlen) == 0) {
            free(pp->macros[i].name);
            free(pp->macros[i].body);
            pp->macros[i] = pp->macros[--pp->n_macros];
            return;
        }
    }
}

/* ── Expand macros in a string ──────────────────────────────── */
static void expand_text(Preproc *pp, const char *src, Buf *out,
                         const char *file, uint32_t line);

static void expand_text(Preproc *pp, const char *src, Buf *out,
                         const char *file, uint32_t line) {
    const char *p = src;
    while (*p) {
        /* Strings: don't expand inside them */
        if (*p == '"') {
            buf_push_char(out, *p++);
            while (*p && *p != '"') {
                if (*p == '\\') buf_push_char(out, *p++);
                buf_push_char(out, *p++);
            }
            if (*p == '"') buf_push_char(out, *p++);
            continue;
        }
        /* Char literals */
        if (*p == '\'') {
            buf_push_char(out, *p++);
            while (*p && *p != '\'') {
                if (*p == '\\') buf_push_char(out, *p++);
                buf_push_char(out, *p++);
            }
            if (*p == '\'') buf_push_char(out, *p++);
            continue;
        }
        /* Identifier: check if macro */
        if (isalpha((unsigned char)*p) || *p == '_') {
            size_t n = ident_len(p);
            /* Builtin macros */
            if (n == 8 && memcmp(p,"__FILE__",8)==0) {
                buf_push_char(out,'"');
                buf_push_str(out, file);
                buf_push_char(out,'"');
                p += n; continue;
            }
            if (n == 8 && memcmp(p,"__LINE__",8)==0) {
                char tmp[16]; snprintf(tmp,sizeof(tmp),"%u",line);
                buf_push_str(out, tmp);
                p += n; continue;
            }
            if (n == 8 && memcmp(p,"__DATE__",8)==0) {
                time_t t = time(NULL);
                char tmp[32]; strftime(tmp,sizeof(tmp),"\"%b %d %Y\"",localtime(&t));
                buf_push_str(out, tmp);
                p += n; continue;
            }
            Macro *m = macro_find(pp, p, n);
            if (m && !m->func_like) {
                /* Object-like: expand body */
                expand_text(pp, m->body, out, file, line);
                p += n; continue;
            }
            if (m && m->func_like) {
                /* Function-like: consume args */
                p += n;
                if (*p == '(') {
                    p++;
                    /* Collect args */
                    char *args[64]; uint32_t n_args = 0;
                    int depth = 1;
                    const char *arg_start = p;
                    while (*p && depth > 0) {
                        if (*p == '(') depth++;
                        else if (*p == ')') { if (--depth == 0) break; }
                        else if (*p == ',' && depth == 1) {
                            args[n_args++] = strndup_safe(arg_start, p - arg_start);
                            arg_start = p + 1;
                        }
                        p++;
                    }
                    if (p > arg_start || n_args < m->n_params)
                        args[n_args++] = strndup_safe(arg_start, p - arg_start);
                    if (*p == ')') p++;
                    /* Substitute params in body */
                    char *body = strdup(m->body);
                    Buf expanded = {0};
                    /* Simple textual substitution */
                    const char *bp = body;
                    while (*bp) {
                        if (isalpha((unsigned char)*bp) || *bp == '_') {
                            size_t bn = ident_len(bp);
                            bool replaced = false;
                            for (uint32_t ai = 0; ai < m->n_params && ai < n_args; ai++) {
                                if (strlen(m->params[ai]) == bn &&
                                    memcmp(m->params[ai], bp, bn) == 0) {
                                    buf_push_str(&expanded, args[ai]);
                                    bp += bn; replaced = true; break;
                                }
                            }
                            if (!replaced) {
                                buf_push_strn(&expanded, bp, bn); bp += bn;
                            }
                        } else {
                            buf_push_char(&expanded, *bp++);
                        }
                    }
                    buf_push_char(&expanded, '\0');
                    expand_text(pp, expanded.data, out, file, line);
                    free(expanded.data); free(body);
                    for (uint32_t ai = 0; ai < n_args; ai++) free(args[ai]);
                    continue;
                }
            }
            /* Not a macro */
            buf_push_strn(out, p, n); p += n;
            continue;
        }
        buf_push_char(out, *p++);
    }
}

/* ── Evaluate simple #if constant expression ────────────────── */
static bool eval_cond(Preproc *pp, const char *expr) {
    expr = skip_ws(expr);
    /* defined(NAME) or defined NAME */
    if (startswith(expr, "defined")) {
        const char *p = expr + 7;
        p = skip_ws(p);
        bool paren = (*p == '(');
        if (paren) p++;
        p = skip_ws(p);
        size_t n = ident_len(p);
        bool found = macro_find(pp, p, n) != NULL;
        return found;
    }
    /* Expand macros then evaluate numeric */
    Buf expanded = {0};
    expand_text(pp, expr, &expanded, "<cond>", 0);
    buf_push_char(&expanded, '\0');
    long val = strtol(expanded.data, NULL, 0);
    free(expanded.data);
    return val != 0;
}

/* ── Read file ──────────────────────────────────────────────── */
static char* read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    (void)fread(buf, 1, sz, f); buf[sz] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

/* ── Resolve #include path ──────────────────────────────────── */
static char* resolve_include(Preproc *pp, const char *name,
                              bool system, const char *cur_file) {
    char path[4096];
    /* Local include first (non-system) */
    if (!system) {
        /* Same directory as current file */
        const char *slash = strrchr(cur_file, '/');
        if (slash) {
            size_t dir_len = slash - cur_file + 1;
            snprintf(path, sizeof(path), "%.*s%s", (int)dir_len, cur_file, name);
            if (fopen(path, "rb")) return strdup(path);
        }
    }
    for (uint32_t i = 0; i < pp->n_paths; i++) {
        snprintf(path, sizeof(path), "%s/%s", pp->include_paths[i], name);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); return strdup(path); }
    }
    return NULL;
}

/* ── Main processing function (recursive for #include) ───────── */
static void process(Preproc *pp, const char *src, size_t len,
                    const char *filename, Buf *out, int depth) {
    if (depth > 32) return; /* #include depth guard */

    const char *p   = src;
    const char *end = src + len;
    uint32_t    line = 1;

    while (p < end) {
        /* Track line number */
        if (*p == '\n') { line++; p++; buf_push_char(out, '\n'); continue; }

        /* Are we in a false conditional? Skip lines */
        bool in_true_branch = (pp->cond_depth == 0) ||
                               pp->cond_true[pp->cond_depth - 1];

        /* Skip whitespace before # */
        const char *line_start = p;
        while (p < end && (*p == ' ' || *p == '\t')) p++;

        if (*p == '#') {
            p++;
            const char *dir_start = skip_ws(p);
            const char *dir_end   = dir_start;
            while (dir_end < end && isalpha((unsigned char)*dir_end)) dir_end++;
            size_t dir_len = dir_end - dir_start;
            p = skip_ws(dir_end);
            /* Collect rest of line */
            const char *arg_start = p;
            while (p < end && *p != '\n') p++;
            char *arg = strndup_safe(arg_start, p - arg_start);

            /* Remove trailing whitespace from arg */
            size_t alen = strlen(arg);
            while (alen > 0 && (arg[alen-1]==' '||arg[alen-1]=='\t')) arg[--alen]='\0';

#define DIRECTIVE(s) (dir_len==strlen(s) && memcmp(dir_start,s,dir_len)==0)

            if (DIRECTIVE("define") && in_true_branch) {
                const char *n = skip_ws(arg);
                size_t nlen = ident_len(n);
                char name[256]; memcpy(name, n, nlen); name[nlen] = '\0';
                const char *body = n + nlen;
                bool func_like = (*body == '(');
                Macro *m = NULL;
                if (func_like) {
                    /* Parse param list */
                    body++;
                    char **params = malloc(32 * sizeof(char*));
                    uint32_t np = 0;
                    while (*body && *body != ')') {
                        body = skip_ws(body);
                        size_t pn = ident_len(body);
                        if (pn) { params[np++] = strndup_safe(body, pn); body += pn; }
                        body = skip_ws(body);
                        if (*body == ',') body++;
                    }
                    if (*body == ')') body++;
                    body = skip_ws(body);
                    macro_define(pp, name, body);
                    m = macro_find(pp, name, strlen(name));
                    if (m) { m->func_like=true; m->params=params; m->n_params=np; }
                } else {
                    body = skip_ws(body);
                    macro_define(pp, name, body);
                }
            } else if (DIRECTIVE("undef") && in_true_branch) {
                const char *n = skip_ws(arg);
                size_t nlen = ident_len(n);
                char name[256]; memcpy(name, n, nlen); name[nlen] = '\0';
                macro_undef(pp, name);
            } else if (DIRECTIVE("include") && in_true_branch) {
                const char *a = skip_ws(arg);
                bool sys = (*a == '<');
                a++;
                const char *end_q = strchr(a, sys ? '>' : '"');
                if (end_q) {
                    char *inc_name = strndup_safe(a, end_q - a);
                    char *path     = resolve_include(pp, inc_name, sys, filename);
                    if (path) {
                        size_t flen = 0;
                        char *fsrc  = read_file(path, &flen);
                        if (fsrc) {
                            /* Emit line marker */
                            char marker[256];
                            snprintf(marker, sizeof(marker),
                                     "\n# 1 \"%s\"\n", path);
                            buf_push_str(out, marker);
                            process(pp, fsrc, flen, path, out, depth + 1);
                            snprintf(marker, sizeof(marker),
                                     "\n# %u \"%s\"\n", line, filename);
                            buf_push_str(out, marker);
                            free(fsrc);
                        }
                        free(path);
                    } else {
                        /* Emit stub comment — do not hard-error on system headers */
                        char stub[256];
                        snprintf(stub, sizeof(stub),
                                 "/* #include <%s> — sistema */\n", inc_name);
                        buf_push_str(out, stub);
                    }
                    free(inc_name);
                }
            } else if (DIRECTIVE("ifdef")) {
                if (pp->cond_depth < 64) {
                    size_t nlen = ident_len(skip_ws(arg));
                    bool found = macro_find(pp, skip_ws(arg), nlen) != NULL;
                    pp->cond_true[pp->cond_depth++] =
                        in_true_branch && found;
                }
            } else if (DIRECTIVE("ifndef")) {
                if (pp->cond_depth < 64) {
                    size_t nlen = ident_len(skip_ws(arg));
                    bool found = macro_find(pp, skip_ws(arg), nlen) != NULL;
                    pp->cond_true[pp->cond_depth++] =
                        in_true_branch && !found;
                }
            } else if (DIRECTIVE("if")) {
                if (pp->cond_depth < 64) {
                    pp->cond_true[pp->cond_depth++] =
                        in_true_branch && eval_cond(pp, arg);
                }
            } else if (DIRECTIVE("elif")) {
                if (pp->cond_depth > 0) {
                    /* Only enter elif if prev branches were false */
                    bool prev = pp->cond_true[pp->cond_depth - 1];
                    pp->cond_true[pp->cond_depth - 1] =
                        !prev && eval_cond(pp, arg);
                }
            } else if (DIRECTIVE("else")) {
                if (pp->cond_depth > 0)
                    pp->cond_true[pp->cond_depth-1] =
                        !pp->cond_true[pp->cond_depth-1];
            } else if (DIRECTIVE("endif")) {
                if (pp->cond_depth > 0) pp->cond_depth--;
            } else if (DIRECTIVE("error") && in_true_branch) {
                riglog_add(pp->log, ERR_CONFIG, filename, line, 0,
                           arg, "", "Revisa la condición que generó el #error", "");
            }
            /* pragma and unknown directives are silently ignored */
            free(arg);
            continue;
        }

        /* Normal code line — restore p and expand */
        p = line_start;
        if (!in_true_branch) {
            /* Skip the entire line */
            while (p < end && *p != '\n') p++;
            buf_push_char(out, '\n');
            continue;
        }

        /* Expand macros on this line */
        const char *line_end = p;
        while (line_end < end && *line_end != '\n') line_end++;
        char *line_src = strndup_safe(p, line_end - p);
        expand_text(pp, line_src, out, filename, line);
        free(line_src);
        p = line_end;
    }
}

/* ── Public API ─────────────────────────────────────────────── */
void preproc_init(Preproc *pp, RigCtx *ctx, RigErrorLog *log) {
    memset(pp, 0, sizeof(*pp));
    pp->ctx = ctx;
    pp->log = log;
    /* Built-in defines */
    preproc_define(pp, "__RIGCOM__", "300");
    preproc_define(pp, "__STDC__",   "1");
    preproc_define(pp, "__STDC_VERSION__", "201112L");
    preproc_define(pp, "NULL", "((void*)0)");
}

void preproc_add_path(Preproc *pp, const char *path) {
    if (pp->n_paths < MAX_INCLUDE_PATHS)
        pp->include_paths[pp->n_paths++] = path;
}

void preproc_define(Preproc *pp, const char *name, const char *body) {
    macro_define(pp, name, body);
}

char* preproc_run(Preproc *pp, const char *src,
                  size_t src_len, const char *filename) {
    Buf out = {0};
    process(pp, src, src_len, filename, &out, 0);
    buf_push_char(&out, '\0');
    return out.data;
}

void preproc_free(Preproc *pp) {
    for (uint32_t i = 0; i < pp->n_macros; i++) {
        free(pp->macros[i].name);
        free(pp->macros[i].body);
        if (pp->macros[i].params) {
            for (uint32_t j = 0; j < pp->macros[i].n_params; j++)
                free(pp->macros[i].params[j]);
            free(pp->macros[i].params);
        }
    }
}
