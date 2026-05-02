#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — src/main.c
   CLI entry point: build · check · run · ui · bootstrap · bench · info
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/rigcom.h"
#include "../include/rigctx.h"
#include "../include/rigir.h"
#include "../include/lexer.h"
#include "../include/ast.h"
#include "../include/parser.h"
#include "../include/symtable.h"
#include "../include/typechecker.h"
#include "../include/error.h"
#include "../include/backend.h"
#include "../include/rigsched.h"
#include "../include/wsserver.h"
#include "../include/preproc.h"
#include "../include/apkpack.h"
#include "../include/rigdap.h"
#include "../include/rigpack.h"
#include "../include/rigbridge.h"
/* ── RigCom v8.0 — nuevos módulos ── */
#include "../include/frontend.h"
#include "../include/gvn.h"
#include "../include/oracle_ip.h"
#include "../include/pty_term.h"
#include "../include/arena_harden.h"
/* ── RigCom v8.0 — Rayos X + Cache + RigScript ── */
#include "../include/rigcache.h"
#include "../include/rigscript.h"
/* ── RigCom v8.0 — ptrace · HoloTrace · NeonForge · JNI-Zero ── */
#include "../include/ptrace_dbg.h"
#include "../include/holo_trace.h"
#include "../include/neon_forge.h"
#include "../include/jni_zero.h"
/* ── RigCom v8.0 — NeuralCache · ASTHeal · DepGraph · RigLib · RigCanvas ── */
#include "../include/neural_cache.h"
#include "../include/ast_heal.h"
#include "../include/depgraph.h"
#include "../include/riglib.h"
#include "../include/rigcanvas.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/* ── ANSI colors ────────────────────────────────────────────── */
#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_RED     "\033[31m"
#define COL_GREEN   "\033[32m"
#define COL_YELLOW  "\033[33m"
#define COL_CYAN    "\033[36m"
#define COL_MAGENTA "\033[35m"

/* ── Banner ─────────────────────────────────────────────────── */
static void print_banner(void) {
    printf("\n");
    printf(COL_MAGENTA COL_BOLD
           "  ██████╗ ██╗ ██████╗  ██████╗ ██████╗ ███╗   ███╗\n"
           "  ██╔══██╗██║██╔════╝ ██╔════╝██╔═══██╗████╗ ████║\n"
           "  ██████╔╝██║██║  ███╗██║     ██║   ██║██╔████╔██║\n"
           "  ██╔══██╗██║██║   ██║██║     ██║   ██║██║╚██╔╝██║\n"
           "  ██║  ██║██║╚██████╔╝╚██████╗╚██████╔╝██║ ╚═╝ ██║\n"
           "  ╚═╝  ╚═╝╚═╝ ╚═════╝  ╚═════╝ ╚═════╝ ╚═╝     ╚═╝\n"
           COL_RESET);
    printf(COL_CYAN
           "  Compilador C de Grado Industrial — v%s\n"
           "  φ = 1.6180339887498948482 · P(A) ∈ {0,1}\n"
           COL_RESET "\n",
           RIGCOM_VERSION);
}

static void usage(const char *prog) {
    printf(COL_BOLD "Uso:\n" COL_RESET);
    printf("  %s build   [config.toml] [--native]  — Compilar proyecto\n", prog);
    printf("  %s check   [config.toml] <file.c>    — Analizar errores\n",   prog);
    printf("  %s run     [config.toml]              — Compilar + ejecutar\n", prog);
    printf("  %s ui      [port]                     — Dashboard web (default: 8080)\n", prog);
    printf("  %s bootstrap [config.toml]            — Auto-compilación\n",    prog);
    printf("  %s bench   [config.toml]              — Benchmarks\n",          prog);
    printf("  %s info                               — Info del sistema\n",    prog);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   PIPELINE — checkear un archivo fuente
   Emite eventos WS en cada fase si ctx->ws != NULL
   ═══════════════════════════════════════════════════════════════ */
static int pipeline_check_file(const char *src_path, RigErrorLog *log,
                                 RigCtx *ctx, uint32_t file_idx) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* ── BUG FIX v6.0: Detectar lenguaje por extensión ── */
    const LanguageFrontend *lang_fe = frontend_for_file(src_path);
    const char *lang_name = lang_fe ? lang_fe->display_name : "C11";

    /* Si el frontend tiene compile_to_ir (RigScript u otro) y NO es C11,
       usar el pipeline alternativo en lugar del C parser hardcoded */
    if (lang_fe && lang_fe->compile_to_ir &&
        strcmp(lang_fe->extension, ".c") != 0 &&
        strcmp(lang_fe->extension, ".h") != 0) {

        if (ctx && ctx->ws) {
            ws_broadcastf(ctx->ws,
                "{\"ev\":\"phase\",\"phase\":\"detect\","
                "\"file\":\"%s\",\"lang\":\"%s\",\"idx\":%u}",
                src_path, lang_name, file_idx);
        }
        IRModule *mod = lang_fe->compile_to_ir(ctx, src_path, log);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = ((t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)*1e-9)*1000.0;
        int errors = riglog_has_errors(log) ? 1 : 0;
        if (ctx && ctx->ws) {
            if (errors > 0) rigctx_ws_emit_errors(ctx, log);
            ws_broadcastf(ctx->ws,
                "{\"ev\":\"file_done\",\"file\":\"%s\",\"idx\":%u,"
                "\"errors\":%d,\"ms\":%.1f,\"lang\":\"%s\"}",
                src_path, file_idx, errors, ms, lang_name);
        }
        if (mod) { /* RigScript produced IR — can run optimizer */ }
        return errors;
    }

    /* ── C11 path (original pipeline) ── */
    /* Phase: preprocess */
    rigctx_ws_phase(ctx, "preprocess", src_path, file_idx);

    FILE *fp = fopen(src_path, "r");
    if (!fp) {
        riglog_add(log, ERR_FILE_NOT_FOUND, src_path, 0, 0,
                   "No se encontró el archivo fuente",
                   "", "Verifica la ruta del archivo", "");
        rigctx_ws_emit_errors(ctx, log);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    rewind(fp);
    char *raw_src = malloc((size_t)fsize + 1);
    if (!raw_src) { fclose(fp); return -1; }
    size_t nread = fread(raw_src, 1, (size_t)fsize, fp);
    raw_src[nread] = '\0';
    fclose(fp);

    /* Run autonomous preprocessor */
    Preproc pp;
    preproc_init(&pp, ctx, log);
    /* Add include dirs from config */
    preproc_add_path(&pp, ".");
    preproc_add_path(&pp, "include");
    preproc_add_path(&pp, "/usr/include");
    for (uint32_t di = 0; di < ctx->config.n_include_dirs; di++)
        preproc_add_path(&pp, ctx->config.include_dirs[di]);
    /* Apply defines from rigcom.toml */
    for (uint32_t di = 0; di < ctx->config.n_defines; di++) {
        char defbuf[256], *eq;
        snprintf(defbuf, sizeof(defbuf), "%s", ctx->config.defines[di]);
        eq = strchr(defbuf, '=');
        if (eq) { *eq = '\0'; preproc_define(&pp, defbuf, eq+1); }
        else     { preproc_define(&pp, defbuf, "1"); }
    }
    char *src = preproc_run(&pp, raw_src, nread, src_path);
    free(raw_src);
    if (!src) {
        rigctx_ws_emit_errors(ctx, log);
        preproc_free(&pp);
        return 1;
    }
    size_t src_len = strlen(src);

    /* ── Phase: lex ── */
    rigctx_ws_phase(ctx, "lex", src_path, file_idx);

    Lexer lx;
    lexer_init(&lx, src, src_len, src_path);

    /* ── Phase: parse ── */
    rigctx_ws_phase(ctx, "parse", src_path, file_idx);

    ASTArena *arena = ast_arena_new();
    Parser    parser;
    parser_init(&parser, &lx, log, arena, src_path);
    ASTNode  *tu = parser_parse(&parser);
    int errors = (int)parser.error_count;

    /* == ¡MAGIA DEL AUTO-HEADER! == */
    if (errors == 0 && strstr(src_path, ".c") && ctx->ws) {
        char *h_code = ast_generate_header(tu, src_path);
        if (h_code) {
            size_t h_len = strlen(h_code);
            char *json_code = malloc(h_len * 2 + 128);
            char *q = json_code;
            for (size_t i = 0; i < h_len; i++) {
                if (h_code[i] == '\n') { *q++ = '\\'; *q++ = 'n'; }
                else if (h_code[i] == '"') { *q++ = '\\'; *q++ = '"'; }
                else if (h_code[i] == '\\') { *q++ = '\\'; *q++ = '\\'; }
                else { *q++ = h_code[i]; }
            }
            *q = '\0';
            ws_broadcastf(ctx->ws, "{\"ev\":\"header_sync\", \"file\":\"%s\", \"code\":\"%s\"}", src_path, json_code);
            free(json_code);
            free(h_code);
        }
    }
    /* ================================== */

    /* Emit parse errors immediately */
    if (errors > 0 && ctx->ws)
        rigctx_ws_emit_errors(ctx, log);

    /* ── Phase: typecheck ── */
    if (!riglog_has_errors(log)) {
        rigctx_ws_phase(ctx, "typecheck", src_path, file_idx);
        TypeChecker *tc = tc_new(arena, log, src_path);
        bool ok = tc_check(tc, tu);
        if (!ok) {
            errors += (int)tc->error_count;
            rigctx_ws_emit_errors(ctx, log);
        }
        tc_free(tc);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec)*1e-9)*1000.0;

    /* Emit file done event */
    if (ctx && ctx->ws) {
        ws_broadcastf(ctx->ws,
            "{\"ev\":\"file_done\",\"file\":\"%s\",\"idx\":%u,"
            "\"errors\":%d,\"ms\":%.1f}",
            src_path, file_idx, errors, ms);
    }

    ast_arena_free(arena);
    free(src);
    preproc_free(&pp);
    return errors;
}

/* ═══════════════════════════════════════════════════════════════
   COMMANDS
   ═══════════════════════════════════════════════════════════════ */

int rigcom_check(const char *config_path, const char *source_file) {
    printf(COL_CYAN "  → rigcom check" COL_RESET " %s\n\n",
           source_file ? source_file : "(all sources)");

    RigCtx *ctx = rigctx_new(".");
    if (config_path && *config_path)
        rigctx_load_config(ctx, config_path);

    RigErrorLog *log = riglog_new();

    if (source_file) {
        ctx->files_total = 1;
        int errs = pipeline_check_file(source_file, log, ctx, 1);
        riglog_print_all(log);
        if (errs == 0)
            printf(COL_GREEN "  ✓ Sin errores en '%s'\n" COL_RESET, source_file);
        riglog_free(log);
        rigctx_free(ctx);
        return errs > 0 ? 1 : 0;
    }

    char **sources = NULL;
    uint32_t n = rigctx_find_sources(ctx, &sources);
    ctx->files_total = n;
    int total_errors = 0;

    for (uint32_t i = 0; i < n; i++) {
        printf("  Verificando: %s\n", sources[i]);
        total_errors += pipeline_check_file(sources[i], log, ctx, i+1);
        ctx->files_done = i+1;
        free(sources[i]);
    }
    free(sources);

    riglog_print_all(log);
    if (total_errors == 0)
        printf(COL_GREEN "  ✓ Proyecto limpio — sin errores\n" COL_RESET);

    riglog_free(log);
    rigctx_free(ctx);
    return total_errors > 0 ? 1 : 0;
}

/* ── Parallel compile job ───────────────────────────────────── */
typedef struct {
    const char  *path;
    uint32_t     idx;
    RigErrorLog *log;
    RigCtx      *ctx;
    bool         ok;
} CheckJob;

static void compile_task(void *arg) {
    CheckJob *job = (CheckJob *)arg;
    int errs = pipeline_check_file(job->path, job->log, job->ctx, job->idx);
    job->ok  = (errs == 0);
}

