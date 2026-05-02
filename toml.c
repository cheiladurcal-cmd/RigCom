/* ============================================================
   RigCom v8.0 — src/toml.c
   Minimal TOML parser: sections, key=value, string arrays
   ============================================================ */
#include "../include/toml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Internal helpers ───────────────────────────────────────── */
static const char* skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\t')) p++;
    return p;
}

static const char* skip_to_eol(const char *p) {
    while (*p && *p != '\n' && *p != '\r') p++;
    return p;
}

static void trim_trailing(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' ||
                     s[n-1] == '\r' || s[n-1] == '\n')) {
        s[--n] = '\0';
    }
}

/* ── Parse key or section name ──────────────────────────────── */
static const char* read_bare_key(const char *p, char *buf, size_t max) {
    size_t i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.')) {
        if (i + 1 < max) buf[i++] = *p;
        p++;
    }
    buf[i] = '\0';
    return p;
}

/* ── Parse quoted string → buf ──────────────────────────────── */
static const char* read_quoted(const char *p, char *buf, size_t max) {
    /* p points to opening " */
    if (*p == '"') p++;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n':  if (i+1<max) buf[i++]='\n'; break;
                case 't':  if (i+1<max) buf[i++]='\t'; break;
                case '\\': if (i+1<max) buf[i++]='\\'; break;
                case '"':  if (i+1<max) buf[i++]='"';  break;
                default:   if (i+1<max) buf[i++]=*p;   break;
            }
        } else {
            if (i+1 < max) buf[i++] = *p;
        }
        p++;
    }
    if (*p == '"') p++;
    buf[i] = '\0';
    return p;
}

/* ── Parse single-quoted string ─────────────────────────────── */
static const char* read_single_quoted(const char *p, char *buf, size_t max) {
    if (*p == '\'') p++;
    size_t i = 0;
    while (*p && *p != '\'') {
        if (i+1 < max) buf[i++] = *p;
        p++;
    }
    if (*p == '\'') p++;
    buf[i] = '\0';
    return p;
}

/* ── Find existing entries for array ────────────────────────── */
static uint32_t count_matching(const TomlDoc *doc, const char *section,
                                const char *key) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < doc->count; i++) {
        if (strcmp(doc->entries[i].section, section) == 0 &&
            strcmp(doc->entries[i].key,     key    ) == 0) n++;
    }
    return n;
}

/* ── Add entry ──────────────────────────────────────────────── */
static bool add_entry(TomlDoc *doc, const char *section,
                       const char *key, const char *value) {
    if (doc->count >= TOML_MAX_ENTRIES) return false;
    TomlEntry *e = &doc->entries[doc->count++];
    strncpy(e->section, section, TOML_KEY_MAX - 1);
    strncpy(e->key,     key,     TOML_KEY_MAX - 1);
    strncpy(e->value,   value,   TOML_VAL_MAX - 1);
    e->section[TOML_KEY_MAX-1] = '\0';
    e->key    [TOML_KEY_MAX-1] = '\0';
    e->value  [TOML_VAL_MAX-1] = '\0';
    return true;
}

/* ── Parse inline array: ["a","b","c"] ──────────────────────── */
static void parse_array(TomlDoc *doc, const char *section,
                         const char *key, const char *p) {
    /* skip '[' */
    p = skip_ws(p);
    if (*p == '[') p++;

    char elem[TOML_VAL_MAX];
    while (*p) {
        p = skip_ws(p);
        if (*p == ']' || *p == '\0') break;
        if (*p == ',') { p++; continue; }
        if (*p == '#') break;   /* comment */
        if (*p == '"') {
            p = read_quoted(p, elem, sizeof(elem));
        } else if (*p == '\'') {
            p = read_single_quoted(p, elem, sizeof(elem));
        } else {
            /* bare value */
            const char *start = p;
            while (*p && *p != ',' && *p != ']' && *p != '\n') p++;
            size_t len = (size_t)(p - start);
            if (len >= TOML_VAL_MAX) len = TOML_VAL_MAX - 1;
            memcpy(elem, start, len);
            elem[len] = '\0';
            trim_trailing(elem);
        }
        /* Store with index suffix: key[0], key[1], ... */
        char indexed_key[TOML_KEY_MAX];
        uint32_t idx = count_matching(doc, section, key);
        snprintf(indexed_key, sizeof(indexed_key), "%s[%u]", key, idx);
        add_entry(doc, section, indexed_key, elem);
        /* Also store raw array entry for count lookup */
        add_entry(doc, section, key, elem);
    }
}

