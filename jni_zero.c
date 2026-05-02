/* ============================================================
   RigCom v8.0 — src/jni_zero.c
   JNI-Zero: Universal Android Interop
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/jni_zero.h"
#include "../include/wsserver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Mapeo de tipos C → JNI/Java ── */
typedef struct { const char *c; const char *jni; const char *java; } TypeMap;

static const TypeMap TYPE_MAP[] = {
    {"int",           "jint",        "int"},
    {"unsigned int",  "jint",        "int"},
    {"int32_t",       "jint",        "int"},
    {"uint32_t",      "jint",        "int"},
    {"long",          "jlong",       "long"},
    {"int64_t",       "jlong",       "long"},
    {"uint64_t",      "jlong",       "long"},
    {"float",         "jfloat",      "float"},
    {"double",        "jdouble",     "double"},
    {"bool",          "jboolean",    "boolean"},
    {"char",          "jbyte",       "byte"},
    {"short",         "jshort",      "short"},
    {"void",          "void",        "void"},
    {"const char*",   "jstring",     "String"},
    {"char*",         "jstring",     "String"},
    {"float*",        "jfloatArray", "float[]"},
    {"int*",          "jintArray",   "int[]"},
    {"double*",       "jdoubleArray","double[]"},
    {NULL, NULL, NULL}
};

static void map_type(const char *c_type, char *jni_out, char *java_out) {
    /* Normalizar: quitar const, espacios extras */
    char norm[64] = {0};
    const char *src = c_type;
    while (*src == ' ' || *src == '\t') src++;
    if (strncmp(src, "const ", 6) == 0) src += 6;
    strncpy(norm, src, 63);
    /* Quitar espacios trailing */
    int len = (int)strlen(norm);
    while (len > 0 && (norm[len-1] == ' ' || norm[len-1] == '\t'))
        norm[--len] = '\0';

    for (int i = 0; TYPE_MAP[i].c; i++) {
        if (strcmp(norm, TYPE_MAP[i].c) == 0) {
            strncpy(jni_out, TYPE_MAP[i].jni,  31);
            strncpy(java_out, TYPE_MAP[i].java, 31);
            return;
        }
    }
    /* Fallback: puntero → long (handle opaco) */
    if (strchr(norm, '*')) {
        strncpy(jni_out,  "jlong", 31);
        strncpy(java_out, "long",  31);
    } else {
        strncpy(jni_out,  "jlong", 31);
        strncpy(java_out, "long",  31);
    }
}

/* ── Tokenizador minimal para extraer firmas ── */
static char* skip_ws(char *p) {
    while (*p && (*p==' '||*p=='\t'||*p=='\r'||*p=='\n')) p++;
    return p;
}

/* Extrae el tipo de retorno y nombre de la función de la línea siguiente a [[rigcom::export]].
   Línea ejemplo: "float* rig_audio_process(float *in, int n, float gain)"
   También soporta:  "void rig_init(void)"                              */
