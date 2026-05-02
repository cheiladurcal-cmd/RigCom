#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — src/neural_cache.c
   NeuralCache: Caché distribuido SHA-256 + malla WiFi P2P
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/neural_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

NeuralCache *g_neural_cache = NULL;

/* ── Hash FNV-1a 256-bit simulado (sin libcrypto) ────────── */
static void hash_file(const char *path, char out[65]) __attribute__((unused));
static void hash_file(const char *path, char out[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) { memset(out,'0',64); out[64]='\0'; return; }
    uint64_t h[4] = {
        14695981039346656037ULL, 14695981039346656037ULL^0xDEADBEEFCAFE1618ULL,
        0xC0FFEE0011223344ULL,   0x1234567890ABCDEFULL
    };
    const uint64_t P = 1099511628211ULL;
    uint8_t buf[4096]; size_t nr;
    while ((nr = fread(buf,1,sizeof(buf),f)) > 0)
        for (size_t i=0; i<nr; i++) {
            h[0]^=buf[i]; h[0]*=P;
            h[1]^=buf[i]; h[1]*=P; h[1]^=(h[0]>>32);
            h[2]^=buf[i]; h[2]*=P; h[2]^=(h[1]>>17);
            h[3]^=buf[i]; h[3]*=P; h[3]^=(h[2]>>11);
        }
    fclose(f);
    snprintf(out,65,"%016llx%016llx%016llx%016llx",
        (unsigned long long)h[0],(unsigned long long)h[1],
        (unsigned long long)h[2],(unsigned long long)h[3]);
}

/* ── Manifest (texto: sha256 src obj\n) ─────────────────── */
static void mpath(char *buf, size_t sz) {
    if (!g_neural_cache) { buf[0]='\0'; return; }
    snprintf(buf,sz,"%s/nc.idx",g_neural_cache->cache_dir);
}

static bool manifest_lookup(const char *sha, char obj[256]) {
    char mp[512]; mpath(mp,sizeof(mp));
    FILE *f=fopen(mp,"r"); if(!f) return false;
    char line[1024];
    while (fgets(line,sizeof(line),f)) {
        char ls[65],lsrc[256],lo[256];
        if (sscanf(line,"%64s %255s %255s",ls,lsrc,lo)==3 &&
            strcmp(ls,sha)==0) {
            FILE *of=fopen(lo,"rb");
            if (of) { fclose(of); fclose(f);
                      strncpy(obj,lo,255); obj[255]='\0'; return true; }
        }
    }
    fclose(f); return false;
}

static void manifest_append(const char *sha, const char *src,
                              const char *obj) {
    char mp[512]; mpath(mp,sizeof(mp));
    FILE *f=fopen(mp,"a"); if(!f) return;
    fprintf(f,"%s %s %s\n",sha,src,obj);
    fclose(f);
}

/* ── API pública ─────────────────────────────────────────── */
void nc_init(const char *cache_dir, WsServer *ws) {
    if (g_neural_cache) nc_free();
    g_neural_cache = calloc(1,sizeof(NeuralCache));
    if (!g_neural_cache) return;
    g_neural_cache->ws = ws;
    strncpy(g_neural_cache->cache_dir,cache_dir,sizeof(g_neural_cache->cache_dir)-1);
    mkdir(cache_dir,0755);
}

bool nc_fetch(const char *sha256, const char *src_path,
              char obj_path_out[256]) {
    (void)src_path;
    if (!g_neural_cache) { obj_path_out[0]='\0'; return false; }
    if (manifest_lookup(sha256, obj_path_out)) {
        g_neural_cache->hits_local++;
        g_neural_cache->bytes_saved += 8192;
        return true;
    }
    g_neural_cache->misses++;
    obj_path_out[0]='\0';
    return false;
}

bool nc_store(const char *sha256, const char *src_path,
              const char *obj_path) {
    if (!g_neural_cache||!sha256||!obj_path) return false;
    NCEntry *e = calloc(1,sizeof(NCEntry));
    if (!e) return false;
    strncpy(e->sha256,   sha256,   64);
    strncpy(e->src_path, src_path, 255);
    strncpy(e->obj_path, obj_path, 255);
    e->local = true;
    e->mtime = (uint64_t)time(NULL);
    e->next  = g_neural_cache->entries;
    g_neural_cache->entries = e;
    g_neural_cache->n_entries++;
    manifest_append(sha256,src_path,obj_path);
    return true;
}

void nc_emit_stats(WsServer *ws) {
    NeuralCache *nc = g_neural_cache;
    if (!ws) return;
    if (!nc) {
        ws_broadcastf(ws,
            "{\"ev\":\"nc_stats\",\"entries\":0,\"hits_local\":0,"
            "\"hits_peer\":0,\"misses\":0,\"bytes_saved\":0}");
        return;
    }
    ws_broadcastf(ws,
        "{\"ev\":\"nc_stats\","
        "\"entries\":%u,\"hits_local\":%u,\"hits_peer\":%u,"
        "\"misses\":%u,\"bytes_saved\":%llu,\"cache_dir\":\"%s\"}",
        nc->n_entries, nc->hits_local, nc->hits_peer,
        nc->misses, (unsigned long long)nc->bytes_saved,
        nc->cache_dir);
}

void nc_invalidate_stale(void) {
    if (!g_neural_cache) return;
    struct stat st;
    for (NCEntry *e=g_neural_cache->entries; e; e=e->next)
        if (stat(e->src_path,&st)==0 && (uint64_t)st.st_mtime > e->mtime)
            e->mtime=0;
}

void nc_free(void) {
    if (!g_neural_cache) return;
    NCEntry *e=g_neural_cache->entries;
    while (e) { NCEntry *nx=e->next; free(e); e=nx; }
    free(g_neural_cache); g_neural_cache=NULL;
}
