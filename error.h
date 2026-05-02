/* ============================================================
   RigCom v8.0 — include/error.h
   Error system: real-time reporting + suggestions + JSON
   ============================================================ */
#ifndef ERROR_H
#define ERROR_H

#include <stdint.h>
#include <stdbool.h>

/* ── Error kinds ── */
typedef enum {
    ERR_SYNTAX            = 0,
    ERR_SEMANTIC          = 1,
    ERR_TYPE_MISMATCH     = 2,
    ERR_UNDEFINED_SYMBOL  = 3,
    ERR_REDEFINITION      = 4,
    ERR_INVALID_OPERATION = 5,
    ERR_FILE_NOT_FOUND    = 6,
    ERR_CONFIG            = 7,
    ERR_INTERNAL          = 8,
    WARN_IMPLICIT_CONV    = 100,
    WARN_UNUSED_VAR       = 101,
    WARN_SHADOW           = 102,
} RigErrorKind;

/* ── Single error record ── */
typedef struct {
    int       kind;
    char     *file;
    uint32_t  line;
    uint32_t  column;
    char     *message;
    char     *context;     /* source line + caret */
    char     *suggestion;
    char     *fix_example;
} RigError;

/* ── Error log ── */
typedef struct RigErrorLog {
    RigError **errors;
    uint32_t   count;
    uint32_t   capacity;
} RigErrorLog;

/* ── Lifecycle ── */
RigErrorLog* riglog_new (void);
void         riglog_free(RigErrorLog *log);

/* ── Append ── */
void riglog_add(RigErrorLog *log, int kind,
                const char *file, uint32_t line, uint32_t col,
                const char *msg,  const char *ctx,
                const char *suggestion, const char *fix);

/* ── Output ── */
void  riglog_print_error(RigError *err);
void  riglog_print_all  (RigErrorLog *log);
char* riglog_to_json    (RigErrorLog *log);
bool  riglog_has_errors (const RigErrorLog *log);

#endif /* ERROR_H */