/* ── Parse source text ──────────────────────────────────────── */
bool toml_parse_string(TomlDoc *doc, const char *text) {
    memset(doc, 0, sizeof(*doc));
    char section[TOML_KEY_MAX] = "";
    const char *p = text;

    while (*p) {
        p = skip_ws(p);
        if (*p == '\0') break;

        /* Comment */
        if (*p == '#') { p = skip_to_eol(p); continue; }

        /* Newline */
        if (*p == '\n' || *p == '\r') { p++; continue; }

        /* Section header [name] */
        if (*p == '[') {
            p++;
            /* Skip array-of-tables [[ ]] */
            if (*p == '[') p++;
            p = skip_ws(p);
            const char *end = p;
            while (*end && *end != ']' && *end != '\n') end++;
            size_t len = (size_t)(end - p);
            if (len >= TOML_KEY_MAX) len = TOML_KEY_MAX - 1;
            memcpy(section, p, len);
            section[len] = '\0';
            trim_trailing(section);
            /* remove trailing ] for [[ */
            if (section[len-1] == ']') section[len-1] = '\0';
            p = skip_to_eol(end);
            continue;
        }

        /* Key = value */
        char key[TOML_KEY_MAX];
        p = read_bare_key(p, key, sizeof(key));
        if (key[0] == '\0') { p = skip_to_eol(p); continue; }

        p = skip_ws(p);
        if (*p != '=') { p = skip_to_eol(p); continue; }
        p++; /* consume '=' */
        p = skip_ws(p);

        /* Inline array */
        if (*p == '[') {
            parse_array(doc, section, key, p);
            p = skip_to_eol(p);
            continue;
        }

        /* Value */
        char value[TOML_VAL_MAX];
        if (*p == '"') {
            p = read_quoted(p, value, sizeof(value));
        } else if (*p == '\'') {
            p = read_single_quoted(p, value, sizeof(value));
        } else {
            /* bare value: read to end of line or comment */
            const char *start = p;
            while (*p && *p != '\n' && *p != '\r' && *p != '#') p++;
            size_t len = (size_t)(p - start);
            if (len >= TOML_VAL_MAX) len = TOML_VAL_MAX - 1;
            memcpy(value, start, len);
            value[len] = '\0';
            trim_trailing(value);
        }

        add_entry(doc, section, key, value);
        p = skip_to_eol(p);
    }
    return true;
}

/* ── Parse from file ────────────────────────────────────────── */
bool toml_parse_file(TomlDoc *doc, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        memset(doc, 0, sizeof(*doc));
        snprintf(doc->errbuf, sizeof(doc->errbuf),
                 "Cannot open: %s", path);
        doc->has_error = true;
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    if (sz <= 0) { fclose(fp); return toml_parse_string(doc, ""); }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return false; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    buf[rd] = '\0';
    fclose(fp);

    bool ok = toml_parse_string(doc, buf);
    free(buf);
    return ok;
}

/* ── Lookup helpers ─────────────────────────────────────────── */
const char* toml_get(const TomlDoc *doc, const char *section, const char *key) {
    for (uint32_t i = 0; i < doc->count; i++) {
        if (strcmp(doc->entries[i].section, section) == 0 &&
            strcmp(doc->entries[i].key,     key    ) == 0) {
            return doc->entries[i].value;
        }
    }
    return NULL;
}

bool toml_get_bool(const TomlDoc *doc, const char *section,
                    const char *key, bool def) {
    const char *v = toml_get(doc, section, key);
    if (!v) return def;
    return (strcmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcmp(v, "yes") == 0);
}

int toml_get_int(const TomlDoc *doc, const char *section,
                  const char *key, int def) {
    const char *v = toml_get(doc, section, key);
    if (!v) return def;
    return atoi(v);
}

const char* toml_get_str(const TomlDoc *doc, const char *section,
                          const char *key, const char *def) {
    const char *v = toml_get(doc, section, key);
    return v ? v : def;
}

/* ── Array lookup: indexed as key[0], key[1], ... ───────────── */
const char* toml_get_array_elem(const TomlDoc *doc, const char *section,
                                 const char *key, uint32_t index) {
    char indexed[TOML_KEY_MAX];
    snprintf(indexed, sizeof(indexed), "%s[%u]", key, index);
    return toml_get(doc, section, indexed);
}

uint32_t toml_get_array_len(const TomlDoc *doc, const char *section,
                              const char *key) {
    uint32_t n = 0;
    char indexed[TOML_KEY_MAX];
    for (;;) {
        snprintf(indexed, sizeof(indexed), "%s[%u]", key, n);
        if (!toml_get(doc, section, indexed)) break;
        n++;
    }
    return n;
}
