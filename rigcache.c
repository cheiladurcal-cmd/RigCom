#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — src/rigcache.c
   Caché Global de Objetos — SHA-256 content-addressed cache
   Si hash(fuente+headers) coincide → reusar .o sin compilar.
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/rigcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>

CacheStats g_cache_stats = {0};

/* ── SHA-256 puro en C — sin dependencias externas ──────── */
typedef struct { uint8_t data[64]; uint32_t datalen; uint64_t bitlen; uint32_t state[8]; } SHA256_CTX;

static const uint32_t k256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTRIGHT(a,b) (((a)>>(b))|((a)<<(32-(b))))
#define CH(x,y,z)  (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x)  (ROTRIGHT(x,2)^ROTRIGHT(x,13)^ROTRIGHT(x,22))
#define EP1(x)  (ROTRIGHT(x,6)^ROTRIGHT(x,11)^ROTRIGHT(x,25))
#define SIG0(x) (ROTRIGHT(x,7)^ROTRIGHT(x,18)^((x)>>3))
#define SIG1(x) (ROTRIGHT(x,17)^ROTRIGHT(x,19)^((x)>>10))

static void sha256_transform(SHA256_CTX *ctx, const uint8_t *d) {
    uint32_t a,b,c,e,f,g,h,i,j,t1,t2,m[64];
    uint32_t dd;
    for(i=0,j=0;i<16;i++,j+=4)
        m[i]=(uint32_t)(d[j]<<24)|(d[j+1]<<16)|(d[j+2]<<8)|d[j+3];
    for(;i<64;i++) m[i]=SIG1(m[i-2])+m[i-7]+SIG0(m[i-15])+m[i-16];
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; dd=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for(i=0;i<64;i++){
        t1=h+EP1(e)+CH(e,f,g)+k256[i]+m[i];
        t2=EP0(a)+MAJ(a,b,c);
        h=g; g=f; f=e; e=dd+t1; dd=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=dd;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}
static void sha256_init(SHA256_CTX *ctx) {
    ctx->datalen=0; ctx->bitlen=0;
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
}
static void sha256_update(SHA256_CTX *ctx, const uint8_t *d, size_t len) {
    for(size_t i=0;i<len;i++){
        ctx->data[ctx->datalen++]=d[i];
        if(ctx->datalen==64){ sha256_transform(ctx,ctx->data); ctx->bitlen+=512; ctx->datalen=0; }
    }
}
static void sha256_final(SHA256_CTX *ctx, uint8_t *hash) {
    uint32_t i=ctx->datalen;
    ctx->data[i++]=0x80;
    if(ctx->datalen<56){ while(i<56) ctx->data[i++]=0; }
    else { while(i<64) ctx->data[i++]=0; sha256_transform(ctx,ctx->data); memset(ctx->data,0,56); }
    ctx->bitlen+=ctx->datalen*8;
    ctx->data[63]=(uint8_t)(ctx->bitlen); ctx->data[62]=(uint8_t)(ctx->bitlen>>8);
    ctx->data[61]=(uint8_t)(ctx->bitlen>>16); ctx->data[60]=(uint8_t)(ctx->bitlen>>24);
    ctx->data[59]=(uint8_t)(ctx->bitlen>>32); ctx->data[58]=(uint8_t)(ctx->bitlen>>40);
    ctx->data[57]=(uint8_t)(ctx->bitlen>>48); ctx->data[56]=(uint8_t)(ctx->bitlen>>56);
    sha256_transform(ctx,ctx->data);
    for(i=0;i<4;i++){
        hash[i]    =(uint8_t)(ctx->state[0]>>(24-i*8));
        hash[i+4]  =(uint8_t)(ctx->state[1]>>(24-i*8));
        hash[i+8]  =(uint8_t)(ctx->state[2]>>(24-i*8));
        hash[i+12] =(uint8_t)(ctx->state[3]>>(24-i*8));
        hash[i+16] =(uint8_t)(ctx->state[4]>>(24-i*8));
        hash[i+20] =(uint8_t)(ctx->state[5]>>(24-i*8));
        hash[i+24] =(uint8_t)(ctx->state[6]>>(24-i*8));
        hash[i+28] =(uint8_t)(ctx->state[7]>>(24-i*8));
    }
}

/* Hashea el contenido de un archivo */
static bool hash_file_content(const char *path, SHA256_CTX *ctx) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[4096];
    size_t nr;
    while ((nr = fread(buf, 1, sizeof(buf), f)) > 0)
        sha256_update(ctx, buf, nr);
    fclose(f);
    return true;
}

/* ── Inicializar caché ────────────────────────────────────── */
bool rigcache_init(const char *cache_dir) {
    struct stat st;
    if (stat(cache_dir, &st) != 0) {
        if (mkdir(cache_dir, 0755) != 0) return false;
    }
    return true;
}

