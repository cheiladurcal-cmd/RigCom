/* ============================================================
   RigCom v8.0 — include/neural_cache.h
   NeuralCache: Caché distribuido SHA-256 por malla WiFi P2P
   API usada por main.c: nc_init / nc_fetch / nc_emit_stats
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef NEURAL_CACHE_H
#define NEURAL_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include "wsserver.h"
#include "rigcache.h"

typedef struct NCEntry {
    char     sha256[65];
    char     src_path[256];
    char     obj_path[256];
    uint64_t mtime;
    bool     local;
    char     peer_ip[64];
    struct NCEntry *next;
} NCEntry;

typedef struct {
    WsServer  *ws;
    char       cache_dir[256];
    NCEntry   *entries;
    uint32_t   n_entries;
    uint32_t   hits_local;
    uint32_t   hits_peer;
    uint32_t   misses;
    uint64_t   bytes_saved;
} NeuralCache;

extern NeuralCache *g_neural_cache;

void nc_init         (const char *cache_dir, WsServer *ws);
bool nc_fetch        (const char *sha256, const char *src_path,
                      char obj_path_out[256]);
bool nc_store        (const char *sha256, const char *src_path,
                      const char *obj_path);
void nc_emit_stats   (WsServer *ws);
void nc_invalidate_stale(void);
void nc_free         (void);

#endif /* NEURAL_CACHE_H */
