/* ============================================================
   RigCom v8.0 — include/toml.h
   Minimal TOML parser (section + key=value)
   ============================================================ */
#ifndef TOML_H
#define TOML_H

#include <stdint.h>
#include <stdbool.h>

#define TOML_MAX_ENTRIES 256
#define TOML_KEY_MAX     64
#define TOML_VAL_MAX     512

typedef struct {
    char section[TOML_KEY_MAX];
    char key    [TOML_KEY_MAX];
    char value  [TOML_VAL_MAX];
} TomlEntry;

typedef struct {
    TomlEntry entries[TOML_MAX_ENTRIES];
    uint32_t  count;
    char      errbuf[128];
    bool      has_error;
} TomlDoc;

/* ── Parse from file ────────────────────────────────────────── */
bool toml_parse_file  (TomlDoc *doc, const char *path);
bool toml_parse_string(TomlDoc *doc, const char *text);

/* ── Lookup helpers ─────────────────────────────────────────── */
const char* toml_get      (const TomlDoc *doc, const char *section, const char *key);
bool        toml_get_bool (const TomlDoc *doc, const char *section, const char *key, bool   def);
int         toml_get_int  (const TomlDoc *doc, const char *section, const char *key, int    def);
const char* toml_get_str  (const TomlDoc *doc, const char *section, const char *key,
                            const char *def);

/* ── Array value (returns nth element, "" if none) ──────────── */
/* For values like: defines = ["A=1", "B=2"] */
const char* toml_get_array_elem(const TomlDoc *doc, const char *section,
                                 const char *key, uint32_t index);
uint32_t    toml_get_array_len (const TomlDoc *doc, const char *section, const char *key);

#endif /* TOML_H */
