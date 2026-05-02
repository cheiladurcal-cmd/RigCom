/* ============================================================
   RigCom v8.0 — include/rigcache.h
   Caché Global de Objetos — Builds Instantáneos (Fase 2)
   Hash SHA-256 de fuente+headers → .o del caché.
   Si el hash no cambió, se omite todo el pipeline.
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef RIGCACHE_H
#define RIGCACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Directorio de caché por defecto */
#define RIGCACHE_DIR        ".rigcache"
#define RIGCACHE_MANIFEST   ".rigcache/manifest.idx"
#define RIGCACHE_MAX_ENTRY  256

/* ── Entrada del caché ───────────────────────────────────── */
typedef struct {
    char     src_path[256];   /* ruta del .c original            */
    char     sha256[65];      /* hex SHA-256 (fuente+headers)    */
    char     obj_path[256];   /* ruta al .o en caché             */
    uint64_t mtime;           /* mtime Unix del .c               */
    bool     valid;
} CacheEntry;

/* ── API pública ─────────────────────────────────────────── */

/* Inicializa/crea directorio de caché */
bool rigcache_init(const char *cache_dir);

/* Calcula SHA-256 de archivo fuente + todos sus #includes */
bool rigcache_hash_file(const char *src_path,
                         const char *include_dirs[],
                         int n_include_dirs,
                         char out_sha256[65]);

/* Busca un .o en caché para ese hash.
   Devuelve true y rellena obj_path_out si hay hit. */
bool rigcache_lookup(const char *cache_dir,
                      const char *src_path,
                      const char *sha256,
                      char obj_path_out[256]);

/* Registra un .o recién compilado en el caché */
bool rigcache_store(const char *cache_dir,
                     const char *src_path,
                     const char *sha256,
                     const char *obj_path);

/* Estadísticas de caché para el dashboard */
typedef struct {
    int     entries;
    int     hits;
    int     misses;
    uint64_t bytes_saved;
} CacheStats;

extern CacheStats g_cache_stats;

/* Limpia entradas antiguas (LRU) */
void rigcache_evict(const char *cache_dir, int max_entries);

/* Serializa stats como JSON */
int rigcache_stats_json(char *buf, size_t sz);

#endif /* RIGCACHE_H */
