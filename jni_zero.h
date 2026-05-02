/* ============================================================
   RigCom v8.0 — include/jni_zero.h
   JNI-Zero: Universal Android Interop
   Escanea [[rigcom::export]] en código C → genera:
     • _JNI_GLUE.c  (wrappers JNI con firma correcta)
     • RigBridge.java (clase Java que llama a los C exports)
   El usuario escribe C puro. RigCom genera todo lo demás.
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef JNI_ZERO_H
#define JNI_ZERO_H
#include <stdbool.h>
#include <stdint.h>

/* Parámetro de una función exportada */
typedef struct JniParam {
    char c_type[64];    /* "int", "float*", "const char*", ... */
    char name[64];
    char jni_type[32];  /* "jint", "jfloatArray", "jstring", ... */
    char java_type[32]; /* "int", "float[]", "String", ...       */
} JniParam;

/* Función anotada con [[rigcom::export]] */
typedef struct JniExport {
    char      fn_name[128];
    char      ret_c[64];
    char      ret_jni[32];
    char      ret_java[32];
    JniParam  params[16];
    uint32_t  n_params;
    uint32_t  source_line;
    struct JniExport *next;
} JniExport;

/* Resultado del escaneo */
typedef struct {
    JniExport *exports;
    uint32_t   n_exports;
    char       pkg_name[128];   /* com.rigcom.PROJECT */
    char       class_name[64];  /* RigBridge */
} JniZeroResult;

/* ── API pública ── */

/* Escanea un archivo fuente buscando [[rigcom::export]] */
JniZeroResult* jni_zero_scan  (const char *src_path,
                                const char *pkg_name);

/* Genera _JNI_GLUE.c en out_dir */
bool           jni_zero_gen_c (JniZeroResult *r,
                                const char *out_dir,
                                const char *orig_src);

/* Genera RigBridge.java en out_dir */
bool           jni_zero_gen_java(JniZeroResult *r,
                                  const char *out_dir);

/* Libera resultado */
void           jni_zero_free  (JniZeroResult *r);

/* Thread async: escanea + genera + emite resultados WS */
typedef struct {
    char  file[512];
    char  pkg[128];
    char  out_dir[512];
    void *srv;   /* WsServer* */
} JniZeroArg;
void* jni_zero_thread(void *arg);

#endif /* JNI_ZERO_H */