static JniExport* parse_export_line(const char *line, uint32_t lineno) {
    JniExport *ex = calloc(1, sizeof(JniExport));
    if (!ex) return NULL;
    ex->source_line = lineno;

    /* Copiar para modificar */
    char buf[1024];
    strncpy(buf, line, sizeof(buf)-1);

    /* Quitar atributos y calificadores comunes */
    char *p = buf;
    while (*p == ' ' || *p == '\t') p++;

    /* Encontrar paréntesis de apertura */
    char *paren = strchr(p, '(');
    if (!paren) { free(ex); return NULL; }

    /* El nombre de la función está justo antes del '(' */
    char *name_end = paren;
    while (name_end > p && (*name_end==' '||*name_end=='('||*name_end=='\t'))
        name_end--;
    char *name_start = name_end;
    while (name_start > p && (isalnum((unsigned char)*name_start) ||
                               *name_start == '_'))
        name_start--;
    name_start++;

    int nlen = (int)(name_end - name_start + 1);
    if (nlen <= 0 || nlen >= 128) { free(ex); return NULL; }
    strncpy(ex->fn_name, name_start, (size_t)nlen);

    /* Tipo de retorno: todo antes del nombre */
    char ret_buf[64] = {0};
    int rlen = (int)(name_start - p);
    if (rlen > 0 && rlen < 64) {
        strncpy(ret_buf, p, (size_t)rlen);
        /* Trim */
        int rl = (int)strlen(ret_buf);
        while (rl>0 && (ret_buf[rl-1]==' '||ret_buf[rl-1]=='\t'||ret_buf[rl-1]=='*'))
            ret_buf[--rl] = '\0';
        /* Reconstruir con * si era puntero */
        if (strchr(p, '*') && (strrchr(p,'*') < name_start))
            strncat(ret_buf, "*", sizeof(ret_buf)-strlen(ret_buf)-1);
    } else {
        strncpy(ret_buf, "void", 63);
    }
    strncpy(ex->ret_c, ret_buf, sizeof(ex->ret_c)-1);
    map_type(ex->ret_c, ex->ret_jni, ex->ret_java);

    /* Parámetros: contenido entre '(' y ')' */
    char *close = strchr(paren, ')');
    if (!close) { free(ex); return NULL; }
    char params_buf[512] = {0};
    int plen = (int)(close - paren - 1);
    if (plen > 0) strncpy(params_buf, paren+1, (size_t)plen);

    /* Parsear parámetros separados por coma */
    if (strcmp(params_buf, "void") != 0 && strlen(params_buf) > 0) {
        char *tok = strtok(params_buf, ",");
        while (tok && ex->n_params < 16) {
            tok = skip_ws(tok);
            JniParam *pm = &ex->params[ex->n_params];
            /* Último token es el nombre, el resto es el tipo */
            char *last_space = strrchr(tok, ' ');
            if (!last_space) last_space = strrchr(tok, '\t');
            if (last_space) {
                strncpy(pm->name, skip_ws(last_space), sizeof(pm->name)-1);
                /* Eliminar * del nombre si lo tiene */
                char *ast = strchr(pm->name, '*');
                if (ast) *ast = '\0';
                int tlen = (int)(last_space - tok);
                if (tlen > 0 && tlen < 63)
                    strncpy(pm->c_type, tok, (size_t)tlen);
                else
                    strncpy(pm->c_type, tok, sizeof(pm->c_type)-1);
            } else {
                snprintf(pm->name, sizeof(pm->name), "p%u", ex->n_params);
                strncpy(pm->c_type, tok, sizeof(pm->c_type)-1);
            }
            map_type(pm->c_type, pm->jni_type, pm->java_type);
            ex->n_params++;
            tok = strtok(NULL, ",");
        }
    }
    return ex;
}

/* ── Escaneo del archivo fuente ── */
JniZeroResult* jni_zero_scan(const char *src_path, const char *pkg_name) {
    FILE *f = fopen(src_path, "r");
    if (!f) return NULL;

    JniZeroResult *r = calloc(1, sizeof(JniZeroResult));
    if (!r) { fclose(f); return NULL; }

    const char *base = strrchr(src_path, '/');
    base = base ? base+1 : src_path;
    strncpy(r->pkg_name,   pkg_name ? pkg_name : "com.rigcom.app",
            sizeof(r->pkg_name)-1);
    strncpy(r->class_name, "RigBridge", sizeof(r->class_name)-1);

    char line[1024];
    uint32_t lineno = 0;
    bool next_is_export = false;

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        /* Quitar newline */
        size_t ll = strlen(line);
        while (ll > 0 && (line[ll-1]=='\n'||line[ll-1]=='\r')) line[--ll]='\0';

        if (strstr(line, "[[rigcom::export]]") ||
            strstr(line, "RIGCOM_EXPORT") ||
            strstr(line, "__attribute__((rigcom_export))")) {
            next_is_export = true;
            continue;
        }
        if (next_is_export) {
            next_is_export = false;
            /* Ignorar líneas vacías y comentarios */
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (!*p || *p == '/' || *p == '#') continue;

            JniExport *ex = parse_export_line(line, lineno);
            if (ex) {
                ex->next   = r->exports;
                r->exports = ex;
                r->n_exports++;
            }
        }
    }
    fclose(f);
    return r;
}