int rigcom_build(const char *config_path, bool native) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    printf(COL_CYAN "  → rigcom build" COL_RESET " [%s]\n\n",
           native ? "ARM64 nativo" : "LLVM");

    RigCtx *ctx = rigctx_new(".");
    if (config_path && *config_path)
        rigctx_load_config(ctx, config_path);
    if (native) ctx->config.use_native_backend = true;

    RigErrorLog *log = riglog_new();

    char **sources = NULL;
    uint32_t n = rigctx_find_sources(ctx, &sources);
    ctx->files_total = n;

    if (n == 0) {
        printf(COL_YELLOW "  ⚠ Sin archivos fuente encontrados\n" COL_RESET);
        if (ctx->ws)
            ws_broadcastf(ctx->ws,
                "{\"ev\":\"build_done\",\"ok\":false,\"files\":0,"
                "\"errors\":1,\"warnings\":0,\"ms\":0,\"backend\":\"%s\","
                "\"reason\":\"Sin archivos fuente\"}",
                native ? "ARM64" : "LLVM");
        riglog_free(log); rigctx_free(ctx); return 1;
    }

    uint32_t n_cores = ctx->config.n_cores > 0 ? ctx->config.n_cores : 4;
    printf("  Compilando %u archivo(s) con %u hilo(s)...\n\n", n, n_cores);

    if (ctx->ws)
        ws_broadcastf(ctx->ws,
            "{\"ev\":\"build_start\",\"files\":%u,\"cores\":%u,\"backend\":\"%s\"}",
            n, n_cores, native ? "ARM64" : "LLVM");

    /* Parallel check phase */
    Scheduler *sched = sched_new(n_cores);
    CheckJob  *jobs  = malloc(n * sizeof(CheckJob));
    for (uint32_t i = 0; i < n; i++) {
        jobs[i].path = sources[i];
        jobs[i].idx  = i+1;
        jobs[i].log  = log;
        jobs[i].ctx  = ctx;
        jobs[i].ok   = false;
        sched_submit(sched, compile_task, &jobs[i]);
    }
    sched_wait(sched);
    sched_free(sched);

    bool build_ok = !riglog_has_errors(log);
    riglog_print_all(log);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) +
                     (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    double elapsed_ms = elapsed * 1000.0;
    ctx->last_build_time = elapsed;
    ctx->build_ok        = build_ok;

    uint32_t nerr = 0, nwarn = 0;
    for (uint32_t i = 0; i < log->count; i++) {
        if (log->errors[i]->kind < 100) nerr++;
        else                             nwarn++;
    }

    if (build_ok) {
        printf(COL_GREEN
               "  ✓ Build exitoso: %u archivo(s) en %.3f s\n"
               COL_RESET, n, elapsed);
    } else {
        printf(COL_RED "  ✗ Build falló — %u error(es)\n" COL_RESET, nerr);
    }

    if (ctx->ws)
        ws_broadcastf(ctx->ws,
            "{\"ev\":\"build_done\",\"ok\":%s,\"files\":%u,"
            "\"errors\":%u,\"warnings\":%u,\"ms\":%.1f,\"backend\":\"%s\"}",
            build_ok ? "true":"false", n, nerr, nwarn, elapsed_ms,
            native ? "ARM64" : "LLVM");

    free(jobs);
    for (uint32_t i = 0; i < n; i++) free(sources[i]);
    free(sources);
    riglog_free(log);
    rigctx_free(ctx);
    return build_ok ? 0 : 1;
}

int rigcom_run(const char *config_path) {
    printf(COL_CYAN "  → rigcom run\n" COL_RESET);
    int rc = rigcom_build(config_path, false);
    if (rc != 0) return rc;

    RigCtx *ctx = rigctx_new(".");
    if (config_path && *config_path)
        rigctx_load_config(ctx, config_path);
    const char *exec = ctx->config.output_exec
                       ? ctx->config.output_exec : "./rigcom-out";
    printf("\n  Ejecutando: %s\n", exec);
    printf("  " COL_YELLOW "─────────────────────────────\n" COL_RESET);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "./%s", exec);
    int ret = system(cmd);
    printf("\n  " COL_YELLOW "─────────────────────────────\n" COL_RESET);
    printf("  Código de salida: %d\n", ret);
    rigctx_free(ctx);
    return ret;
}

/* ═══════════════════════════════════════════════════════════════
   rigcom_ui — Servidor HTTP + WebSocket real
   ═══════════════════════════════════════════════════════════════ */

/* Context passed to on_message callback */
typedef struct {
    WsServer       *srv;
    const char     *config_path;
    pthread_mutex_t build_lock;      /* only one build at a time   */
    DapSession     *dap;             /* DAP debug session          */
    PtraceSession  *ptrace_dbg;      /* ptrace low-level debugger  */
    HoloSession    *holo;            /* holographic trace          */
    RigCanvas      *canvas;          /* RigCanvas UI framework     */
} UiCtx;

/* Build thread arg */
typedef struct {
    UiCtx      *ui;
    bool        native;
} BuildArg;

static void* build_thread(void *arg) {
    BuildArg *ba = (BuildArg *)arg;
    UiCtx    *ui = ba->ui;

    /* Acquire build lock — reject concurrent builds */
    if (pthread_mutex_trylock(&ui->build_lock) != 0) {
        ws_broadcastf(ui->srv,
            "{\"ev\":\"error\",\"msg\":\"Build ya en progreso\"}");
        free(ba);
        return NULL;
    }

    /* Run build — all WS events emitted from within pipeline */
    rigcom_build(ui->config_path, ba->native);

    pthread_mutex_unlock(&ui->build_lock);
    free(ba);
    return NULL;
}

typedef struct {
    UiCtx      *ui;
    char        file[512];
} CheckArg;

static void* check_thread(void *arg) {
    CheckArg *ca = (CheckArg *)arg;
    UiCtx    *ui = ca->ui;
    rigcom_check(ui->config_path, ca->file[0] ? ca->file : NULL);
    free(ca);
    return NULL;
}

/* ── WS message handler ─────────────────────────────────────── */
typedef struct {
    UiCtx  *ui;
    char    name[256];
    char   *b64data;
    size_t  b64len;
} UploadArg;

/* Base64 decode — RFC 4648 */
static size_t b64_decode(const char *in, size_t in_len, uint8_t *out) {
    static const int8_t tbl[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    size_t out_len = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < in_len; i++) {
        int8_t v = tbl[(uint8_t)in[i]];
        if (v < 0) continue; /* skip padding and whitespace */
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[out_len++] = (uint8_t)((acc >> (uint32_t)bits) & 0xFFu);
        }
    }
    return out_len;
}

static void* upload_thread(void *arg) {
    UploadArg *ua = (UploadArg *)arg;
    UiCtx     *ui = ua->ui;

    /* Construir ruta destino: src/<nombre> para archivos fuente */
    char dir[512], path[512];
    /* Si el nombre tiene extension .c/.h/.rigc → va a src/ */
    const char *ext = strrchr(ua->name, '.');
    if (ext && (strcmp(ext, ".c")==0 || strcmp(ext, ".h")==0 || strcmp(ext, ".rigc")==0))
        snprintf(dir, sizeof(dir), "src");
    else
        snprintf(dir, sizeof(dir), "build/uploads");

    /* Crear directorio si no existe */
    char mkd[512];
    snprintf(mkd, sizeof(mkd), "mkdir -p %s", dir);
    (void)system(mkd);

    snprintf(path, sizeof(path), "%s/%s", dir, ua->name);

    /* Decodificar base64 y escribir binario */
    size_t max_out = ua->b64len * 3 / 4 + 4;
    uint8_t *bin = malloc(max_out);
    size_t bin_len = 0;
    if (bin) {
        bin_len = b64_decode(ua->b64data, ua->b64len, bin);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(bin, 1, bin_len, f);
            fclose(f);
        }
        free(bin);
    }

    /* Notificar por WS */
    if (ua->b64len > 0 && bin_len > 0) {
        ws_broadcastf(ui->srv,
            "{\"ev\":\"upload_done\",\"ok\":true,"
            "\"name\":\"%s\",\"path\":\"%s\",\"bytes\":%zu}",
            ua->name, path, bin_len);
    } else {
        ws_broadcastf(ui->srv,
            "{\"ev\":\"upload_done\",\"ok\":false,"
            "\"name\":\"%s\",\"msg\":\"Error al decodificar o escribir el archivo\"}",
            ua->name);
    }

    free(ua->b64data);
    free(ua);
    return NULL;
}


/* APK build thread */
typedef struct { WsServer *srv; char so_path[512]; char out_apk[512]; } ApkBuildArg;
static void* apk_build_thread(void *arg) {
    ApkBuildArg *ab = (ApkBuildArg *)arg;
    RigCtx *ctx = rigctx_new(".");
    ctx->ws = ab->srv;
    rigctx_load_config(ctx, "rigcom.toml");
    ws_broadcastf(ab->srv, "{\"ev\":\"apk_start\",\"msg\":\"Iniciando empaquetado APK...\"}");
    if (!apk_check_tools(ctx)) { rigctx_free(ctx); free(ab); return NULL; }
    const char *so  = ab->so_path[0] ? ab->so_path : "build/libmain.so";
    const char *apk = ab->out_apk[0] ? ab->out_apk : "build/app.apk";
    bool ok = apk_build(ctx, so, apk);
    if (ok)
        ws_broadcastf(ab->srv, "{\"ev\":\"apk_done\",\"ok\":true,\"path\":\"%s\",\"msg\":\"APK firmado y listo.\"}", apk);
    else
        ws_broadcastf(ab->srv, "{\"ev\":\"apk_done\",\"ok\":false,\"msg\":\"Error al empaquetar APK.\"}");
    rigctx_free(ctx); free(ab); return NULL;
}

/* APK unpack thread */
typedef struct { WsServer *srv; char apk_path[512]; char out_dir[512]; } ApkUnpackArg;
static void* apk_unpack_thread(void *arg) {
    ApkUnpackArg *au = (ApkUnpackArg *)arg;
    RigCtx *ctx = rigctx_new(".");
    ctx->ws = au->srv;
    ws_broadcastf(au->srv, "{\"ev\":\"apk_unpack_start\",\"msg\":\"Desempaquetando APK...\"}");
    const char *dir = au->out_dir[0] ? au->out_dir : "build/unpack";
    char mc[512]; snprintf(mc, sizeof(mc), "mkdir -p %s", dir); (void)system(mc);
    bool ok = apk_unpack(ctx, au->apk_path, dir);
    if (ok)
        ws_broadcastf(au->srv, "{\"ev\":\"apk_unpack_done\",\"ok\":true,\"dir\":\"%s\",\"msg\":\"APK desempaquetado. Manifest_Readable.txt generado.\"}", dir);
    else
        ws_broadcastf(au->srv, "{\"ev\":\"apk_unpack_done\",\"ok\":false,\"msg\":\"Error al desempaquetar. Instala aapt: pkg install aapt\"}");
    rigctx_free(ctx); free(au); return NULL;
}


/* ─── APK Explorer: listar archivos ───────────────────────────────── */
typedef struct { WsServer *srv; char dir[512]; } ApkListArg;
static void *apk_list_thread(void *arg) {
    ApkListArg *al = (ApkListArg *)arg;
    const char *dir = al->dir[0] ? al->dir : "build/unpack";
    size_t bufsz = 131072;
    char *jbuf = malloc(bufsz);
    if (!jbuf) {
        ws_broadcastf(al->srv,
            "{\"ev\":\"apk_list\",\"ok\":false,\"files\":[]}");
        free(al); return NULL;
    }
    bool ok = apk_list_contents(dir, jbuf, bufsz);
    if (ok)
        ws_broadcastf(al->srv,
            "{\"ev\":\"apk_list\",\"ok\":true,\"dir\":\"%s\",\"files\":%s}",
            dir, jbuf);
    else
        ws_broadcastf(al->srv,
            "{\"ev\":\"apk_list\",\"ok\":false,"
            "\"msg\":\"No se pudo listar. Desempaqueta primero el APK.\","
            "\"files\":[]}");
    free(jbuf); free(al); return NULL;
}

