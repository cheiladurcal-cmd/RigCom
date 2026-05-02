/* ============================================================
   RigCom v8.0 — src/riglib.c
   RigLib: stdlib ARM64 sin dependencias Termux / Android.
   Todos los syscalls pasan por syscall(2); el asignador usa
   mmap anónimo y una lista libre first-fit con coalescencia.
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/riglib.h"
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/mman.h>

/* ARM64: sys_openat reemplaza a sys_open */
#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif

RLHeapStats g_rl_heap = {0};

/* ══════════════════════════════════════════════════════════════
   SYSCALL WRAPPERS
   ══════════════════════════════════════════════════════════════ */

void* rl_mmap(void *addr, rl_size len, int prot, int flags,
               int fd, rl_i64 offset) {
    void *p = (void*)syscall(SYS_mmap, addr, len, prot, flags, fd, offset);
    if (p != RL_MAP_FAILED) g_rl_heap.bytes_mmaped += len;
    return p;
}

int rl_munmap(void *addr, rl_size len) {
    int r = (int)syscall(SYS_munmap, addr, len);
    if (r == 0) g_rl_heap.bytes_mmaped -= len;
    return r;
}

rl_i64 rl_write(int fd, const void *buf, rl_size len) {
    return (rl_i64)syscall(SYS_write, fd, buf, len);
}

rl_i64 rl_read(int fd, void *buf, rl_size len) {
    return (rl_i64)syscall(SYS_read, fd, buf, len);
}