/* ── Generación de _JNI_GLUE.c ── */
bool jni_zero_gen_c(JniZeroResult *r, const char *out_dir, const char *orig_src) {
    char path[512];
    snprintf(path, sizeof(path), "%s/_JNI_GLUE.c", out_dir);
    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f,
        "/* ============================================================\n"
        "   _JNI_GLUE.c — Auto-generado por RigCom v8.0 JNI-Zero\n"
        "   NO EDITAR MANUALMENTE — Regenerar con: rigcom jni_export\n"
        "   Paquete: %s\n"
        "   Clase:   %s\n"
        "   Fuente:  %s\n"
        "   ============================================================ */\n"
        "#include <jni.h>\n"
        "#include <string.h>\n"
        "#include <stdlib.h>\n"
        "#include \"%s\"\n\n",
        r->pkg_name, r->class_name, orig_src, orig_src);

    /* Construir prefijo JNI: com.rigcom.app → Java_com_rigcom_app_RigBridge */
    char jni_prefix[256] = "Java_";
    char pkg_flat[128];
    strncpy(pkg_flat, r->pkg_name, sizeof(pkg_flat)-1);
    for (char *p = pkg_flat; *p; p++) if (*p == '.') *p = '_';
    strncat(jni_prefix, pkg_flat, sizeof(jni_prefix)-strlen(jni_prefix)-1);
    strncat(jni_prefix, "_",      sizeof(jni_prefix)-strlen(jni_prefix)-1);
    strncat(jni_prefix, r->class_name, sizeof(jni_prefix)-strlen(jni_prefix)-1);

    for (JniExport *ex = r->exports; ex; ex = ex->next) {
        /* Firma JNI */
        fprintf(f, "JNIEXPORT %s JNICALL\n", ex->ret_jni);
        fprintf(f, "%s_%s(JNIEnv *env, jclass clazz", jni_prefix, ex->fn_name);
        for (uint32_t i = 0; i < ex->n_params; i++)
            fprintf(f, ", %s %s", ex->params[i].jni_type, ex->params[i].name);
        fprintf(f, ")\n{\n");
        fprintf(f, "    (void)env; (void)clazz;\n");

        /* Conversiones de entrada: jstring → const char* */
        for (uint32_t i = 0; i < ex->n_params; i++) {
            JniParam *pm = &ex->params[i];
            if (strcmp(pm->jni_type, "jstring") == 0) {
                fprintf(f,
                    "    const char *_c_%s = (*env)->GetStringUTFChars(env, %s, NULL);\n",
                    pm->name, pm->name);
            }
        }

        /* Llamada a la función C real */
        bool has_ret = strcmp(ex->ret_jni, "void") != 0;
        if (has_ret) fprintf(f, "    %s _ret = ", ex->ret_c);
        else         fprintf(f, "    ");
        fprintf(f, "%s(", ex->fn_name);
        for (uint32_t i = 0; i < ex->n_params; i++) {
            if (i) fprintf(f, ", ");
            JniParam *pm = &ex->params[i];
            if (strcmp(pm->jni_type, "jstring") == 0)
                fprintf(f, "_c_%s", pm->name);
            else
                fprintf(f, "%s", pm->name);
        }
        fprintf(f, ");\n");

        /* Liberación de strings */
        for (uint32_t i = 0; i < ex->n_params; i++) {
            JniParam *pm = &ex->params[i];
            if (strcmp(pm->jni_type, "jstring") == 0) {
                fprintf(f,
                    "    (*env)->ReleaseStringUTFChars(env, %s, _c_%s);\n",
                    pm->name, pm->name);
            }
        }

        /* Retorno */
        if (has_ret) {
            if (strcmp(ex->ret_jni, "jstring") == 0)
                fprintf(f, "    return (*env)->NewStringUTF(env, _ret ? _ret : \"\");\n");
            else
                fprintf(f, "    return (%s)_ret;\n", ex->ret_jni);
        }
        fprintf(f, "}\n\n");
    }
    fclose(f);
    return true;
}

