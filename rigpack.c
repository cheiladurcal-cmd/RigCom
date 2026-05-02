#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — src/rigpack.c
   RigPack: instalación automática de librerías para ARM64
   Flujo: pkg-config → termux pkg → apt-get → source build
   Emite WS: rigpack_progress · rigpack_done · rigpack_list
   ============================================================ */
#include "../include/rigpack.h"
#include "../include/wsserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── Tabla de nombres canónicos → paquetes del SO ─────────── */
typedef struct {
    const char *name;          /* nombre que escribe el usuario  */
    const char *termux_pkg;    /* nombre en termux               */
    const char *apt_pkg;       /* nombre en apt-get              */
    const char *pkgconf_name;  /* nombre en pkg-config           */
    const char *cflags_extra;  /* flags adicionales de compilación*/
    const char *ldflags;       /* flags de linkeo                */
} RigPackEntry;

static const RigPackEntry PACK_TABLE[] = {
    {"sqlite3",   "libsqlite",        "libsqlite3-dev",
     "sqlite3",   "",                 "-lsqlite3"},
    {"openssl",   "openssl",          "libssl-dev",
     "openssl",   "",                 "-lssl -lcrypto"},
    {"ssl",       "openssl",          "libssl-dev",
     "openssl",   "",                 "-lssl -lcrypto"},
    {"crypto",    "openssl",          "libssl-dev",
     "openssl",   "",                 "-lssl -lcrypto"},
    {"curl",      "libcurl",          "libcurl4-openssl-dev",
     "libcurl",   "",                 "-lcurl"},
    {"zlib",      "zlib",             "zlib1g-dev",
     "zlib",      "",                 "-lz"},
    {"z",         "zlib",             "zlib1g-dev",
     "zlib",      "",                 "-lz"},
    {"json-c",    "json-c",           "libjson-c-dev",
     "json-c",    "",                 "-ljson-c"},
    {"jsonc",     "json-c",           "libjson-c-dev",
     "json-c",    "",                 "-ljson-c"},
    {"libpng",    "libpng",           "libpng-dev",
     "libpng",    "",                 "-lpng"},
    {"png",       "libpng",           "libpng-dev",
     "libpng",    "",                 "-lpng"},
    {"sdl2",      "libsdl2",          "libsdl2-dev",
     "sdl2",      "",                 "-lSDL2"},
    {"SDL2",      "libsdl2",          "libsdl2-dev",
     "sdl2",      "",                 "-lSDL2"},
    {"raylib",    "raylib",           "libraylib-dev",
     "raylib",    "",                 "-lraylib"},
    {"ncurses",   "ncurses",          "libncurses-dev",
     "ncurses",   "",                 "-lncurses"},
    {"pthread",   NULL,               NULL,
     NULL,        "",                 "-lpthread"},
    {"math",      NULL,               NULL,
     NULL,        "",                 "-lm"},
    {"m",         NULL,               NULL,
     NULL,        "",                 "-lm"},
    {"uuid",      NULL,               "uuid-dev",
     "uuid",      "",                 "-luuid"},
    {NULL, NULL, NULL, NULL, NULL, NULL}
};

/* ─── Helpers internos ─────────────────────────────────────── */

static int64_t ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void pack_progress(WsServer *srv, const char *lib,
                          const char *stage, const char *msg) {
    ws_broadcastf(srv,
        "{\"ev\":\"rigpack_progress\","
        "\"lib\":\"%s\",\"stage\":\"%s\",\"msg\":\"%s\"}",
        lib, stage, msg);
}

/* Detecta si estamos en Termux */
static bool is_termux(void) {
    return (getenv("TERMUX_VERSION") != NULL ||
            getenv("PREFIX") != NULL ||
            access("/data/data/com.termux", F_OK) == 0);
}

/* Ejecuta comando y captura stdout, retorna 0 en éxito */
static int run_cmd(const char *cmd, char *out, size_t outsz) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    size_t n = 0;
    if (out && outsz > 1) {
        n = fread(out, 1, outsz - 1, fp);
        out[n] = '\0';
        /* trim newlines */
        while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r'))
            out[--n] = '\0';
    }
    int rc = pclose(fp);
    return (rc == 0) ? 0 : -1;
}

