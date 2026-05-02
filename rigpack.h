/* ============================================================
   RigCom v8.0 — include/rigpack.h
   RigPack: Zero-Config Library Manager for ARM64
   Descarga · compila · linkea · actualiza rigcom.toml
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef RIGPACK_H
#define RIGPACK_H

#include "wsserver.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Métodos de instalación (por orden de preferencia) ─────── */
typedef enum {
    RPKG_PKGCONF = 0, /* ya disponible vía pkg-config           */
    RPKG_TERMUX,      /* termux: pkg install lib-xxx             */
    RPKG_APT,         /* debian/ubuntu: apt-get install lib-xxx  */
    RPKG_SOURCE,      /* fallback: descarga y compila source     */
} RigPackMethod;

/* ── Argumento para thread de instalación ──────────────────── */
typedef struct {
    WsServer *srv;
    char      lib[256];   /* nombre solicitado (ej: "sqlite3")  */
} RigPackArg;

/* ── Argumento para thread de listado ──────────────────────── */
typedef struct {
    WsServer *srv;
} RigPackListArg;

/* ─── API pública ───────────────────────────────────────────── */

/* Thread: instala lib → emite rigpack_progress / rigpack_done */
void *rigpack_install_thread(void *arg);

/* Emite evento rigpack_list con librerías instaladas/disponibles */
void  rigpack_emit_list(WsServer *srv);

/* Resuelve flags de compilación para una lib → malloc'd o NULL */
char *rigpack_resolve_flags(const char *lib);

/* Actualiza [libs] en rigcom.toml con la nueva lib */
bool  rigpack_update_toml(const char *lib,
                          const char *cflags,
                          const char *ldflags);

#endif /* RIGPACK_H */