/* ── Hash SHA-256 de fuente + headers transitivos ─────────── */
bool rigcache_hash_file(const char *src_path,
                         const char *include_dirs[],
                         int n_include_dirs,
                         char out_sha256[65]) {
    SHA256_CTX ctx;
    sha256_init(&ctx);

    /* 1. Hash el archivo fuente */
    if (!hash_file_content(src_path, &ctx)) return false;

    /* 2. Busca #include "..." y hashea transitivamente (nivel 1) */
    FILE *f = fopen(src_path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "#include \"", 10) != 0) continue;
            char *q = strchr(line + 10, '"');
            if (!q) continue;
            *q = '\0';
            const char *inc_name = line + 10;

            /* Buscar en include_dirs */
            for (int d = 0; d < n_include_dirs; d++) {
                char inc_path[512];
                snprintf(inc_path, sizeof(inc_path), "%s/%s",
                         include_dirs[d], inc_name);
                if (hash_file_content(inc_path, &ctx)) break;
            }
            /* Intentar relativo al src */
            char rel[512];
            const char *slash = strrchr(src_path, '/');
            if (slash) {
                size_t dir_len = (size_t)(slash - src_path);
                if (dir_len < sizeof(rel)-1) {
                    memcpy(rel, src_path, dir_len);
                    rel[dir_len] = '/';
                    strncpy(rel + dir_len + 1, inc_name, sizeof(rel)-dir_len-2);
                    hash_file_content(rel, &ctx);
                }
            }
        }
        fclose(f);
    }

    uint8_t raw[32];
    sha256_final(&ctx, raw);
    for (int i = 0; i < 32; i++)
        snprintf(out_sha256 + i*2, 3, "%02x", raw[i]);
    out_sha256[64] = '\0';
    return true;
}

/* ── Lookup en caché ─────────────────────────────────────── */
bool rigcache_lookup(const char *cache_dir,
                      const char *src_path,
                      const char *sha256,
                      char obj_path_out[256]) {
    /* Nombre del objeto: cache_dir/sha256.o */
    snprintf(obj_path_out, 256, "%s/%s.o", cache_dir, sha256);

    struct stat st;
    if (stat(obj_path_out, &st) == 0) {
        g_cache_stats.hits++;
        g_cache_stats.bytes_saved += (uint64_t)st.st_size;
        (void)src_path;
        return true;
    }
    g_cache_stats.misses++;
    return false;
}

/* ── Guardar en caché ─────────────────────────────────────── */
bool rigcache_store(const char *cache_dir,
                     const char *src_path,
                     const char *sha256,
                     const char *obj_path) {
    char dest[256];
    snprintf(dest, sizeof(dest), "%s/%s.o", cache_dir, sha256);

    /* Copiar el .o al caché */
    FILE *src_f = fopen(obj_path, "rb");
    if (!src_f) { (void)src_path; return false; }
    FILE *dst_f = fopen(dest, "wb");
    if (!dst_f) { fclose(src_f); return false; }

    uint8_t buf[8192];
    size_t nr;
    while ((nr = fread(buf, 1, sizeof(buf), src_f)) > 0)
        fwrite(buf, 1, nr, dst_f);
    fclose(src_f); fclose(dst_f);

    g_cache_stats.entries++;
    return true;
}

/* ── Evict LRU ────────────────────────────────────────────── */
void rigcache_evict(const char *cache_dir, int max_entries) {
    DIR *d = opendir(cache_dir);
    if (!d) return;
    /* Contar entradas */
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strstr(de->d_name, ".o")) count++;
    }
    closedir(d);
    if (count <= max_entries) return;

    /* Eliminar los más antiguos */
    d = opendir(cache_dir);
    if (!d) return;
    while ((de = readdir(d)) != NULL && count > max_entries) {
        if (!strstr(de->d_name, ".o")) continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", cache_dir, de->d_name);
        unlink(full);
        count--;
    }
    closedir(d);
}

/* ── Estadísticas JSON ────────────────────────────────────── */
int rigcache_stats_json(char *buf, size_t sz) {
    return snprintf(buf, sz,
        "{\"ev\":\"cache_stats\","
        "\"entries\":%d,"
        "\"hits\":%d,"
        "\"misses\":%d,"
        "\"bytes_saved\":%llu,"
        "\"hit_rate\":%.1f}",
        g_cache_stats.entries,
        g_cache_stats.hits,
        g_cache_stats.misses,
        (unsigned long long)g_cache_stats.bytes_saved,
        (g_cache_stats.hits + g_cache_stats.misses > 0)
          ? (g_cache_stats.hits * 100.0 /
             (g_cache_stats.hits + g_cache_stats.misses))
          : 0.0);
}