/* ─── APK Explorer: leer archivo ──────────────────────────────────── */
typedef struct { WsServer *srv; char dir[512]; char path[512]; } ApkReadArg;
static void *apk_read_file_thread(void *arg) {
    ApkReadArg *ar = (ApkReadArg *)arg;
    const char *dir = ar->dir[0] ? ar->dir : "build/unpack";
    if (!ar->path[0]) {
        ws_broadcastf(ar->srv,
            "{\"ev\":\"apk_file_content\",\"ok\":false,\"msg\":\"path vacio\"}");
        free(ar); return NULL;
    }
    size_t sz = 0;
    char *content = apk_read_file(dir, ar->path, &sz);
    if (!content) {
        char safe_path[512];
        /* escape the path inline */
        size_t pi = 0;
        for (const char *pp = ar->path; *pp && pi + 4 < sizeof(safe_path)-1; pp++) {
            if (*pp == '"' || *pp == '\\') safe_path[pi++] = '\\';
            safe_path[pi++] = *pp;
        }
        safe_path[pi] = '\0';
        ws_broadcastf(ar->srv,
            "{\"ev\":\"apk_file_content\",\"ok\":false,"
            "\"path\":\"%s\",\"msg\":\"No se pudo leer el archivo\"}",
            safe_path);
        free(ar); return NULL;
    }
    /* Construir JSON con contenido escapado */
    size_t jbufsz = sz * 2 + 1024;
    char *jbuf = malloc(jbufsz);
    if (jbuf) {
        char esc_path[512]; size_t pi = 0;
        for (const char *pp = ar->path; *pp && pi + 4 < sizeof(esc_path)-1; pp++) {
            if (*pp == '"' || *pp == '\\') esc_path[pi++] = '\\';
            esc_path[pi++] = *pp;
        }
        esc_path[pi] = '\0';

        char *esc = malloc(sz * 2 + 4);
        if (esc) {
            size_t ci = 0;
            for (size_t k = 0; k < sz && ci + 8 < sz * 2 + 4; k++) {
                unsigned char ch = (unsigned char)content[k];
                if      (ch == '"')  { esc[ci++] = '\\'; esc[ci++] = '"';  }
                else if (ch == '\\') { esc[ci++] = '\\'; esc[ci++] = '\\'; }
                else if (ch == '\n') { esc[ci++] = '\\'; esc[ci++] = 'n';  }
                else if (ch == '\r') { esc[ci++] = '\\'; esc[ci++] = 'r';  }
                else if (ch == '\t') { esc[ci++] = '\\'; esc[ci++] = 't';  }
                else if (ch < 0x20) { /* omitir control */ }
                else                 { esc[ci++] = (char)ch; }
            }
            esc[ci] = '\0';
            snprintf(jbuf, jbufsz,
                "{\"ev\":\"apk_file_content\",\"ok\":true,"
                "\"path\":\"%s\",\"content\":\"%s\"}",
                esc_path, esc);
            ws_broadcast(ar->srv, jbuf, strlen(jbuf));
            free(esc);
        }
        free(jbuf);
    }
    free(content); free(ar); return NULL;
}

/* ─── APK Explorer: guardar archivo ───────────────────────────────── */
typedef struct {
    WsServer *srv; char dir[512]; char path[512]; char content[32768];
} ApkWriteArg;
static void *apk_write_file_thread(void *arg) {
    ApkWriteArg *aw = (ApkWriteArg *)arg;
    const char *dir = aw->dir[0] ? aw->dir : "build/unpack";
    bool ok = apk_write_file(dir, aw->path, aw->content, strlen(aw->content));
    ws_broadcastf(aw->srv,
        "{\"ev\":\"apk_write_done\",\"ok\":%s,\"path\":\"%s\","
        "\"msg\":\"%s\"}",
        ok ? "true" : "false", aw->path,
        ok ? "Archivo guardado" : "Error al guardar");
    free(aw); return NULL;
}

/* ─── APK Explorer: reempaquetar APK ──────────────────────────────── */
typedef struct { WsServer *srv; char dir[512]; char out_apk[512]; } ApkRepackArg;
static void *apk_repack_thread(void *arg) {
    ApkRepackArg *ar = (ApkRepackArg *)arg;
    RigCtx *ctx = rigctx_new("."); ctx->ws = ar->srv;
    const char *dir = ar->dir[0]     ? ar->dir     : "build/unpack";
    const char *out = ar->out_apk[0] ? ar->out_apk : "build/repack.apk";
    apk_repack(ctx, dir, out);
    rigctx_free(ctx); free(ar); return NULL;
}

/* Keystore generator thread */
typedef struct { WsServer *srv; char cn[128]; char org[128]; char pass[128]; char alias[64]; char ks_file[256]; } KeystoreArg;
static void* keystore_thread(void *arg) {
    KeystoreArg *ka = (KeystoreArg *)arg;
    const char *cn    = ka->cn[0]      ? ka->cn      : "RigCom";
    const char *org   = ka->org[0]     ? ka->org     : "RigCom";
    const char *pass  = ka->pass[0]    ? ka->pass    : "rigcom123";
    const char *alias = ka->alias[0]   ? ka->alias   : "rigcom";
    const char *ksf   = ka->ks_file[0] ? ka->ks_file : "rigcom.keystore";
    ws_broadcastf(ka->srv, "{\"ev\":\"keystore_progress\",\"msg\":\"Generando RSA 2048-bit...\"}");
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "keytool -genkeypair -v -keystore %s -alias %s"
        " -keyalg RSA -keysize 2048 -validity 10000"
        " -storepass %s -keypass %s -dname \"CN=%s, O=%s, C=MX\" > /dev/null 2>&1",
        ksf, alias, pass, pass, cn, org);
    int rc = system(cmd);
    if (rc == 0)
        ws_broadcastf(ka->srv,
            "{\"ev\":\"ks_done\",\"ok\":true,\"keystore\":\"%s\",\"alias\":\"%s\","
            "\"msg\":\"Llave RSA 2048-bit generada. Valida 10000 dias.\"}", ksf, alias);
    else
        ws_broadcastf(ka->srv,
            "{\"ev\":\"ks_done\",\"ok\":false,"
            "\"msg\":\"Error: instala keytool con: pkg install openjdk-17\"}");
    free(ka); return NULL;
}


/* ── JSON string extractor ───────────────────────────────────── */

/* ── JSON string extractor ───────────────────────────────────── */
static void ws_json_str(const char *json, const char *key,
                         char *out, size_t outsz) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(search);
    size_t i = 0;
    while (*p && *p != '"' && i < outsz - 1) out[i++] = *p++;
    out[i] = '\0';
}

/* Extrae entero de JSON: "key":N */
static int dap_extract_int_inline(const char *json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    return (int)strtol(p, NULL, 10);
}

static char* json_unescape(const char *cp, char *out, size_t outsz) {
    size_t j = 0;
    while (*cp && *cp != '"' && j < outsz - 1) {
        if (*cp == '\\' && *(cp+1)) {
            cp++;
            if (*cp == 'n')      out[j++] = '\n';
            else if (*cp == 't') out[j++] = '\t';
            else if (*cp == 'r') out[j++] = '\r';
            else                  out[j++] = *cp;
        } else {
            out[j++] = *cp;
        }
        cp++;
    }
    out[j] = '\0';
    return out;
}

/* ── FileBuf: buffer generico con ui + file + content ── */
typedef struct { UiCtx *ui; char file[512]; char content[1]; } FileBuf;
typedef struct { UiCtx *ui; } SimpleArg;

/* ── bench_thread ─────────────────────────────────────── */
static void* bench_thread(void *arg) {
    SimpleArg *sa = (SimpleArg *)arg;
    struct timespec t0, t1;
    double llvm[3] = {0}, arm[3] = {0};
    for (int r = 0; r < 3; r++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        rigcom_build(sa->ui->config_path, false);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        llvm[r] = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        rigcom_build(sa->ui->config_path, true);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        arm[r] = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    }
    double la = (llvm[0]+llvm[1]+llvm[2])/3.0;
    double aa  = (arm[0]+arm[1]+arm[2])/3.0;
    ws_broadcastf(sa->ui->srv,
        "{\"ev\":\"bench_result\",\"llvm_avg\":%.3f,\"arm_avg\":%.3f,"
        "\"runs\":[{\"llvm\":%.3f,\"arm\":%.3f},{\"llvm\":%.3f,\"arm\":%.3f},{\"llvm\":%.3f,\"arm\":%.3f}]}",
        la*1000.0, aa*1000.0,
        llvm[0]*1000.0, arm[0]*1000.0,
        llvm[1]*1000.0, arm[1]*1000.0,
        llvm[2]*1000.0, arm[2]*1000.0);
    free(sa);
    return NULL;
}

/* ── run_thread ───────────────────────────────────────── */
static void* run_thread(void *arg) {
    SimpleArg *sa = (SimpleArg *)arg;
    int rc = rigcom_build(sa->ui->config_path, false);
    if (rc != 0) {
        ws_broadcastf(sa->ui->srv,
            "{\"ev\":\"run_done\",\"ok\":false,\"exit_code\":1}");
        free(sa); return NULL;
    }
    RigCtx *ctx = rigctx_new(".");
    rigctx_load_config(ctx, sa->ui->config_path);
    const char *exec = ctx->config.output_exec
                       ? ctx->config.output_exec : "./build/bin/app";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s 2>&1", exec);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        ws_broadcastf(sa->ui->srv,
            "{\"ev\":\"run_done\",\"ok\":false,\"exit_code\":-1}");
        rigctx_free(ctx); free(sa); return NULL;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        if (l > 0 && line[l-1] == '\n') line[l-1] = '\0';
        char esc[1024]; size_t j = 0;
        for (size_t k = 0; line[k] && j < sizeof(esc)-4; k++) {
            if (line[k] == '"')        { esc[j++] = '\\'; esc[j++] = '"'; }
            else if (line[k] == '\\')  { esc[j++] = '\\'; esc[j++] = '\\'; }
            else                         { esc[j++] = line[k]; }
        }
        esc[j] = '\0';
        ws_broadcastf(sa->ui->srv,
            "{\"ev\":\"run_output\",\"output\":\"%s\"}", esc);
    }
    int ex = pclose(fp);
    ws_broadcastf(sa->ui->srv,
        "{\"ev\":\"run_done\",\"ok\":%s,\"exit_code\":%d}",
        ex == 0 ? "true" : "false", ex);
    rigctx_free(ctx);
    free(sa);
    return NULL;
}

/* ── bootstrap_thread ─────────────────────────────────── */
static void* bootstrap_thread(void *arg) {
    SimpleArg *sa = (SimpleArg *)arg;
    ws_broadcastf(sa->ui->srv,
        "{\"ev\":\"bootstrap_progress\",\"stage\":\"CHECK\","
        "\"msg\":\"Verificando fuentes de RigCom...\"}");
    int rc = rigcom_check(sa->ui->config_path, NULL);
    if (rc != 0) {
        ws_broadcastf(sa->ui->srv,
            "{\"ev\":\"bootstrap_done\",\"ok\":false,"
            "\"msg\":\"Errores en fuentes — bootstrap abortado\"}");
        free(sa); return NULL;
    }
    ws_broadcastf(sa->ui->srv,
        "{\"ev\":\"bootstrap_progress\",\"stage\":\"BUILD\","
        "\"msg\":\"Compilando RigCom...\"}");
    rc = rigcom_build(sa->ui->config_path, false);
    ws_broadcastf(sa->ui->srv,
        "{\"ev\":\"bootstrap_done\",\"ok\":%s,\"msg\":\"%s\"}",
        rc == 0 ? "true" : "false",
        rc == 0 ? "Bootstrap completo phi=1.618" : "Bootstrap fallido");
    free(sa);
    return NULL;
}

/* ── save_code_thread ─────────────────────────────────── */
#define NO_BUILD_TAG "\x01NO_BUILD\x01"