/* ── Generación de RigBridge.java ── */
bool jni_zero_gen_java(JniZeroResult *r, const char *out_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/RigBridge.java", out_dir);
    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f,
        "// ============================================================\n"
        "// RigBridge.java — Auto-generado por RigCom v8.0 JNI-Zero\n"
        "// NO EDITAR MANUALMENTE\n"
        "// ============================================================\n"
        "package %s;\n\n"
        "public class %s {\n"
        "    static {\n"
        "        System.loadLibrary(\"main\");\n"
        "    }\n\n",
        r->pkg_name, r->class_name);

    for (JniExport *ex = r->exports; ex; ex = ex->next) {
        fprintf(f, "    public static native %s %s(",
                ex->ret_java, ex->fn_name);
        for (uint32_t i = 0; i < ex->n_params; i++) {
            if (i) fprintf(f, ", ");
            fprintf(f, "%s %s", ex->params[i].java_type, ex->params[i].name);
        }
        fprintf(f, ");\n");
    }
    fprintf(f, "}\n");
    fclose(f);
    return true;
}

void jni_zero_free(JniZeroResult *r) {
    if (!r) return;
    JniExport *e = r->exports;
    while (e) { JniExport *n = e->next; free(e); e = n; }
    free(r);
}

/* ── Thread async ── */
void* jni_zero_thread(void *arg) {
    JniZeroArg *a = (JniZeroArg*)arg;
    WsServer *srv = (WsServer*)a->srv;
    ws_broadcastf(srv,
        "{\"ev\":\"jni_scan_start\",\"file\":\"%s\"}", a->file);

    JniZeroResult *r = jni_zero_scan(a->file, a->pkg);
    if (!r || r->n_exports == 0) {
        ws_broadcastf(srv,
            "{\"ev\":\"jni_scan_done\",\"ok\":false,"
            "\"msg\":\"No se encontraron [[rigcom::export]] en el archivo.\","
            "\"hint\":\"Anota tus funciones con [[rigcom::export]] antes de la declaracion.\"}");
        if (r) jni_zero_free(r);
        free(a);
        return NULL;
    }

    const char *out = a->out_dir[0] ? a->out_dir : ".";
    bool ok_c    = jni_zero_gen_c   (r, out, a->file);
    bool ok_java = jni_zero_gen_java(r, out);

    ws_broadcastf(srv,
        "{\"ev\":\"jni_scan_done\","
        "\"ok\":true,"
        "\"n_exports\":%u,"
        "\"pkg\":\"%s\","
        "\"jni_c\":\"%s/_JNI_GLUE.c\","
        "\"java\":\"%s/RigBridge.java\","
        "\"ok_c\":%s,\"ok_java\":%s}",
        r->n_exports, r->pkg_name, out, out,
        ok_c ? "true":"false",
        ok_java ? "true":"false");

    /* Emitir cada export para el UI */
    for (JniExport *ex = r->exports; ex; ex = ex->next) {
        ws_broadcastf(srv,
            "{\"ev\":\"jni_export\","
            "\"fn\":\"%s\","
            "\"ret_c\":\"%s\","
            "\"ret_java\":\"%s\","
            "\"n_params\":%u,"
            "\"line\":%u}",
            ex->fn_name, ex->ret_c, ex->ret_java,
            ex->n_params, ex->source_line);
    }

    jni_zero_free(r);
    free(a);
    return NULL;
}