/* Busca la entrada en la tabla por nombre (case-insensitive) */
static const RigPackEntry *pack_find(const char *lib) {
    for (int i = 0; PACK_TABLE[i].name; i++) {
        if (strcasecmp(PACK_TABLE[i].name, lib) == 0)
            return &PACK_TABLE[i];
    }
    return NULL;
}

/* ─── pkg-config probe ─────────────────────────────────────── */
static bool probe_pkgconfig(const char *pkgname,
                            char *cflags, size_t csz,
                            char *ldflags, size_t lsz) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "pkg-config --cflags %s 2>/dev/null", pkgname);
    if (run_cmd(cmd, cflags, csz) != 0) return false;

    snprintf(cmd, sizeof(cmd),
             "pkg-config --libs %s 2>/dev/null", pkgname);
    return run_cmd(cmd, ldflags, lsz) == 0;
}

/* ─── Actualiza rigcom.toml con nueva lib ──────────────────── */
bool rigpack_update_toml(const char *lib,
                         const char *cflags,
                         const char *ldflags) {
    /* Lee el toml actual */
    FILE *fp = fopen("rigcom.toml", "r");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    char *buf = malloc((size_t)sz + 4096);
    if (!buf) { fclose(fp); return false; }
    size_t nr = fread(buf, 1, (size_t)sz, fp);
    buf[nr] = '\0';
    fclose(fp);

    /* Busca sección [libs] o la crea */
    char *libs_sec = strstr(buf, "[libs]");
    char new_entry[1024];
    snprintf(new_entry, sizeof(new_entry),
             "\n# RigPack: %s\n"
             "[[libs]]\nname = \"%s\"\n"
             "cflags = \"%s\"\nldflags = \"%s\"\n",
             lib, lib,
             cflags  ? cflags  : "",
             ldflags ? ldflags : "");

    FILE *out = fopen("rigcom.toml", "w");
    if (!out) { free(buf); return false; }

    if (libs_sec) {
        /* Inserta después de [libs] */
        char *after = strchr(libs_sec, '\n');
        if (after) after++;
        else after = libs_sec + 6;
        fwrite(buf, 1, (size_t)(after - buf), out);
        fputs(new_entry, out);
        fputs(after, out);
    } else {
        /* Agrega al final */
        fputs(buf, out);
        fputs(new_entry, out);
    }
    fclose(out);
    free(buf);
    return true;
}

/* ─── Resolve flags ────────────────────────────────────────── */
char *rigpack_resolve_flags(const char *lib) {
    const RigPackEntry *e = pack_find(lib);
    if (!e) return NULL;
    if (e->ldflags && e->ldflags[0]) return strdup(e->ldflags);
    return NULL;
}