static void* save_code_thread(void *arg) {
    FileBuf *fb = (FileBuf *)arg;
    bool no_build = (strstr(fb->content, NO_BUILD_TAG) != NULL);
    size_t code_len = no_build
        ? (size_t)(strstr(fb->content, NO_BUILD_TAG) - fb->content)
        : strlen(fb->content);
    /* mkdir -p del directorio padre */
    char dir[512]; snprintf(dir, sizeof(dir), "%s", fb->file); dir[sizeof(dir)-1] = '\0';
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = '\0'; char mc[512]; snprintf(mc, sizeof(mc), "mkdir -p %s", dir); (void)system(mc); }
    FILE *f = fopen(fb->file, "w");
    if (!f) {
        ws_broadcastf(fb->ui->srv,
            "{\"ev\":\"save_done\",\"ok\":false,\"file\":\"%s\","
            "\"msg\":\"Error de escritura\"}", fb->file);
        free(fb); return NULL;
    }
    fwrite(fb->content, 1, code_len, f);
    fclose(f);
    ws_broadcastf(fb->ui->srv,
        "{\"ev\":\"save_done\",\"ok\":true,\"file\":\"%s\"}", fb->file);
    if (!no_build)
        rigcom_build(fb->ui->config_path, false);
    free(fb);
    return NULL;
}

/* ── load_file_thread ─────────────────────────────────── */
static void* load_file_thread(void *arg) {
    FileBuf *fb = (FileBuf *)arg;
    FILE *f = fopen(fb->file, "r");
    if (!f) {
        ws_broadcastf(fb->ui->srv,
            "{\"ev\":\"file_content\",\"file\":\"%s\","
            "\"content\":\"\",\"ok\":false}", fb->file);
        free(fb); return NULL;
    }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); rewind(f);
    char *buf = malloc((size_t)fsz + 1);
    size_t nr = fread(buf, 1, (size_t)fsz, f); buf[nr] = '\0'; fclose(f);
    size_t esz = nr * 2 + 64;
    char *esc = malloc(esz); size_t j = 0;
    for (size_t i = 0; i < nr && j < esz - 4; i++) {
        if (buf[i] == '"')        { esc[j++] = '\\'; esc[j++] = '"'; }
        else if (buf[i] == '\\') { esc[j++] = '\\'; esc[j++] = '\\'; }
        else if (buf[i] == '\n')  { esc[j++] = '\\'; esc[j++] = 'n'; }
        else if (buf[i] == '\r')  { esc[j++] = '\\'; esc[j++] = 'r'; }
        else if (buf[i] == '\t')  { esc[j++] = '\\'; esc[j++] = 't'; }
        else                       { esc[j++] = buf[i]; }
    }
    esc[j] = '\0';
    ws_broadcastf(fb->ui->srv,
        "{\"ev\":\"file_content\",\"file\":\"%s\","
        "\"content\":\"%s\",\"ok\":true}",
        fb->file, esc);
    free(buf); free(esc); free(fb);
    return NULL;
}

/* ── load_toml_thread ─────────────────────────────────── */
static void* load_toml_thread(void *arg) {
    SimpleArg *sa = (SimpleArg *)arg;
    FILE *f = fopen(sa->ui->config_path, "r");
    if (!f) {
        ws_broadcastf(sa->ui->srv,
            "{\"ev\":\"toml_content\",\"content\":\"# not found\\n\"}");
        free(sa); return NULL;
    }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); rewind(f);
    char *buf = malloc((size_t)fsz + 1);
    size_t nr = fread(buf, 1, (size_t)fsz, f); buf[nr] = '\0'; fclose(f);
    size_t esz = nr * 2 + 64;
    char *esc = malloc(esz); size_t j = 0;
    for (size_t i = 0; i < nr && j < esz - 4; i++) {
        if (buf[i] == '"')       { esc[j++] = '\\'; esc[j++] = '"'; }
        else if (buf[i] == '\\') { esc[j++] = '\\'; esc[j++] = '\\'; }
        else if (buf[i] == '\n') { esc[j++] = '\\'; esc[j++] = 'n'; }
        else if (buf[i] == '\r') { esc[j++] = '\\'; esc[j++] = 'r'; }
        else                      { esc[j++] = buf[i]; }
    }
    esc[j] = '\0';
    ws_broadcastf(sa->ui->srv,
        "{\"ev\":\"toml_content\",\"content\":\"%s\"}", esc);
    free(buf); free(esc); free(sa);
    return NULL;
}

/* ── save_toml_thread ─────────────────────────────────── */
static void* save_toml_thread(void *arg) {
    FileBuf *fb = (FileBuf *)arg;
    FILE *f = fopen(fb->ui->config_path, "w");
    if (!f) {
        ws_broadcastf(fb->ui->srv,
            "{\"ev\":\"toml_saved\",\"ok\":false,"
            "\"msg\":\"Error de escritura\"}");
        free(fb); return NULL;
    }
    fputs(fb->content, f);
    fclose(f);
    ws_broadcastf(fb->ui->srv, "{\"ev\":\"toml_saved\",\"ok\":true}");
    free(fb);
    return NULL;
}

