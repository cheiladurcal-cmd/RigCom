/* ============================================================
   RigCom v8.0 — include/rigcom.h
   Public API — Intelligent Compiler Platform
   Author: Richard Felipe Urbina
   Fases: Rayos X · NEON · RigScript · Oracle · Sandbox · SafeStack
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef RIGCOM_H
#define RIGCOM_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define RIGCOM_VERSION       "8.0.0"
#define RIGCOM_VERSION_MAJOR  8
#define RIGCOM_VERSION_MINOR  0
#define RIGCOM_VERSION_PATCH  0
#define RIGCOM_BUILD_DATE     __DATE__
#define RIGCOM_BUILD_TIME     __TIME__
#define RIGCOM_PHI            1.6180339887498948482
#define RIGCOM_SCHUMANN       7.83

/* Feature flags (all enabled in v6.0) */
#define RIGCOM_FEAT_XRAY      1   /* Motor de Rayos X (ptrace debugger)  */
#define RIGCOM_FEAT_NEON      1   /* SIMD auto-vectorización NEON         */
#define RIGCOM_FEAT_CACHE     1   /* SHA-256 content-addressed cache      */
#define RIGCOM_FEAT_RIGSCRIPT 1   /* RigScript language frontend          */
#define RIGCOM_FEAT_ORACLE    1   /* Oracle inter-procedural analysis     */
#define RIGCOM_FEAT_LSP       1   /* Language Server Protocol embedded    */
#define RIGCOM_FEAT_SANDBOX   1   /* RigBridge namespace sandbox          */
#define RIGCOM_FEAT_SAFESTACK 1   /* SafeStack dual-stack hardening       */

typedef struct RigCtx      RigCtx;
typedef struct RigErrorLog RigErrorLog;

int rigcom_build    (const char *config_path, bool native);
int rigcom_check    (const char *config_path, const char *source_file);
int rigcom_run      (const char *config_path);
int rigcom_ui       (uint16_t port);
int rigcom_bootstrap(const char *config_path);
int rigcom_bench    (const char *config_path);
int rigcom_info     (void);
int rigcom_compile_file(const char *src_path, const char *out_path,
                        bool native, const char *optimize);
uint32_t rigcom_tokenize(const char *src, size_t len, const char *file,
                          RigErrorLog *log);
int rigcom_rigscript(const char *src_path);
int rigcom_debug    (const char *program, bool stop_at_entry);

#endif /* RIGCOM_H */