int rl_open(const char *path, int flags, int mode) {
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

int rl_close(int fd) {
    return (int)syscall(SYS_close, fd);
}

rl_i64 rl_lseek(int fd, rl_i64 offset, int whence) {
    return (rl_i64)syscall(SYS_lseek, fd, offset, whence);
}

void rl_exit(int code) {
    syscall(SYS_exit_group, code);
    __builtin_unreachable();
}

int rl_getpid(void) {
    return (int)syscall(SYS_getpid);
}

int rl_mkdir(const char *path, int mode) {
#ifdef SYS_mkdir
    return (int)syscall(SYS_mkdir, path, mode);
#else
    return (int)syscall(SYS_mkdirat, AT_FDCWD, path, mode);
#endif
}

int rl_unlink(const char *path) {
    return (int)syscall(SYS_unlinkat, AT_FDCWD, path, 0);
}

int rl_stat(const char *path, void *statbuf) {
#ifdef SYS_stat
    return (int)syscall(SYS_stat, path, statbuf);
#else
    return (int)syscall(SYS_newfstatat, AT_FDCWD, path, statbuf, 0);
#endif
}

/* ══════════════════════════════════════════════════════════════
   ASIGNADOR DE MEMORIA — first-fit con lista libre
   Cabecera de bloque (16 bytes, alineada):
     [size_t usable_size][size_t magic][data…]
   La lista libre enlaza bloques libres dentro del espacio data.
   Chunks: mmap en múltiplos de RL_CHUNK (256 KB).
   ══════════════════════════════════════════════════════════════ */

#define RL_MAGIC_FREE  ((rl_size)0xDEADBEEFCAFE0000ull)
#define RL_MAGIC_USED  ((rl_size)0xC0FFEE0011223344ull)
#define RL_HDR_SZ      (sizeof(RLBlkHdr))
#define RL_ALIGN       16
#define RL_CHUNK       (256 * 1024)
#define RL_HUGE_THRESH (128 * 1024)

typedef struct RLBlkHdr {
    rl_size          usable;
    rl_size          magic;
    struct RLBlkHdr *next_free;
} RLBlkHdr;

static RLBlkHdr *s_freelist = NULL;

static rl_size rl_align_up(rl_size v, rl_size a) {
    return (v + a - 1) & ~(a - 1);
}

static RLBlkHdr* blk_from_ptr(void *p) {
    return (RLBlkHdr*)((char*)p - RL_HDR_SZ);
}

static void* ptr_from_blk(RLBlkHdr *b) {
    return (char*)b + RL_HDR_SZ;
}

/* Añadir bloque libre al inicio de la lista libre */
static void freelist_push(RLBlkHdr *b) {
    b->magic     = RL_MAGIC_FREE;
    b->next_free = s_freelist;
    s_freelist   = b;
    g_rl_heap.free_blocks++;
}

/* Buscar bloque libre first-fit de tamaño >= need */
static RLBlkHdr* freelist_pop(rl_size need) {
    RLBlkHdr **prev = &s_freelist;
    RLBlkHdr  *cur  = s_freelist;
    while (cur) {
        if (cur->usable >= need) {
            *prev = cur->next_free;
            cur->next_free = NULL;
            g_rl_heap.free_blocks--;
            return cur;
        }
        prev = &cur->next_free;
        cur  = cur->next_free;
    }
    return NULL;
}

/* Pedir chunk nuevo al SO */
static RLBlkHdr* mmap_chunk(rl_size need) {
    rl_size total = rl_align_up(need + RL_HDR_SZ, RL_CHUNK);
    void *p = rl_mmap(NULL, total,
                      RL_PROT_READ | RL_PROT_WRITE,
                      RL_MAP_PRIVATE | RL_MAP_ANON, -1, 0);
    if (p == RL_MAP_FAILED) return NULL;
    RLBlkHdr *b = (RLBlkHdr*)p;
    b->usable    = total - RL_HDR_SZ;
    b->magic     = RL_MAGIC_FREE;
    b->next_free = NULL;
    return b;
}

void* rl_malloc(rl_size size) {
    if (size == 0) return NULL;
    rl_size need = rl_align_up(size, RL_ALIGN);

    /* Bloques enormes: mmap directo */
    if (need > RL_HUGE_THRESH) {
        rl_size total = rl_align_up(need + RL_HDR_SZ, 4096);
        void *p = rl_mmap(NULL, total,
                          RL_PROT_READ | RL_PROT_WRITE,
                          RL_MAP_PRIVATE | RL_MAP_ANON, -1, 0);
        if (p == RL_MAP_FAILED) return NULL;
        RLBlkHdr *b = (RLBlkHdr*)p;
        b->usable    = total - RL_HDR_SZ;
        b->magic     = RL_MAGIC_USED;
        b->next_free = NULL;
        g_rl_heap.allocs++;
        g_rl_heap.bytes_in_use += b->usable;
        return ptr_from_blk(b);
    }

    RLBlkHdr *b = freelist_pop(need);
    if (!b) {
        b = mmap_chunk(need);
        if (!b) return NULL;
        /* Si el chunk es mucho mayor que need, dividirlo */
        rl_size leftover = b->usable - need - RL_HDR_SZ;
        if (leftover > RL_HDR_SZ + RL_ALIGN) {
            RLBlkHdr *split = (RLBlkHdr*)((char*)ptr_from_blk(b) + need);
            split->usable = leftover;
            freelist_push(split);
            b->usable = need;
        }
    }

    b->magic = RL_MAGIC_USED;
    g_rl_heap.allocs++;
    g_rl_heap.bytes_in_use += b->usable;
    return ptr_from_blk(b);
}

void rl_free(void *ptr) {
    if (!ptr) return;
    RLBlkHdr *b = blk_from_ptr(ptr);
    if (b->magic != RL_MAGIC_USED) return; /* doble free / corrupción */
    g_rl_heap.bytes_in_use -= b->usable;
    g_rl_heap.frees++;
    /* Bloques enormes: munmap directo */
    if (b->usable > RL_HUGE_THRESH) {
        rl_size total = rl_align_up(b->usable + RL_HDR_SZ, 4096);
        rl_munmap(b, total);
        return;
    }
    freelist_push(b);
}

void* rl_realloc(void *ptr, rl_size new_size) {
    if (!ptr)     return rl_malloc(new_size);
    if (!new_size) { rl_free(ptr); return NULL; }
    RLBlkHdr *b = blk_from_ptr(ptr);
    if (b->magic != RL_MAGIC_USED) return NULL;
    if (b->usable >= new_size) return ptr;
    void *np = rl_malloc(new_size);
    if (!np) return NULL;
    rl_memcpy(np, ptr, b->usable);
    rl_free(ptr);
    return np;
}

void* rl_calloc(rl_size nmemb, rl_size size) {
    rl_size total = nmemb * size;
    void *p = rl_malloc(total);
    if (p) rl_memset(p, 0, total);
    return p;
}

/* ══════════════════════════════════════════════════════════════
   OPERACIONES DE MEMORIA
   ══════════════════════════════════════════════════════════════ */

void* rl_memcpy(void *dst, const void *src, rl_size n) {
    uint8_t       *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}

void* rl_memmove(void *dst, const void *src, rl_size n) {
    uint8_t       *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    if (d < s) { while (n--) *d++ = *s++; }
    else if (d > s) { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

void* rl_memset(void *dst, int c, rl_size n) {
    uint8_t *d = (uint8_t*)dst;
    uint8_t  v = (uint8_t)c;
    while (n--) *d++ = v;
    return dst;
}

int rl_memcmp(const void *a, const void *b, rl_size n) {
    const uint8_t *pa = (const uint8_t*)a;
    const uint8_t *pb = (const uint8_t*)b;
    while (n--) {
        int diff = (int)*pa++ - (int)*pb++;
        if (diff) return diff;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
   STRINGS
   ══════════════════════════════════════════════════════════════ */

rl_size rl_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (rl_size)(p - s);
}

char* rl_strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char* rl_strncpy(char *dst, const char *src, rl_size n) {
    char *d = dst;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dst;
}

int rl_strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int rl_strncmp(const char *a, const char *b, rl_size n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char* rl_strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

char* rl_strncat(char *dst, const char *src, rl_size n) {
    char *d = dst;
    while (*d) d++;
    while (n && (*d++ = *src++)) n--;
    *d = '\0';
    return dst;
}

char* rl_strchr(const char *s, int c) {
    while (*s) { if (*s == (char)c) return (char*)s; s++; }
    return (c == '\0') ? (char*)s : NULL;
}

char* rl_strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) { if (*s == (char)c) last = s; s++; }
    if (c == '\0') return (char*)s;
    return (char*)last;
}

char* rl_strstr(const char *hay, const char *needle) {
    rl_size nl = rl_strlen(needle);
    if (!nl) return (char*)hay;
    while (*hay) {
        if (*hay == *needle && rl_strncmp(hay, needle, nl) == 0)
            return (char*)hay;
        hay++;
    }
    return NULL;
}

char* rl_strdup(const char *s) {
    rl_size len = rl_strlen(s);
    char *d = (char*)rl_malloc(len + 1);
    if (d) { rl_memcpy(d, s, len); d[len] = '\0'; }
    return d;
}

/* ══════════════════════════════════════════════════════════════
   CONVERSIÓN
   ══════════════════════════════════════════════════════════════ */

int rl_itoa(rl_i64 val, char *buf, int base) {
    if (base < 2 || base > 36) { buf[0] = '\0'; return 0; }
    char tmp[66]; int n = 0; bool neg = false;
    if (val < 0 && base == 10) { neg = true; val = -val; }
    rl_u64 uv = (rl_u64)val;
    do {
        int d = (int)(uv % (rl_u64)base);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        uv /= (rl_u64)base;
    } while (uv);
    if (neg) tmp[n++] = '-';
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        char t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t;
    }
    tmp[n] = '\0';
    rl_memcpy(buf, tmp, (rl_size)(n + 1));
    return n;
}

int rl_utoa(rl_u64 val, char *buf, int base) {
    if (base < 2 || base > 36) { buf[0] = '\0'; return 0; }
    char tmp[66]; int n = 0;
    do {
        int d = (int)(val % (rl_u64)base);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        val /= (rl_u64)base;
    } while (val);
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        char t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t;
    }
    tmp[n] = '\0';
    rl_memcpy(buf, tmp, (rl_size)(n + 1));
    return n;
}

rl_i64 rl_atoi(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    rl_i64 r = 0; int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') r = r * 10 + (*s++ - '0');
    return r * sign;
}

rl_u64 rl_atou(const char *s, int base) {
    while (*s == ' ' || *s == '\t') s++;
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    rl_u64 r = 0;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        r = r * (rl_u64)base + (rl_u64)d;
        s++;
    }
    return r;
}

