/* ============================================================
   RigCom v8.0 — src/error.c
   Error system: real-time reporting + suggestions + JSON
   ============================================================ */
#include "../include/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static char* dup_str(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* ── Lifecycle ──────────────────────────────────────────────── */
RigErrorLog* riglog_new(void) {
    RigErrorLog *log = malloc(sizeof(RigErrorLog));
    if (!log) return NULL;
    log->capacity = 128;
    log->count    = 0;
    log->errors   = malloc(log->capacity * sizeof(RigError *));
    if (!log->errors) { free(log); return NULL; }
    return log;
}

void riglog_free(RigErrorLog *log) {
    if (!log) return;
    for (uint32_t i = 0; i < log->count; i++) {
        RigError *e = log->errors[i];
        free(e->file);
        free(e->message);
        free(e->context);
        free(e->suggestion);
        free(e->fix_example);
        free(e);
    }
    free(log->errors);
    free(log);
}

/* ── Append ── */
void riglog_add(RigErrorLog *log, int kind,
                const char *file, uint32_t line, uint32_t col,
                const char *msg, const char *ctx,
                const char *suggestion, const char *fix) {
    if (!log) return;
    if (log->count >= log->capacity) {
        log->capacity *= 2;
        RigError **tmp = realloc(log->errors,
                                  log->capacity * sizeof(RigError *));
        if (!tmp) return;
        log->errors = tmp;
    }
    RigError *err = malloc(sizeof(RigError));
    if (!err) return;
    err->kind        = kind;
    err->file        = dup_str(file);
    err->line        = line;
    err->column      = col;
    err->message     = dup_str(msg);
    err->context     = dup_str(ctx);
    err->suggestion  = dup_str(suggestion);
    err->fix_example = dup_str(fix);
    log->errors[log->count++] = err;
}

bool riglog_has_errors(const RigErrorLog *log) {
    if (!log) return false;
    for (uint32_t i = 0; i < log->count; i++)
        if (log->errors[i]->kind < 100) return true;
    return false;
}

/* ── Print single error ── */
void riglog_print_error(RigError *err) {
    if (!err) return;
    const char *kind_str = (err->kind < 100) ? "\033[31merror\033[0m"
                                              : "\033[33mwarning\033[0m";
    printf("\n");
    printf("  \033[33m%s:%u:%u:\033[0m %s\n",
           err->file, err->line, err->column, kind_str);
    printf("  \033[31m│  %s\033[0m\n", err->message);
    if (err->context && err->context[0])
        printf("  \033[90m│  %s\033[0m\n", err->context);
    if (err->suggestion && err->suggestion[0])
        printf("  \033[32m│  💡 %s\033[0m\n", err->suggestion);
    if (err->fix_example && err->fix_example[0])
        printf("  \033[32m└─ ✓  %s\033[0m\n", err->fix_example);
}

/* ── Print all ── */
void riglog_print_all(RigErrorLog *log) {
    if (!log) return;
    if (log->count == 0) {
        printf("  \033[32m✓ Sin errores\033[0m\n");
        return;
    }
    uint32_t nerr = 0, nwarn = 0;
    for (uint32_t i = 0; i < log->count; i++) {
        riglog_print_error(log->errors[i]);
        if (log->errors[i]->kind < 100) nerr++;
        else                             nwarn++;
    }
    printf("\n");
    if (nerr)  printf("  \033[31m✗ %u error(es)\033[0m", nerr);
    if (nwarn) printf("  \033[33m  %u advertencia(s)\033[0m", nwarn);
    printf("\n\n");
}

/* ── JSON serialization ── */
char* riglog_to_json(RigErrorLog *log) {
    if (!log || log->count == 0) {
        char *s = malloc(3);
        if (s) strcpy(s, "[]");
        return s;
    }

    /* Dynamic buffer */
    size_t sz  = 64 + log->count * 512;
    char  *buf = malloc(sz);
    if (!buf) return NULL;
    size_t pos = 0;

    pos += (size_t)snprintf(buf + pos, sz - pos, "[\n");

    for (uint32_t i = 0; i < log->count; i++) {
        RigError *e = log->errors[i];
        /* Escape quotes in strings for safety */
        pos += (size_t)snprintf(buf + pos, sz - pos,
            "  {"
            "\"file\":\"%s\","
            "\"line\":%u,"
            "\"col\":%u,"
            "\"kind\":%d,"
            "\"msg\":\"%s\","
            "\"suggestion\":\"%s\","
            "\"fix\":\"%s\""
            "}%s\n",
            e->file       ? e->file       : "",
            e->line,
            e->column,
            e->kind,
            e->message    ? e->message    : "",
            e->suggestion ? e->suggestion : "",
            e->fix_example? e->fix_example: "",
            (i < log->count - 1) ? "," : "");
    }

    if (pos + 3 < sz) {
        buf[pos++] = ']';
        buf[pos++] = '\n';
        buf[pos]   = '\0';
    }
    return buf;
}