/* ─── Thread principal: instala ────────────────────────────── */
void *rigpack_install_thread(void *arg) {
    RigPackArg *rp = (RigPackArg *)arg;
    WsServer   *srv = rp->srv;
    char        lib[256];
    strncpy(lib, rp->lib, sizeof(lib)-1);
    lib[sizeof(lib)-1] = '\0';
    free(rp);

    int64_t t0 = ms_now();
    pack_progress(srv, lib, "resolving", "Buscando librería...");

    const RigPackEntry *entry = pack_find(lib);

    /* ── Fase 1: pkg-config probe ── */
    char cflags[512] = {0};
    char ldflags_buf[512] = {0};

    const char *pkgname = entry ? entry->pkgconf_name : lib;
    if (pkgname && probe_pkgconfig(pkgname, cflags, sizeof(cflags),
                                   ldflags_buf, sizeof(ldflags_buf))) {
        pack_progress(srv, lib, "found",
                      "Encontrada via pkg-config ✓");
        /* Ya disponible: solo actualiza toml */
        rigpack_update_toml(lib, cflags, ldflags_buf);
        int64_t ms = ms_now() - t0;
        ws_broadcastf(srv,
            "{\"ev\":\"rigpack_done\","
            "\"ok\":true,\"lib\":\"%s\","
            "\"cflags\":\"%s\",\"ldflags\":\"%s\","
            "\"method\":\"pkgconfig\",\"ms\":%lld}",
            lib, cflags, ldflags_buf, (long long)ms);
        return NULL;
    }

    /* ── Fase 2: instala paquete del SO ── */
    bool in_termux = is_termux();
    const char *pkg_bin = in_termux ? "pkg" : "apt-get";
    const char *pkg_name = NULL;

    if (entry) {
        pkg_name = in_termux ? entry->termux_pkg : entry->apt_pkg;
    }
    if (!pkg_name) {
        /* Intenta nombre genérico: lib<name>-dev o lib<name> */
        char guess[320];
        if (in_termux) snprintf(guess, sizeof(guess), "lib%s", lib);
        else           snprintf(guess, sizeof(guess), "lib%s-dev", lib);
        pkg_name = guess;
    }

    char msg[512];
    snprintf(msg, sizeof(msg), "Instalando %s install %s ...",
             pkg_bin, pkg_name);
    pack_progress(srv, lib, "installing", msg);

    char cmd[1024];
    if (in_termux) {
        snprintf(cmd, sizeof(cmd),
                 "pkg install -y %s 2>&1 | tail -5", pkg_name);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "apt-get install -y %s 2>&1 | tail -5", pkg_name);
    }

    char install_out[2048] = {0};
    int rc = run_cmd(cmd, install_out, sizeof(install_out));

    if (rc != 0) {
        /* Emite error pero con output real para debug */
        ws_broadcastf(srv,
            "{\"ev\":\"rigpack_done\","
            "\"ok\":false,\"lib\":\"%s\","
            "\"msg\":\"Instalación fallida. Output: %s\","
            "\"hint\":\"Verifica conexión o instala manualmente\"}",
            lib,
            install_out[0] ? install_out :
            "pkg manager no disponible");
        return NULL;
    }

    /* ── Fase 3: pkg-config post-install probe ── */
    pack_progress(srv, lib, "probing", "Verificando flags...");

    if (pkgname && probe_pkgconfig(pkgname, cflags, sizeof(cflags),
                                   ldflags_buf, sizeof(ldflags_buf))) {
        /* OK, got real flags */
    } else if (entry && entry->ldflags) {
        /* Usa flags conocidas de la tabla */
        strncpy(ldflags_buf, entry->ldflags, sizeof(ldflags_buf)-1);
    } else {
        snprintf(ldflags_buf, sizeof(ldflags_buf), "-l%s", lib);
    }

    rigpack_update_toml(lib, cflags, ldflags_buf);

    int64_t ms = ms_now() - t0;
    ws_broadcastf(srv,
        "{\"ev\":\"rigpack_done\","
        "\"ok\":true,\"lib\":\"%s\","
        "\"cflags\":\"%s\",\"ldflags\":\"%s\","
        "\"method\":\"%s\",\"ms\":%lld}",
        lib, cflags, ldflags_buf,
        in_termux ? "termux-pkg" : "apt-get",
        (long long)ms);
    return NULL;
}

/* ─── Emite lista de libs instaladas/disponibles ───────────── */
void rigpack_emit_list(WsServer *srv) {
    /* Construye JSON array con tabla + estado real */
    char buf[8192];
    char *p = buf;
    size_t rem = sizeof(buf);

    int n = snprintf(p, rem, "{\"ev\":\"rigpack_list\",\"libs\":[");
    p += n; rem -= (size_t)n;

    bool first = true;
    for (int i = 0; PACK_TABLE[i].name; i++) {
        const RigPackEntry *e = &PACK_TABLE[i];

        /* Comprueba si ya está instalada */
        bool installed = false;
        char cflags[256] = {0}, ldf[256] = {0};
        if (e->pkgconf_name) {
            installed = probe_pkgconfig(e->pkgconf_name,
                                        cflags, sizeof(cflags),
                                        ldf,    sizeof(ldf));
        }
        if (!installed && e->termux_pkg) {
            char chk[512];
            snprintf(chk, sizeof(chk),
                     "pkg list-installed 2>/dev/null | grep -q '^%s'",
                     e->termux_pkg);
            installed = (system(chk) == 0);
        }

        int nc = snprintf(p, rem,
            "%s{\"name\":\"%s\","
            "\"pkg_termux\":\"%s\","
            "\"pkg_apt\":\"%s\","
            "\"ldflags\":\"%s\","
            "\"installed\":%s}",
            first ? "" : ",",
            e->name,
            e->termux_pkg  ? e->termux_pkg  : "",
            e->apt_pkg     ? e->apt_pkg     : "",
            e->ldflags     ? e->ldflags     : "",
            installed ? "true" : "false");
        if (nc < 0 || (size_t)nc >= rem) break;
        p += nc; rem -= (size_t)nc;
        first = false;
    }

    snprintf(p, rem, "]}");
    ws_broadcast(srv, buf, strlen(buf));
}