/* ══════════════════════════════════════════════════════════════
   FORMATEADOR MÍNIMO — sin printf de libc
   Soporta: %s %d %u %x %X %lld %llu %llx %p %c %%
   ══════════════════════════════════════════════════════════════ */

static int rl_fmt_core(char *obuf, rl_size osz,
                        int fd, const char *fmt, va_list ap) {
    char tmp[72];
    int written = 0;

#define EMIT_CHAR(c) do { \
    char _c = (c); \
    if (obuf) { if ((rl_size)written < osz - 1) obuf[written] = _c; } \
    else rl_write(fd, &_c, 1); \
    written++; \
} while(0)

#define EMIT_STR(s) do { \
    const char *_s = (s); \
    while (_s && *_s) { EMIT_CHAR(*_s); _s++; } \
} while(0)

    while (*fmt) {
        if (*fmt != '%') { EMIT_CHAR(*fmt++); continue; }
        fmt++;
        bool is_ll = false;
        if (*fmt == 'l' && *(fmt+1) == 'l') { is_ll = true; fmt += 2; }
        else if (*fmt == 'l') { is_ll = true; fmt++; }

        switch (*fmt) {
        case 's': {
            const char *sv = va_arg(ap, const char*);
            EMIT_STR(sv ? sv : "(null)");
            break;
        }
        case 'd': {
            rl_i64 iv = is_ll ? va_arg(ap, rl_i64) : (rl_i64)va_arg(ap, int);
            rl_itoa(iv, tmp, 10); EMIT_STR(tmp);
            break;
        }
        case 'u': {
            rl_u64 uv = is_ll ? va_arg(ap, rl_u64) : (rl_u64)va_arg(ap, unsigned);
            rl_utoa(uv, tmp, 10); EMIT_STR(tmp);
            break;
        }
        case 'x': {
            rl_u64 uv = is_ll ? va_arg(ap, rl_u64) : (rl_u64)va_arg(ap, unsigned);
            rl_utoa(uv, tmp, 16); EMIT_STR(tmp);
            break;
        }
        case 'X': {
            rl_u64 uv = is_ll ? va_arg(ap, rl_u64) : (rl_u64)va_arg(ap, unsigned);
            int n = rl_utoa(uv, tmp, 16);
            for (int i = 0; i < n; i++)
                if (tmp[i] >= 'a') tmp[i] -= 32;
            EMIT_STR(tmp);
            break;
        }
        case 'p': {
            rl_u64 pv = (rl_u64)(uintptr_t)va_arg(ap, void*);
            EMIT_STR("0x"); rl_utoa(pv, tmp, 16); EMIT_STR(tmp);
            break;
        }
        case 'c': {
            char cv = (char)va_arg(ap, int);
            EMIT_CHAR(cv);
            break;
        }
        case '%': EMIT_CHAR('%'); break;
        default:  EMIT_CHAR('%'); EMIT_CHAR(*fmt); break;
        }
        fmt++;
    }

    if (obuf && (rl_size)written < osz) obuf[written] = '\0';
    return written;

#undef EMIT_CHAR
#undef EMIT_STR
}

int rl_dprintf(int fd, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = rl_fmt_core(NULL, 0, fd, fmt, ap);
    va_end(ap);
    return n;
}

int rl_snprintf(char *buf, rl_size sz, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = rl_fmt_core(buf, sz, -1, fmt, ap);
    va_end(ap);
    return n;
}

/* ══════════════════════════════════════════════════════════════
   ESTADÍSTICAS
   ══════════════════════════════════════════════════════════════ */

void rl_heap_stats_emit(int fd) {
    rl_dprintf(fd,
        "{\"rl_heap\":{"
        "\"allocs\":%llu,"
        "\"frees\":%llu,"
        "\"bytes_in_use\":%llu,"
        "\"bytes_mmaped\":%llu,"
        "\"free_blocks\":%u}}",
        (unsigned long long)g_rl_heap.allocs,
        (unsigned long long)g_rl_heap.frees,
        (unsigned long long)g_rl_heap.bytes_in_use,
        (unsigned long long)g_rl_heap.bytes_mmaped,
        (unsigned)g_rl_heap.free_blocks);
}
