/* ============================================================
   RigCom v8.0 — include/riglib.h
   RigLib: stdlib ARM64 sin dependencias Termux / Android.
   Usa syscalls Linux directos (SYS_mmap, SYS_write, …) y
   un asignador de arena propio; no enlaza contra bionic ni
   Termux libc.  Compatible ARM64 + x86-64 Linux.
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef RIGLIB_H
#define RIGLIB_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Tipos básicos ─────────────────────────────────────────── */
typedef  int8_t   rl_i8;
typedef  int16_t  rl_i16;
typedef  int32_t  rl_i32;
typedef  int64_t  rl_i64;
typedef uint8_t   rl_u8;
typedef uint16_t  rl_u16;
typedef uint32_t  rl_u32;
typedef uint64_t  rl_u64;
typedef size_t    rl_size;

/* ── Constantes ────────────────────────────────────────────── */
#define RL_STDIN   0
#define RL_STDOUT  1
#define RL_STDERR  2

/* open() flags portables */
#define RL_O_RDONLY  0x0000
#define RL_O_WRONLY  0x0001
#define RL_O_RDWR    0x0002
#define RL_O_CREAT   0x0040
#define RL_O_TRUNC   0x0200
#define RL_O_APPEND  0x0400
#define RL_O_CLOEXEC 0x80000

/* mmap prot/flags */
#define RL_PROT_READ   0x1
#define RL_PROT_WRITE  0x2
#define RL_MAP_PRIVATE 0x02
#define RL_MAP_ANON    0x20
#define RL_MAP_FAILED  ((void*)-1)

/* ── Estadísticas del heap ─────────────────────────────────── */
typedef struct {
    rl_u64 allocs;
    rl_u64 frees;
    rl_u64 bytes_in_use;
    rl_u64 bytes_mmaped;
    rl_u32 free_blocks;
} RLHeapStats;

extern RLHeapStats g_rl_heap;

/* ── Memoria ───────────────────────────────────────────────── */

/* malloc/free/realloc propios — no llaman a libc */
void*  rl_malloc  (rl_size size);
void   rl_free    (void *ptr);
void*  rl_realloc (void *ptr, rl_size new_size);
void*  rl_calloc  (rl_size nmemb, rl_size size);

/* Operaciones de memoria de bajo nivel */
void*  rl_memcpy  (void *dst, const void *src, rl_size n);
void*  rl_memmove (void *dst, const void *src, rl_size n);
void*  rl_memset  (void *dst, int c, rl_size n);
int    rl_memcmp  (const void *a, const void *b, rl_size n);

/* Syscall mmap/munmap directos */
void*  rl_mmap   (void *addr, rl_size len, int prot, int flags,
                   int fd, rl_i64 offset);
int    rl_munmap (void *addr, rl_size len);

/* ── Strings ───────────────────────────────────────────────── */
rl_size rl_strlen  (const char *s);
char*   rl_strcpy  (char *dst, const char *src);
char*   rl_strncpy (char *dst, const char *src, rl_size n);
int     rl_strcmp  (const char *a, const char *b);
int     rl_strncmp (const char *a, const char *b, rl_size n);
char*   rl_strcat  (char *dst, const char *src);
char*   rl_strncat (char *dst, const char *src, rl_size n);
char*   rl_strchr  (const char *s, int c);
char*   rl_strrchr (const char *s, int c);
char*   rl_strstr  (const char *hay, const char *needle);
char*   rl_strdup  (const char *s);   /* usa rl_malloc */

/* ── I/O (syscalls directos) ───────────────────────────────── */
rl_i64 rl_write  (int fd, const void *buf, rl_size len);
rl_i64 rl_read   (int fd, void *buf, rl_size len);
int    rl_open   (const char *path, int flags, int mode);
int    rl_close  (int fd);
rl_i64 rl_lseek  (int fd, rl_i64 offset, int whence);
__attribute__((noreturn))
void   rl_exit   (int code);
int    rl_getpid (void);
int    rl_mkdir  (const char *path, int mode);
int    rl_unlink (const char *path);
int    rl_stat   (const char *path, void *statbuf);

/* ── Conversión ────────────────────────────────────────────── */

/* Entero → string.  Devuelve longitud escrita (sin '\0').
   base: 10 = decimal, 16 = hex, 2 = binario. */
int  rl_itoa  (rl_i64 val, char *buf, int base);
int  rl_utoa  (rl_u64 val, char *buf, int base);

/* String → entero */
rl_i64 rl_atoi (const char *s);
rl_u64 rl_atou (const char *s, int base);

/* Formateador mínimo para fd (no usa printf de libc).
   Soporta: %s %d %u %x %lld %llu %llx %p %c %%.
   Devuelve bytes escritos. */
int rl_dprintf (int fd, const char *fmt, ...);

/* Versión a buffer en vez de fd */
int rl_snprintf (char *buf, rl_size sz, const char *fmt, ...);

/* ── Estadísticas del heap ─────────────────────────────────── */
void rl_heap_stats_emit (int fd);  /* escribe JSON en fd */

#endif /* RIGLIB_H */