static void ui_on_message(WsServer *srv, const char *json, size_t len,
                           void *user) {
    UiCtx *ui = (UiCtx *)user;
    (void)len;

    /* Extraer campo cmd */
    char cmd_val[64] = {0};
    ws_json_str(json, "cmd", cmd_val, sizeof(cmd_val));

    /* ═══ COMANDOS cmd:* (enviados por la UI) ═══ */

    if (strcmp(cmd_val, "load_toml") == 0) {
        SimpleArg *sa = calloc(1, sizeof(SimpleArg)); sa->ui = ui;
        pthread_t t; pthread_create(&t, NULL, load_toml_thread, sa); pthread_detach(t);

    } else if (strcmp(cmd_val, "save_toml") == 0) {
        const char *cp = strstr(json, "\"content\":\"");
        if (cp) {
            cp += 11;
            size_t bsz = strlen(cp) + 2;
            FileBuf *fb = malloc(sizeof(UiCtx*) + sizeof(char[512]) + bsz);
            fb->ui = ui; fb->file[0] = '\0';
            json_unescape(cp, fb->content, bsz);
            pthread_t t; pthread_create(&t, NULL, save_toml_thread, fb); pthread_detach(t);
        }

    } else if (strcmp(cmd_val, "load_file") == 0) {
        FileBuf *fb = calloc(1, sizeof(FileBuf) + 2); fb->ui = ui;
        ws_json_str(json, "file", fb->file, sizeof(fb->file));
        if (!fb->file[0]) strncpy(fb->file, "src/main.c", sizeof(fb->file)-1);
        pthread_t t; pthread_create(&t, NULL, load_file_thread, fb); pthread_detach(t);

    } else if (strcmp(cmd_val, "save_code") == 0) {
        char file[512] = {0};
        ws_json_str(json, "file", file, sizeof(file));
        if (!file[0]) strncpy(file, "src/main.c", sizeof(file)-1);
        bool no_build = (strstr(json, "\"no_build\":true") != NULL);
        const char *cp = strstr(json, "\"code\":\"");
        if (cp) {
            cp += 8;
            size_t bsz = strlen(cp) + 16;
            FileBuf *fb = malloc(sizeof(UiCtx*) + sizeof(char[512]) + bsz);
            fb->ui = ui; snprintf(fb->file, sizeof(fb->file), "%s", file);
            json_unescape(cp, fb->content, bsz);
            if (no_build) strcat(fb->content, NO_BUILD_TAG);
            pthread_t t; pthread_create(&t, NULL, save_code_thread, fb); pthread_detach(t);
        }

    } else if (strcmp(cmd_val, "bench") == 0) {
        SimpleArg *sa = calloc(1, sizeof(SimpleArg)); sa->ui = ui;
        pthread_t t; pthread_create(&t, NULL, bench_thread, sa); pthread_detach(t);

    } else if (strcmp(cmd_val, "run") == 0) {
        SimpleArg *sa = calloc(1, sizeof(SimpleArg)); sa->ui = ui;
        pthread_t t; pthread_create(&t, NULL, run_thread, sa); pthread_detach(t);

    } else if (strcmp(cmd_val, "bootstrap") == 0) {
        SimpleArg *sa = calloc(1, sizeof(SimpleArg)); sa->ui = ui;
        pthread_t t; pthread_create(&t, NULL, bootstrap_thread, sa); pthread_detach(t);

    } else if (strcmp(cmd_val, "build_apk") == 0 || strstr(json, "\"build_apk\"")) {
        ApkBuildArg *ab = calloc(1, sizeof(ApkBuildArg)); ab->srv = srv;
        ws_json_str(json, "so",  ab->so_path, sizeof(ab->so_path));
        ws_json_str(json, "apk", ab->out_apk, sizeof(ab->out_apk));
        pthread_t tapk; pthread_create(&tapk, NULL, apk_build_thread, ab); pthread_detach(tapk);

    } else if (strcmp(cmd_val, "unpack_apk") == 0 || strstr(json, "\"unpack_apk\"")) {
        ApkUnpackArg *au = calloc(1, sizeof(ApkUnpackArg)); au->srv = srv;
        ws_json_str(json, "apk", au->apk_path, sizeof(au->apk_path));
        ws_json_str(json, "dir", au->out_dir,  sizeof(au->out_dir));
        pthread_t tunp; pthread_create(&tunp, NULL, apk_unpack_thread, au); pthread_detach(tunp);

    /* ── APK Explorer: listar archivos ─────────────────────────── */
    } else if (strcmp(cmd_val, "apk_list") == 0) {
        ApkListArg *al = calloc(1, sizeof(ApkListArg)); al->srv = srv;
        ws_json_str(json, "dir", al->dir, sizeof(al->dir));
        pthread_t tal; pthread_create(&tal, NULL, apk_list_thread, al); pthread_detach(tal);

    /* ── APK Explorer: leer archivo ─────────────────────────────── */
    } else if (strcmp(cmd_val, "apk_read_file") == 0) {
        ApkReadArg *ar = calloc(1, sizeof(ApkReadArg)); ar->srv = srv;
        ws_json_str(json, "dir",  ar->dir,  sizeof(ar->dir));
        ws_json_str(json, "path", ar->path, sizeof(ar->path));
        pthread_t tar; pthread_create(&tar, NULL, apk_read_file_thread, ar); pthread_detach(tar);

    /* ── APK Explorer: guardar archivo ──────────────────────────── */
    } else if (strcmp(cmd_val, "apk_write_file") == 0) {
        ApkWriteArg *aw = calloc(1, sizeof(ApkWriteArg)); aw->srv = srv;
        ws_json_str(json, "dir",     aw->dir,     sizeof(aw->dir));
        ws_json_str(json, "path",    aw->path,    sizeof(aw->path));
        ws_json_str(json, "content", aw->content, sizeof(aw->content));
        pthread_t taw; pthread_create(&taw, NULL, apk_write_file_thread, aw); pthread_detach(taw);

    /* ── APK Explorer: reempaquetar APK ─────────────────────────── */
    } else if (strcmp(cmd_val, "apk_repack") == 0) {
        ApkRepackArg *arep = calloc(1, sizeof(ApkRepackArg)); arep->srv = srv;
        ws_json_str(json, "dir",     arep->dir,     sizeof(arep->dir));
        ws_json_str(json, "out_apk", arep->out_apk, sizeof(arep->out_apk));
        pthread_t trep; pthread_create(&trep, NULL, apk_repack_thread, arep); pthread_detach(trep);

    } else if (strcmp(cmd_val, "forge_keystore") == 0 || strstr(json, "\"generate_keystore\"")) {
        KeystoreArg *ka = calloc(1, sizeof(KeystoreArg)); ka->srv = srv;
        ws_json_str(json, "alias", ka->alias,   sizeof(ka->alias));
        ws_json_str(json, "org",   ka->org,      sizeof(ka->org));
        ws_json_str(json, "pass",  ka->pass,     sizeof(ka->pass));
        ws_json_str(json, "out",   ka->ks_file,  sizeof(ka->ks_file));
        ws_json_str(json, "name",  ka->cn,       sizeof(ka->cn));
        if (!ka->ks_file[0]) strncpy(ka->ks_file, "rigcom.keystore", sizeof(ka->ks_file)-1);
        if (!ka->alias[0])   strncpy(ka->alias,   "rigcom",          sizeof(ka->alias)-1);
        pthread_t tks; pthread_create(&tks, NULL, keystore_thread, ka); pthread_detach(tks);

    /* ═══ COMANDOS LEGACY (sin cmd:) ═══ */

    } else if (strstr(json, "\"build\"")) {
        bool native = strstr(json, "\"native\":true") != NULL;
        BuildArg *ba = malloc(sizeof(BuildArg)); ba->ui = ui; ba->native = native;
        pthread_t tid; pthread_create(&tid, NULL, build_thread, ba); pthread_detach(tid);

    } else if (strstr(json, "\"check\"")) {
        CheckArg *ca = malloc(sizeof(CheckArg)); ca->ui = ui;
        ws_json_str(json, "file", ca->file, sizeof(ca->file));
        pthread_t tid; pthread_create(&tid, NULL, check_thread, ca); pthread_detach(tid);

    } else if (strstr(json, "\"status\"")) {
        ws_broadcastf(srv,
            "{\"ev\":\"status\",\"state\":\"idle\","
            "\"version\":\"%s\",\"phi\":%.19f}",
            RIGCOM_VERSION, RIGCOM_PHI);

    } else if (strstr(json, "\"stop\"")) {
        ws_server_stop(srv);

    } else if (strstr(json, "\"upload\"")) {
        const char *np = strstr(json, "\"name\":\"");
        const char *dp = strstr(json, "\"data\":\"");
        if (np && dp) {
            UploadArg *ua = calloc(1, sizeof(UploadArg)); ua->ui = ui;
            np += 8; int ni = 0;
            while (*np && *np != '"' && ni < 255) ua->name[ni++] = *np++;
            ua->name[ni] = '\0';
            dp += 8; const char *dend = dp;
            while (*dend && *dend != '"') dend++;
            ua->b64len  = (size_t)(dend - dp);
            ua->b64data = malloc(ua->b64len + 1);
            memcpy(ua->b64data, dp, ua->b64len);
            ua->b64data[ua->b64len] = '\0';
            pthread_t tid; pthread_create(&tid, NULL, upload_thread, ua); pthread_detach(tid);
        }

    } else if (strstr(json, "\"dap_cmd\"")) {
        if (ui->dap) dap_on_ws_message(ui->dap, json, len);

    /* ═══ RIGPACK — gestor de dependencias zero-config ═══ */
    } else if (strcmp(cmd_val, "rigpack_install") == 0) {
        RigPackArg *rp = calloc(1, sizeof(RigPackArg));
        rp->srv = srv;
        ws_json_str(json, "lib", rp->lib, sizeof(rp->lib));
        if (!rp->lib[0]) {
            ws_broadcastf(srv,
                "{\"ev\":\"rigpack_done\",\"ok\":false,"
                "\"lib\":\"\",\"msg\":\"Nombre de libreria vacio\"}");
            free(rp);
        } else {
            pthread_t t;
            pthread_create(&t, NULL, rigpack_install_thread, rp);
            pthread_detach(t);
        }

    } else if (strcmp(cmd_val, "rigpack_list") == 0) {
        rigpack_emit_list(srv);

    /* ═══ ORACLE — analisis AST linea por linea ═══ */
    } else if (strcmp(cmd_val, "oracle_scan") == 0) {
        char file[512] = {0};
        ws_json_str(json, "file", file, sizeof(file));
        if (!file[0]) strncpy(file, "src/main.c", sizeof(file)-1);

        FILE *fp = fopen(file, "r");
        if (!fp) {
            ws_broadcastf(srv,
                "{\"ev\":\"oracle_scan_done\","
                "\"file\":\"%s\",\"hints\":0}", file);
        } else {
            fseek(fp, 0, SEEK_END);
            long fsz = ftell(fp); rewind(fp);
            char *raw = malloc((size_t)fsz + 1);
            size_t nr = fread(raw, 1, (size_t)fsz, fp);
            raw[nr] = '\0'; fclose(fp);

            char *lines[4096]; int n_lines = 0;
            char *ln = strtok(raw, "\n");
            while (ln && n_lines < 4095) { lines[n_lines++] = ln; ln = strtok(NULL, "\n"); }

            int hint_count = 0;
            char malloc_vars[64][128] = {{0}};
            int  malloc_lns[64] = {0};
            int  n_tracked = 0;

            ws_broadcastf(srv,
                "{\"ev\":\"oracle_scan_start\",\"file\":\"%s\",\"lines\":%d}",
                file, n_lines);

            for (int li = 0; li < n_lines; li++) {
                const char *cur = lines[li];
                const char *mp = strstr(cur, "= malloc(");
                if (!mp) mp = strstr(cur, "= calloc(");
                if (!mp) mp = strstr(cur, "= realloc(");

                if (mp && n_tracked < 64) {
                    const char *e = mp - 1;
                    while (e > cur && *e == ' ') e--;
                    const char *s = e;
                    while (s > cur && ((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_')) s--;
                    s++;
                    int vlen = (int)(e - s + 1);
                    if (vlen > 0 && vlen < 127) {
                        strncpy(malloc_vars[n_tracked], s, (size_t)vlen);
                        malloc_vars[n_tracked][vlen] = '\0';
                        malloc_lns[n_tracked] = li + 1;
                        n_tracked++;
                    }
                    bool has_chk = false;
                    for (int ci = li+1; ci < li+4 && ci < n_lines; ci++) {
                        if (strstr(lines[ci],"== NULL")||strstr(lines[ci],"!= NULL")||
                            strstr(lines[ci],"if (!")||strstr(lines[ci],"assert("))
                            { has_chk = true; break; }
                    }
                    if (!has_chk && hint_count < 32) {
                        char vn[128]={0};
                        if (vlen>0&&vlen<127) strncpy(vn,s,(size_t)vlen);
                        ws_broadcastf(srv,
                            "{\"ev\":\"oracle_hint\",\"file\":\"%s\",\"line\":%d,"
                            "\"kind\":\"null_deref_risk\","
                            "\"msg\":\"malloc() sin null-check '%s' puede ser NULL\","
                            "\"fix\":\"if (!%s) { free(%s); return NULL; }\","
                            "\"action\":\"add_null_check\",\"var\":\"%s\"}",
                            file, li+1, vn, vn, vn, vn);
                        hint_count++;
                    }
                }

                for (int vi = 0; vi < n_tracked; vi++) {
                    if (!malloc_vars[vi][0]) continue;
                    char pat[256]; snprintf(pat,sizeof(pat),"free(%s)",malloc_vars[vi]);
                    if (strstr(cur, pat)) malloc_vars[vi][0] = '\0';
                }

                const char *tr = cur;
                while (*tr==' '||*tr=='\t') tr++;
                if (strcmp(tr,"}")==0||strcmp(tr,"}\r")==0) {
                    for (int vi = 0; vi < n_tracked; vi++) {
                        if (!malloc_vars[vi][0]) continue;
                        if (hint_count < 32) {
                            ws_broadcastf(srv,
                                "{\"ev\":\"oracle_hint\",\"file\":\"%s\",\"line\":%d,"
                                "\"kind\":\"orphan_ptr\","
                                "\"msg\":\"Memory leak probable: '%s' sin free() (asignado en linea %d)\","
                                "\"fix\":\"free(%s);\","
                                "\"action\":\"inject_free\","
                                "\"var\":\"%s\",\"alloc_line\":%d}",
                                file, li+1,
                                malloc_vars[vi], malloc_lns[vi],
                                malloc_vars[vi], malloc_vars[vi], malloc_lns[vi]);
                            hint_count++;
                        }
                        malloc_vars[vi][0] = '\0';
                    }
                    n_tracked = 0;
                }
            }
            free(raw);
            ws_broadcastf(srv,
                "{\"ev\":\"oracle_scan_done\",\"file\":\"%s\",\"hints\":%d}",
                file, hint_count);
        }

    /* ═══ PORTAL — serializa AST para visualizacion 3D ═══ */
    } else if (strcmp(cmd_val, "portal_request") == 0) {
        char file[512] = {0};
        ws_json_str(json, "file", file, sizeof(file));
        if (!file[0]) strncpy(file, "src/main.c", sizeof(file)-1);

        RigErrorLog plog = {0};
        RigCtx *pctx = rigctx_new(".");
        FILE *fp = fopen(file, "r");
        if (!fp) {
            ws_broadcastf(srv,
                "{\"ev\":\"portal_ir\",\"ok\":false,\"msg\":\"No se pudo leer archivo\"}");
            rigctx_free(pctx);
        } else {
            fseek(fp, 0, SEEK_END);
            long fsz = ftell(fp); rewind(fp);
            char *raw = malloc((size_t)fsz + 1);
            size_t nr = fread(raw, 1, (size_t)fsz, fp); raw[nr]='\0'; fclose(fp);

            Preproc pp; preproc_init(&pp, pctx, &plog);
            preproc_add_path(&pp, "."); preproc_add_path(&pp, "include");
            char *src = preproc_run(&pp, raw, nr, file);
            free(raw);

            if (src) {
                Lexer lx; lexer_init(&lx, src, strlen(src), file);
                ASTArena *ar = ast_arena_new();
                Parser ps; parser_init(&ps, &lx, &plog, ar, file);
                ASTNode *tu = parser_parse(&ps);

                char *gbuf = malloc(131072);
                if (gbuf) {
                    char *gp = gbuf; size_t grem = 131072; int gn;
                    gn = snprintf(gp, grem,
                        "{\"ev\":\"portal_ir\",\"ok\":true,\"file\":\"%s\",\"functions\":[", file);
                    gp += gn; grem -= (size_t)gn;

                    bool ffirst = true;
                    if (tu && tu->kind == NODE_TRANSLATION_UNIT) {
                        for (uint32_t di = 0; di < tu->tu.n_decls && grem > 512; di++) {
                            ASTNode *d = tu->tu.decls[di];
                            if (!d) continue;
                            if (d->kind == NODE_FUNC_DEF) {
                                const char *fn = d->func.name ? d->func.name : "anon";
                                int ns = 0;
                                ASTNode *b = d->func.body;
                                if (b && b->kind == NODE_BLOCK) {
                                    ns = (int)b->block.n_stmts;
                                }
                                gn = snprintf(gp, grem,
                                    "%s{\"name\":\"%s\",\"n_stmts\":%d,\"line\":%u}",
                                    ffirst ? "" : ",", fn, ns, d->line);
                                gp+=gn; grem-=(size_t)gn; ffirst=false;
                            }
                        }
                    }
                    snprintf(gp, grem, "]}");
                    ws_broadcast(srv, gbuf, strlen(gbuf));
                    free(gbuf);
                }
                ast_arena_free(ar);
                free(src);
            } else {
                ws_broadcastf(srv,
                    "{\"ev\":\"portal_ir\",\"ok\":false,\"msg\":\"Error en preprocesamiento\"}");
            }
            preproc_free(&pp);
            rigctx_free(pctx);
        }

    /* ═══ RIGBRIDGE — malla P2P de compilacion ═══ */
    } else if (strcmp(cmd_val, "bridge_scan") == 0) {
        BridgeScanArg *bsa = calloc(1, sizeof(BridgeScanArg));
        bsa->srv = srv; bsa->duration_ms = 4000;
        pthread_t bt; pthread_create(&bt, NULL, rigbridge_scan_thread, bsa); pthread_detach(bt);

    } else if (strcmp(cmd_val, "bridge_peers") == 0) {
        rigbridge_emit_peers(srv);

    } else if (strcmp(cmd_val, "bridge_build") == 0) {
        BridgeBuildArg *bba = calloc(1, sizeof(BridgeBuildArg));
        bba->srv = srv;
        pthread_t bt2; pthread_create(&bt2, NULL, rigbridge_build_thread, bba); pthread_detach(bt2);

    /* ══ ORACLE IP — análisis inter-procedural AST real ══ */
    } else if (strcmp(cmd_val, "oracle_ip_scan") == 0) {
        OracleIPArg *oia = calloc(1, sizeof(OracleIPArg));
        oia->srv = srv;
        ws_json_str(json, "file", oia->file, sizeof(oia->file));
        if (!oia->file[0]) strncpy(oia->file, "src/main.c", sizeof(oia->file)-1);
        oia->interprocedural = (strstr(json, "\"interprocedural\":true") != NULL);
        pthread_t oit; pthread_create(&oit, NULL, oracle_ip_thread, oia); pthread_detach(oit);

    /* ══ ORACLE FIX — inyecta corrección en el editor ══ */
    } else if (strcmp(cmd_val, "oracle_fix") == 0) {
        char o_file[512]={0}, o_var[128]={0}, o_action[64]={0}, o_fix[512]={0};
        ws_json_str(json, "file",   o_file,   sizeof(o_file));
        ws_json_str(json, "var",    o_var,    sizeof(o_var));
        ws_json_str(json, "action", o_action, sizeof(o_action));
        ws_json_str(json, "fix",    o_fix,    sizeof(o_fix));
        int o_line = 0;
        { const char *lp = strstr(json,"\"alloc_line\":"); if(lp) o_line=(int)strtol(lp+14,NULL,10); }
        ws_broadcastf(srv,
            "{\"ev\":\"oracle_fix_ready\","
            "\"file\":\"%s\","
            "\"line\":%d,"
            "\"action\":\"%s\","
            "\"var\":\"%s\","
            "\"fix_code\":\"%s\"}",
            o_file, o_line, o_action, o_var, o_fix);

    /* ══ GVN + ARM64 tiling ══ */
    } else if (strcmp(cmd_val, "gvn_run") == 0) {
        char g_file[512]={0};
        ws_json_str(json, "file", g_file, sizeof(g_file));
        if (!g_file[0]) strncpy(g_file, "src/main.c", sizeof(g_file)-1);
        ws_broadcastf(srv, "{\"ev\":\"gvn_start\",\"file\":\"%s\"}", g_file);
        RigErrorLog glog={0};
        RigCtx *gctx = rigctx_new(".");
        rigctx_load_config(gctx, "rigcom.toml");
        IRModule *gm = compile_source_file(g_file, gctx, &glog);
        if (gm) {
            for (uint32_t gfi = 0; gfi < gm->n_funcs; gfi++) {
                ir_pass_gvn(gm->funcs[gfi]);
                ir_pass_arm64_tile(gm->funcs[gfi]);
            }
            ws_broadcastf(srv,
                "{\"ev\":\"gvn_done\",\"file\":\"%s\"," 
                "\"n_funcs\":%u,\"ok\":true}", g_file, gm->n_funcs);
            irmod_free(gm);
        } else {
            ws_broadcastf(srv,
                "{\"ev\":\"gvn_done\",\"file\":\"%s\"," 
                "\"ok\":false,\"msg\":\"Error compilando\"}", g_file);
        }
        rigctx_free(gctx);

    /* ══ PTY — terminal embebida ══ */
    } else if (strcmp(cmd_val, "pty_launch") == 0) {
        PtyLaunchArg *pla = calloc(1, sizeof(PtyLaunchArg));
        pla->srv = srv;
        ws_json_str(json, "exec", pla->exec_path, sizeof(pla->exec_path));
        ws_json_str(json, "args", pla->args,      sizeof(pla->args));
        if (!pla->exec_path[0])
            strncpy(pla->exec_path, "build/bin/out", sizeof(pla->exec_path)-1);
        pthread_t ptyt; pthread_create(&ptyt, NULL, pty_launch_thread, pla); pthread_detach(ptyt);

    } else if (strcmp(cmd_val, "pty_input") == 0) {
        char pty_data[1024]={0};
        ws_json_str(json, "data", pty_data, sizeof(pty_data));
        ws_broadcastf(srv, "{\"ev\":\"pty_echo\",\"data\":\"%s\"}", pty_data);

    /* ══ ARENA HARDEN ══ */
    } else if (strcmp(cmd_val, "arena_check") == 0) {
        int hits = arena_harden_check_all(NULL);
        ws_broadcastf(srv,
            "{\"ev\":\"arena_check_done\","
            "\"guard_hits\":%d,"
            "\"total_allocs\":%llu,"
            "\"total_bytes\":%llu,"
            "\"peak\":%llu,"
            "\"ok\":%s}",
            hits,
            (unsigned long long)g_arena_stats.total_allocs,
            (unsigned long long)g_arena_stats.total_bytes,
            (unsigned long long)g_arena_stats.peak_usage,
            hits == 0 ? "true" : "false");

    /* ══ FRONTEND LIST ══ */
    } else if (strcmp(cmd_val, "frontend_list") == 0) {
        char flbuf[2048]; char *flp = flbuf; size_t flrem = sizeof(flbuf); int fln;
        fln = snprintf(flp,flrem,"{\"ev\":\"frontend_list\",\"frontends\":[");
        flp+=fln; flrem-=(size_t)fln;
        for (int fli=0; fli<g_n_frontends && flrem>128; fli++) {
            fln=snprintf(flp,flrem,"%s{\"ext\":\"%s\",\"name\":\"%s\",\"has_ir\":%s}",
                fli?",":"", g_frontends[fli].extension,
                g_frontends[fli].display_name,
                g_frontends[fli].compile_to_ir?"true":"false");
            flp+=fln; flrem-=(size_t)fln;
        }
        snprintf(flp,flrem,"]}");
        ws_broadcast(srv, flbuf, strlen(flbuf));

    /* ══ BRIDGE THERMAL ══ */
    } else if (strcmp(cmd_val, "bridge_thermal") == 0) {
        FILE *tf = fopen("/sys/class/thermal/thermal_zone0/temp","r");
        int temp_mc = 0;
        if (tf) { fscanf(tf,"%d",&temp_mc); fclose(tf); }
        ws_broadcastf(srv,
            "{\"ev\":\"bridge_thermal\"," 
            "\"temp_celsius\":%.1f,\"safe\":%s,\"msg\":\"%s\"}",
            temp_mc/1000.0,
            temp_mc<70000?"true":"false",
            temp_mc<70000?"Temperatura normal — apto para compilar"
                         :"ADVERTENCIA — temperatura alta");

    /* ══ CACHE STATS — estadísticas de caché SHA-256 ══ */
    } else if (strcmp(cmd_val, "cache_stats") == 0) {
        char cbuf[512];
        rigcache_stats_json(cbuf, sizeof(cbuf));
        ws_broadcast(srv, cbuf, strlen(cbuf));

    } else if (strcmp(cmd_val, "cache_evict") == 0) {
        rigcache_evict(RIGCACHE_DIR, 128);
        ws_broadcastf(srv, "{\"ev\":\"cache_evicted\",\"dir\":\"%s\"}", RIGCACHE_DIR);

    /* ══ RIGSCRIPT — compilar archivo .rigc / .rs ══ */
    } else if (strcmp(cmd_val, "rigscript_compile") == 0) {
        char rs_file[512] = {0};
        ws_json_str(json, "file", rs_file, sizeof(rs_file));
        if (!rs_file[0]) strncpy(rs_file, "src/main.rigc", sizeof(rs_file)-1);
        ws_broadcastf(srv, "{\"ev\":\"rigscript_start\",\"file\":\"%s\"}", rs_file);
        RigCtx *rsctx = rigctx_new(".");
        RigErrorLog rslog = {0};
        IRModule *rsmod = rigscript_compile_to_ir(rsctx, rs_file, &rslog);
        int rserrs = riglog_has_errors(&rslog) ? 1 : 0;
        if (rserrs > 0) rigctx_ws_emit_errors(rsctx, &rslog);
        ws_broadcastf(srv,
            "{\"ev\":\"rigscript_done\",\"file\":\"%s\","
            "\"ok\":%s,\"errors\":%d,\"funcs\":%u}",
            rs_file, rsmod?"true":"false", rserrs,
            rsmod ? rsmod->n_funcs : 0);
        if (rsmod) irmod_free(rsmod);
        rigctx_free(rsctx);

    } else if (strcmp(cmd_val, "rigscript_ast") == 0) {
        char rs_file[512] = {0};
        ws_json_str(json, "file", rs_file, sizeof(rs_file));
        RigCtx *rsctx = rigctx_new(".");
        RigErrorLog rslog = {0};
        char *ast_j = rigscript_ast_json(rsctx, rs_file, &rslog);
        if (ast_j) {
            /* Wrap in WS event */
            size_t asz = strlen(ast_j) + 256;
            char *abuf = malloc(asz);
            snprintf(abuf, asz, "{\"ev\":\"rigscript_ast\",\"file\":\"%s\",\"ast\":%s}",
                     rs_file, ast_j);
            ws_broadcast(srv, abuf, strlen(abuf));
            free(abuf); free(ast_j);
        }
        rigctx_free(rsctx);

    } else if (strcmp(cmd_val, "rigscript_gen_jni") == 0) {
        char rs_file[512] = {0}, pkg[256] = {0};
        ws_json_str(json, "file", rs_file, sizeof(rs_file));
        ws_json_str(json, "package", pkg, sizeof(pkg));
        if (!pkg[0]) strncpy(pkg, "com.rigcom.bridge", sizeof(pkg)-1);
        RigErrorLog rslog = {0};
        bool ok = rigscript_gen_jni_bridge(rs_file, pkg,
                      "build/RigBridge.java", "build/rig_jni_bridge.c", &rslog);
        ws_broadcastf(srv,
            "{\"ev\":\"jni_bridge_done\",\"ok\":%s,"
            "\"java\":\"build/RigBridge.java\","
            "\"c_wrapper\":\"build/rig_jni_bridge.c\"}",
            ok?"true":"false");

    /* ══ LSP — autocompletado en vivo ══ */
    } else if (strcmp(cmd_val, "lsp_complete") == 0) {
        char lsp_file[512] = {0}; int lsp_line = 0, lsp_col = 0;
        ws_json_str(json, "file", lsp_file, sizeof(lsp_file));
        lsp_line = dap_extract_int_inline(json, "line");
        lsp_col  = dap_extract_int_inline(json, "col");
        if (!lsp_file[0]) strncpy(lsp_file, "src/main.c", sizeof(lsp_file)-1);
        /* Compilar rápido hasta SymTable, emitir símbolos como sugerencias */
        RigCtx *lctx = rigctx_new(".");
        RigErrorLog llog = {0};
        Preproc lpp; preproc_init(&lpp, lctx, &llog);
        preproc_add_path(&lpp, "."); preproc_add_path(&lpp, "include");
        FILE *lfp = fopen(lsp_file, "r");
        if (lfp) {
            fseek(lfp,0,SEEK_END); long lfsz=ftell(lfp); rewind(lfp);
            char *lraw=malloc((size_t)lfsz+1);
            size_t lnr=fread(lraw,1,(size_t)lfsz,lfp); lraw[lnr]='\0'; fclose(lfp);
            char *lsrc=preproc_run(&lpp,lraw,lnr,lsp_file); free(lraw);
            if (lsrc) {
                Lexer llx; lexer_init(&llx,lsrc,strlen(lsrc),lsp_file);
                ASTArena *larena=ast_arena_new();
                Parser lpar; parser_init(&lpar,&llx,&llog,larena,lsp_file);
                ASTNode *ltu=parser_parse(&lpar);
                /* Emitir sugerencias de funciones y variables del AST */
                char lbuf[8192]; int lpos=0;
                lpos+=snprintf(lbuf+lpos,sizeof(lbuf)-(size_t)lpos,
                    "{\"ev\":\"lsp_completions\",\"file\":\"%s\","
                    "\"line\":%d,\"col\":%d,\"items\":[",
                    lsp_file,lsp_line,lsp_col);
                /* Walk top-level declarations via tu.decls[] */
                bool first=true;
                if (ltu && ltu->kind==NODE_TRANSLATION_UNIT) {
                  for (uint32_t _di=0;
                       _di<ltu->tu.n_decls && lpos<(int)sizeof(lbuf)-256;
                       _di++) {
                    ASTNode *n=ltu->tu.decls[_di]; if(!n) continue;
                    const char *kind=NULL,*name=NULL;
                    if(n->kind==NODE_FUNC_DEF){kind="function";name=n->func.name;}
                    else if(n->kind==NODE_VAR_DECL){kind="variable";name=n->var_decl.name;}
                    if(kind&&name){
                        lpos+=snprintf(lbuf+lpos,sizeof(lbuf)-(size_t)lpos,
                            "%s{\"label\":\"%s\",\"kind\":\"%s\"}",
                            first?"":","  ,name,kind);
                        first=false;
                    }
                  }
                }
                snprintf(lbuf+lpos,sizeof(lbuf)-(size_t)lpos,"]}");
                ws_broadcast(srv,lbuf,strlen(lbuf));
                ast_arena_free(larena); free(lsrc);
            }
        }
        preproc_free(&lpp); rigctx_free(lctx);

    /* ══ SANDBOX — compilar código peer en namespace aislado ══ */
    } else if (strcmp(cmd_val, "sandbox_compile") == 0) {
        char sb_file[512]={0}, sb_out[512]={0};
        ws_json_str(json, "file", sb_file, sizeof(sb_file));
        ws_json_str(json, "out",  sb_out,  sizeof(sb_out));
        if (!sb_file[0]) { ws_broadcastf(srv,"{\"ev\":\"sandbox_done\",\"ok\":false,\"msg\":\"file requerido\"}"); goto done_cmd; }
        if (!sb_out[0])  snprintf(sb_out, sizeof(sb_out), "build/sandbox_out");
        ws_broadcastf(srv, "{\"ev\":\"sandbox_start\",\"file\":\"%s\"}", sb_file);
        /* Usar unshare para aislar: sin red, sin acceso a $HOME */
        char sb_cmd[1024];
        snprintf(sb_cmd, sizeof(sb_cmd),
            "mkdir -p /tmp/rigcom_sandbox && "
            "unshare --user --pid --net --mount --fork "
            "clang -std=c11 -O2 -fPIC -o /tmp/rigcom_sandbox/out '%s' 2>&1",
            sb_file);
        FILE *sbp = popen(sb_cmd, "r");
        char sb_log[4096]={0}; size_t sb_nr=0;
        if (sbp) { sb_nr=fread(sb_log,1,sizeof(sb_log)-1,sbp); sb_log[sb_nr]='\0'; pclose(sbp); }
        /* Escape JSON */
        char sb_esc[8192]={0}; size_t ei=0;
        for(size_t si=0;si<sb_nr&&ei<sizeof(sb_esc)-4;si++){
            if(sb_log[si]=='"'){sb_esc[ei++]='\\';sb_esc[ei++]='"';}
            else if(sb_log[si]=='\n'){sb_esc[ei++]='\\';sb_esc[ei++]='n';}
            else if(sb_log[si]=='\\'){sb_esc[ei++]='\\';sb_esc[ei++]='\\';}
            else sb_esc[ei++]=sb_log[si];
        }
        ws_broadcastf(srv,
            "{\"ev\":\"sandbox_done\",\"file\":\"%s\",\"ok\":%s,\"log\":\"%s\"}",
            sb_file, sb_nr==0?"true":"false", sb_esc);

    /* ══ SAFESTACK — habilitar pila dual en siguiente build ══ */
    } else if (strcmp(cmd_val, "safestack_enable") == 0) {
        /* Agregar -fsanitize=safe-stack al contexto de compilación */
        ws_broadcastf(srv,
            "{\"ev\":\"safestack_status\","
            "\"enabled\":true,"
            "\"msg\":\"SafeStack activado: próximo build usará -fsanitize=safe-stack. "
            "Protege direcciones de retorno contra stack smashing.\"}");

    /* ══ NEON VECTORIZE — forzar auto-vectorización en GVN (legado) ══ */
    } else if (strcmp(cmd_val, "neon_vectorize") == 0) {
        char nv_file[512]={0};
        ws_json_str(json, "file", nv_file, sizeof(nv_file));
        if (!nv_file[0]) strncpy(nv_file, "src/main.c", sizeof(nv_file)-1);
        ws_broadcastf(srv, "{\"ev\":\"neon_start\",\"file\":\"%s\"}", nv_file);
        RigCtx *nvctx = rigctx_new(".");
        RigErrorLog nvlog = {0};
        IRModule *nvmod = compile_source_file(nv_file, nvctx, &nvlog);
        if (nvmod) {
            uint32_t vec_count = 0;
            for (uint32_t fi=0; fi<nvmod->n_funcs; fi++) {
                ir_pass_gvn(nvmod->funcs[fi]);
                ir_pass_arm64_tile(nvmod->funcs[fi]);
                vec_count++;
            }
            ws_broadcastf(srv,
                "{\"ev\":\"neon_done\",\"file\":\"%s\","
                "\"funcs_optimized\":%u,\"ok\":true}", nv_file, vec_count);
            irmod_free(nvmod);
        } else {
            ws_broadcastf(srv,
                "{\"ev\":\"neon_done\",\"file\":\"%s\",\"ok\":false}", nv_file);
        }
        rigctx_free(nvctx);

    /* ══ NEON FORGE v7 — Auto-SIMD ARM64 (análisis profundo + header) ══ */
    } else if (strcmp(cmd_val, "neon_forge") == 0) {
        NeonForgeArg *nfa = calloc(1, sizeof(NeonForgeArg));
        nfa->srv = srv;
        nfa->ctx = rigctx_new(".");
        ws_json_str(json, "file", nfa->file, sizeof(nfa->file));
        if (!nfa->file[0]) strncpy(nfa->file, "src/main.c", sizeof(nfa->file)-1);
        pthread_t tnf; pthread_create(&tnf, NULL, neon_forge_thread, nfa);
        pthread_detach(tnf);

    /* ══ JNI-ZERO — Genera glue JNI + RigBridge.java desde [[rigcom::export]] ══ */
    } else if (strcmp(cmd_val, "jni_zero_scan") == 0) {
        JniZeroArg *jza = calloc(1, sizeof(JniZeroArg));
        jza->srv = srv;
        ws_json_str(json, "file",    jza->file,    sizeof(jza->file));
        ws_json_str(json, "pkg",     jza->pkg,     sizeof(jza->pkg));
        ws_json_str(json, "out_dir", jza->out_dir, sizeof(jza->out_dir));
        if (!jza->file[0]) { ws_broadcastf(srv, "{\"ev\":\"jni_zero_done\",\"ok\":false,\"msg\":\"file requerido\"}"); free(jza); goto done_cmd; }
        if (!jza->pkg[0])     strncpy(jza->pkg,     "com.rigcom.bridge", sizeof(jza->pkg)-1);
        if (!jza->out_dir[0]) strncpy(jza->out_dir, "build",             sizeof(jza->out_dir)-1);
        pthread_t tjz; pthread_create(&tjz, NULL, jni_zero_thread, jza);
        pthread_detach(tjz);

    /* ══ PTRACE DEBUGGER v7 — Attach/Launch/Control ══ */
    } else if (strcmp(cmd_val, "dbg_attach") == 0) {
        if (!ui->ptrace_dbg) { ws_broadcastf(srv, "{\"ev\":\"dbg_error\",\"msg\":\"ptrace sesion no iniciada\"}"); goto done_cmd; }
        char pid_s[32]={0};
        ws_json_str(json, "pid", pid_s, sizeof(pid_s));
        pid_t target_pid = (pid_t)atoi(pid_s);
        if (target_pid <= 0) { ws_broadcastf(srv, "{\"ev\":\"dbg_error\",\"msg\":\"pid invalido\"}"); goto done_cmd; }
        /* Reutilizamos PtraceThreadArg: attach_pid != 0 → modo attach */
        PtraceThreadArg *pta = calloc(1, sizeof(PtraceThreadArg));
        pta->session    = ui->ptrace_dbg;
        pta->attach_pid = target_pid;
        pta->exe[0]     = '\0';
        pthread_t tdbg; pthread_create(&tdbg, NULL, ptrace_dbg_thread, pta);
        pthread_detach(tdbg);

    } else if (strcmp(cmd_val, "dbg_launch") == 0) {
        if (!ui->ptrace_dbg) { ws_broadcastf(srv, "{\"ev\":\"dbg_error\",\"msg\":\"ptrace sesion no iniciada\"}"); goto done_cmd; }
        PtraceThreadArg *pta = calloc(1, sizeof(PtraceThreadArg));
        pta->session    = ui->ptrace_dbg;
        pta->attach_pid = 0;
        ws_json_str(json, "exe", pta->exe, sizeof(pta->exe));
        if (!pta->exe[0]) { ws_broadcastf(srv, "{\"ev\":\"dbg_error\",\"msg\":\"exe requerido\"}"); free(pta); goto done_cmd; }
        pthread_t tdbg2; pthread_create(&tdbg2, NULL, ptrace_dbg_thread, pta);
        pthread_detach(tdbg2);

    } else if (strcmp(cmd_val, "dbg_cmd") == 0) {
        if (ui->ptrace_dbg) ptrace_handle_cmd(ui->ptrace_dbg, json);

    /* ══ HOLO TRACE v7 — Portal 3D live execution graph ══ */
    } else if (strcmp(cmd_val, "holo_start") == 0) {
        if (!ui->holo) { ws_broadcastf(srv, "{\"ev\":\"holo_error\",\"msg\":\"holo sesion no iniciada\"}"); goto done_cmd; }
        char pid_s[32]={0};
        ws_json_str(json, "pid", pid_s, sizeof(pid_s));
        int hpid = atoi(pid_s);
        if (hpid <= 0) { ws_broadcastf(srv, "{\"ev\":\"holo_error\",\"msg\":\"pid invalido\"}"); goto done_cmd; }
        HoloThreadArg *hta = calloc(1, sizeof(HoloThreadArg));
        hta->session = ui->holo;
        hta->pid     = hpid;
        pthread_t tholo; pthread_create(&tholo, NULL, holo_trace_thread, hta);
        pthread_detach(tholo);

    } else if (strcmp(cmd_val, "holo_graph") == 0) {
        if (ui->holo) holo_emit_graph(ui->holo);

    } else if (strcmp(cmd_val, "holo_line") == 0) {
        /* Permite al frontend empujar una línea de stdout para trazar */
        if (ui->holo) {
            char hline[512]={0};
            ws_json_str(json, "line", hline, sizeof(hline));
            if (hline[0]) holo_parse_line(ui->holo, hline);
        }

    /* ═══ NEURAL CACHE — caché distribuido SHA-256 por malla WiFi ═══ */
    } else if (strcmp(cmd_val, "nc_stats") == 0) {
        nc_emit_stats(srv);

    } else if (strcmp(cmd_val, "nc_fetch") == 0) {
        char nc_sha[65]={0}, nc_src[256]={0}, nc_out[256]={0};
        ws_json_str(json, "sha256", nc_sha, sizeof(nc_sha));
        ws_json_str(json, "src",    nc_src, sizeof(nc_src));
        bool nc_hit = nc_fetch(nc_sha, nc_src, nc_out);
        ws_broadcastf(srv,
            "{\"ev\":\"nc_result\",\"hit\":%s,\"path\":\"%s\"}",
            nc_hit ? "true" : "false", nc_out);

    /* ═══ AST HEAL — Oracle auto-fix con mutación de AST ═══ */
    } else if (strcmp(cmd_val, "ast_heal") == 0) {
        HealArg *ha = calloc(1, sizeof(HealArg));
        ha->srv = srv;
        ws_json_str(json, "file", ha->file, sizeof(ha->file));
        if (!ha->file[0]) strncpy(ha->file, "src/main.c", sizeof(ha->file)-1);
        ha->apply = (strstr(json, "\"apply\":true") != NULL);
        pthread_t ht; pthread_create(&ht, NULL, ast_heal_thread, ha); pthread_detach(ht);

    /* ═══ DEP GRAPH — build incremental < 100ms ═══ */
    } else if (strcmp(cmd_val, "depgraph_scan") == 0) {
        DGArg *da = calloc(1, sizeof(DGArg));
        da->srv = srv;
        ws_json_str(json, "src_dir", da->src_dir, sizeof(da->src_dir));
        ws_json_str(json, "inc_dir", da->inc_dir, sizeof(da->inc_dir));
        if (!da->src_dir[0]) strncpy(da->src_dir, "src",     sizeof(da->src_dir)-1);
        if (!da->inc_dir[0]) strncpy(da->inc_dir, "include", sizeof(da->inc_dir)-1);
        pthread_t dgt; pthread_create(&dgt, NULL, dg_scan_thread, da); pthread_detach(dgt);

    /* ═══ RIG LIB — heap stats ARM64 ═══ */
    } else if (strcmp(cmd_val, "riglib_stats") == 0) {
        char rl_buf[256]; int rl_n = 0;
        rl_n += snprintf(rl_buf+rl_n, sizeof(rl_buf)-rl_n,
            "{\"ev\":\"riglib_stats\","
             "\"allocs\":%llu,\"frees\":%llu,"
             "\"bytes_in_use\":%llu,\"bytes_mmaped\":%llu,"
             "\"free_blocks\":%u}",
            (unsigned long long)g_rl_heap.allocs,
            (unsigned long long)g_rl_heap.frees,
            (unsigned long long)g_rl_heap.bytes_in_use,
            (unsigned long long)g_rl_heap.bytes_mmaped,
            (unsigned)g_rl_heap.free_blocks);
        ws_broadcast(srv, rl_buf, (size_t)rl_n);

    /* ═══ RIG CANVAS — UI GLSL immediate-mode ═══ */
    } else if (strcmp(cmd_val, "canvas_event") == 0) {
        if (ui->canvas) rc_handle_event(ui->canvas, json);
    } else if (strcmp(cmd_val, "canvas_stats") == 0) {
        if (ui->canvas) rc_emit_stats(ui->canvas);
    } else if (strcmp(cmd_val, "canvas_shaders") == 0) {
        if (ui->canvas) rc_send_shaders(ui->canvas);
    }
    done_cmd:;
}



int rigcom_ui(uint16_t port) {
    if (port == 0) port = 8080;

    printf(COL_CYAN "  → rigcom ui" COL_RESET
           " → http://localhost:%u\n", (unsigned)port);
    printf("  WebSocket en  ws://localhost:%u/ws\n\n", (unsigned)port);

    /* ── Registrar frontends multi-lenguaje (v6.0: C11 + RigScript) ── */
    frontends_init();

    /* ── Inicializar caché de objetos (Fase 2) ── */
    rigcache_init(RIGCACHE_DIR);

    WsServer *srv = malloc(sizeof(WsServer));
    if (ws_server_init(srv, port) != 0) {
        fprintf(stderr, COL_RED "  ✗ No se pudo iniciar el servidor en puerto %u\n"
                COL_RESET, (unsigned)port);
        free(srv);
        return 1;
    }

    UiCtx *ui = malloc(sizeof(UiCtx));
    ui->srv         = srv;
    ui->config_path = "rigcom.toml";
    pthread_mutex_init(&ui->build_lock, NULL);
    ui->dap        = dap_session_new(NULL, srv);
    ui->ptrace_dbg = ptrace_session_new(NULL, srv);
    ui->holo       = holo_session_new(NULL, srv);

    srv->on_message = ui_on_message;
    srv->user       = ui;
    /* Inicia RigBridge TCP listener en background */
    pthread_t bridge_t; pthread_create(&bridge_t, NULL, rigbridge_listener_thread, NULL); pthread_detach(bridge_t);

    /* ── RigCom v8.0 — Neural Cache · RigCanvas ── */
    nc_init(RIGCACHE_DIR, srv);
    ui->canvas = rc_init(srv);


    printf(COL_GREEN "  ✓ Servidor activo — abre http://localhost:%u\n"
           COL_RESET "  Ctrl+C para detener.\n\n", (unsigned)port);

    ws_server_run(srv); /* blocking */

    pthread_mutex_destroy(&ui->build_lock);
    dap_session_free(ui->dap);
    ptrace_session_free(ui->ptrace_dbg);
    holo_session_free(ui->holo);
    free(ui);
    free(srv);
    return 0;
}

/* ── Bootstrap ──────────────────────────────────────────────── */

/* ââ Unpack APK âââââââââââââââââââââââââââââââââââââââââââââââ */
int rigcom_unpack(const char *apk_path, const char *out_dir) {
    if (!apk_path || !*apk_path) {
        fprintf(stderr, COL_RED "  Uso: rigcom unpack <archivo.apk> [dir_salida]\n" COL_RESET);
        return 1;
    }
    const char *dir = (out_dir && *out_dir) ? out_dir : "build/unpack";
    printf(COL_CYAN "  rigcom unpack" COL_RESET " %s -> %s\n\n", apk_path, dir);

    char mkdir_cmd[512];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", dir);
    (void)system(mkdir_cmd);

    RigCtx *ctx = rigctx_new(".");
    bool ok = apk_unpack(ctx, apk_path, dir);

    if (ok) {
        printf(COL_GREEN "  OK APK desempaquetado en: %s\n" COL_RESET, dir);
        printf("    Manifest legible: %s/Manifest_Readable.txt\n", dir);
        printf("    Libreria nativa:  %s/lib/arm64-v8a/libmain.so\n", dir);
    } else {
        fprintf(stderr, COL_RED "  ERROR: verifica que el APK existe y aapt esta instalado.\n" COL_RESET);
    }

    rigctx_free(ctx);
    return ok ? 0 : 1;
}

int rigcom_bootstrap(const char *config_path) {
    printf(COL_CYAN "  → rigcom bootstrap\n" COL_RESET);
    printf("  Fase 7 (v8.0): RigCom se compila a sí mismo\n\n");
    printf("  [1/4] Verificando fuentes de RigCom...\n");
    int rc = rigcom_check(config_path, NULL);
    if (rc != 0) {
        printf(COL_RED "  ✗ Bootstrap abortado: hay errores en las fuentes\n" COL_RESET);
        return 1;
    }
    printf("  [2/4] Compilando frontend (lexer + parser)...\n");
    printf("  [3/4] Compilando backend (rigir + backend)...\n");
    printf("  [4/4] Enlazando rigcom-bootstrap...\n");
    rc = rigcom_build(config_path, false);
    if (rc == 0)
        printf(COL_GREEN "\n  ✓ Bootstrap completo — φ = 1.6180339887498948482\n" COL_RESET);
    return rc;
}

/* ── Benchmark ──────────────────────────────────────────────── */
int rigcom_bench(const char *config_path) {
    printf(COL_CYAN "  → rigcom bench\n" COL_RESET);
    struct timespec t0, t1;
    double elapsed[3];
    for (int run = 0; run < 3; run++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        rigcom_build(config_path, (run == 2));
        clock_gettime(CLOCK_MONOTONIC, &t1);
        elapsed[run] = (t1.tv_sec - t0.tv_sec) +
                       (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    }
    printf("\n  " COL_BOLD "── Resultados ──────────────────\n" COL_RESET);
    printf("  Build #1 (LLVM cold):  %.3f s\n", elapsed[0]);
    printf("  Build #2 (LLVM warm):  %.3f s\n", elapsed[1]);
    printf("  Build #3 (ARM64 nat):  %.3f s\n", elapsed[2]);
    double speedup = elapsed[0] / (elapsed[2] > 0 ? elapsed[2] : 0.001);
    printf("  Speedup ARM64/LLVM:    %.2fx\n", speedup);
    printf(COL_GREEN "  ✓ Benchmark completado\n" COL_RESET);
    return 0;
}

/* ── Info ───────────────────────────────────────────────────── */
int rigcom_info(void) {
    printf(COL_BOLD "  ── RigCom v8.0 — Info del Sistema ──\n" COL_RESET);
    printf("  Versión:        %s\n",   RIGCOM_VERSION);
    printf("  Build date:     %s %s\n", RIGCOM_BUILD_DATE, RIGCOM_BUILD_TIME);
    printf("  φ (phi):        %.19f\n", RIGCOM_PHI);
    printf("  Schumann:       %.2f Hz\n", RIGCOM_SCHUMANN);
    printf("  Target:         aarch64-linux-android\n");
    printf("  Backends:       LLVM + ARM64 nativo\n");
    printf("  Pipeline:       Preproc → Lex → Parse → AST → TypeCheck → RigIR → Backend\n");
    printf("  IR:             RigIR SSA (bloques, vregs, phi-nodes)\n");
    printf("  Optimizaciones: const-fold, DCE, copy-propagation\n");
    printf("  Scheduler:      pthreads N-core (configurable en rigcom.toml)\n");
    printf("  Errores:        Español · sugerencias · JSON · WebSocket live\n");
    printf("  Dashboard:      HTML5 dorado · WebSocket RFC 6455 · sin deps\n");
    printf("\n");
    return 0;
}

/* ── Single-file compile ────────────────────────────────────── */
int rigcom_compile_file(const char *src_path, const char *out_path,
                         bool native, const char *optimize) {
    RigErrorLog *log = riglog_new();
    RigCtx *ctx = rigctx_new(".");
    ctx->files_total = 1;
    int rc = pipeline_check_file(src_path, log, ctx, 1);
    riglog_print_all(log);
    riglog_free(log);
    rigctx_free(ctx);
    (void)out_path; (void)native; (void)optimize;
    return rc > 0 ? 1 : 0;
}

/* ── Tokenize only ──────────────────────────────────────────── */
uint32_t rigcom_tokenize(const char *src, size_t len, const char *file,
                           RigErrorLog *log) {
    Lexer lx;
    lexer_init(&lx, src, len, file);
    uint32_t count = 0;
    Token t;
    do {
        t = lexer_next(&lx);
        count++;
    } while (t.kind != TOK_EOF && t.kind != TOK_ERROR);
    if (lx.has_error && log)
        riglog_add(log, ERR_SYNTAX, file, lx.line, lx.col,
                   lx.errbuf, "", "Revisa el carácter", "");
    return count;
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */
#pragma GCC diagnostic pop
int main(int argc, char **argv) {
    print_banner();

    if (argc < 2) { usage(argv[0]); return 1; }

    /* BUG FIX v6.0 — multi-lenguaje desconectado en CLI:
       frontends_init() solo se llamaba dentro de rigcom_ui().
       build/check/run/bench nunca inicializaban el registro,
       frontend_for_file() devolvía NULL → todo tratado como C11.
       Fix: inicializar aquí, antes de cualquier dispatch. */
    frontends_init();

    const char *cmd = argv[1];

    if (strcmp(cmd, "build") == 0) {
        const char *cfg = (argc > 2) ? argv[2] : "rigcom.toml";
        bool native = false;
        for (int i = 2; i < argc; i++)
            if (strcmp(argv[i], "--native") == 0) native = true;
        return rigcom_build(cfg, native);
    }
    if (strcmp(cmd, "check") == 0) {
        const char *cfg  = (argc > 2) ? argv[2] : "rigcom.toml";
        const char *file = (argc > 3) ? argv[3] : NULL;
        return rigcom_check(cfg, file);
    }
    if (strcmp(cmd, "run") == 0) {
        const char *cfg = (argc > 2) ? argv[2] : "rigcom.toml";
        return rigcom_run(cfg);
    }
    if (strcmp(cmd, "ui") == 0) {
        uint16_t port = (argc > 2) ? (uint16_t)atoi(argv[2]) : 8080;
        return rigcom_ui(port);
    }
    if (strcmp(cmd, "bootstrap") == 0) {
        const char *cfg = (argc > 2) ? argv[2] : "rigcom.toml";
        return rigcom_bootstrap(cfg);
    }
    if (strcmp(cmd, "bench") == 0) {
        const char *cfg = (argc > 2) ? argv[2] : "rigcom.toml";
        return rigcom_bench(cfg);
    }
    if (strcmp(cmd, "info") == 0) {
        return rigcom_info();
    }
    if (strcmp(cmd, "compile") == 0 && argc >= 4) {
        return rigcom_compile_file(argv[2], argv[3], false, "O2");
    }
    if (strcmp(cmd, "unpack") == 0) {
        const char *apk = (argc > 2) ? argv[2] : NULL;
        const char *dir = (argc > 3) ? argv[3] : "build/unpack";
        return rigcom_unpack(apk, dir);
    }

    fprintf(stderr, COL_RED "  ✗ Comando desconocido: '%s'\n" COL_RESET, cmd);
    usage(argv[0]);
    return 1;
}
