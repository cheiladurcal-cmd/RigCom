/* ============================================================
   RigCom v8.0 — include/apkpack.h
   APK Packager: Manifest v2 · V1/V2/V3 · Iconos Adaptativos
   ============================================================ */
#ifndef APKPACK_H
#define APKPACK_H
#include <stdbool.h>
#include <stddef.h>
#include "rigctx.h"

bool  apk_build        (RigCtx *ctx, const char *so_path, const char *out_apk);
bool  apk_unpack       (RigCtx *ctx, const char *apk_path, const char *out_dir);
bool  apk_check_tools  (RigCtx *ctx);
bool  apk_gen_icons    (RigCtx *ctx);
bool  apk_list_contents(const char *dir, char *json_out, size_t max_sz);
char *apk_read_file    (const char *dir, const char *rel_path, size_t *out_sz);
bool  apk_write_file   (const char *dir, const char *rel_path,
                        const char *content, size_t len);
bool  apk_repack       (RigCtx *ctx, const char *unpacked_dir, const char *out_apk);

#endif /* APKPACK_H */
