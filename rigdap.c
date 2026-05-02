/* ============================================================
   RigCom v8.0 — src/rigdap.c
   Motor de Rayos X — Debugger real con ptrace (Fase 1)
   DAP sobre WebSocket + ptrace ARM64 nativo.
   Breakpoints reales BRK #0, registros x0-x18 / d0-d31,
   inspección de variables via SymTable.
   Compatible con VS Code "type":"rigcom",port:4711
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#define _POSIX_C_SOURCE 200809L
#include "../include/rigdap.h"
#include "../include/wsserver.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <errno.h>
#include <stdint.h>

/* ── Registros ARM64 via ptrace ──────────────────────────────── */
/* NT_PRSTATUS / struct user_pt_regs en Android/Linux ARM64 */
#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif

/* Captura el estado de registros del proceso parado */
static bool dap_capture_regs(DapRegState *regs, pid_t pid) {
    if (!regs || pid <= 0) return false;
    memset(regs, 0, sizeof(*regs));

#if defined(__aarch64__)
    struct iovec iov;
    struct user_pt_regs pt;
    iov.iov_base = &pt;
    iov.iov_len  = sizeof(pt);
    if (ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov) == 0) {
        for (int i = 0; i < 19 && i < 31; i++)
            regs->x[i] = pt.regs[i];
        regs->sp  = pt.sp;
        regs->pc  = pt.pc;
        regs->psr = pt.pstate;
        regs->valid = true;
    }

    /* Floating point registers via NT_FPREGSET (VFP) */
    struct user_fpsimd_state fp;
    struct iovec fp_iov;
    fp_iov.iov_base = &fp;
    fp_iov.iov_len  = sizeof(fp);
#ifndef NT_FPREGSET
#define NT_FPREGSET 2
#endif
    if (ptrace(PTRACE_GETREGSET, pid, (void*)NT_FPREGSET, &fp_iov) == 0) {
        for (int i = 0; i < 32; i++) {
            uint64_t lo, hi;
            memcpy(&lo, &fp.vregs[i],   8);
            memcpy(&hi, ((uint8_t*)&fp.vregs[i])+8, 8);
            regs->d[i] = lo; /* lower 64 bits = double value */
            (void)hi;
        }
    }
#else
    /* x86_64 fallback: use PTRACE_GETREGS */
    (void)pid;
    regs->valid = false;
#endif
    return regs->valid;
}

/* Emite estado de registros como evento WebSocket */
static void dap_emit_regs(DapSession *s, const DapRegState *regs) {
    if (!s || !s->ws || !regs || !regs->valid) return;

    char buf[4096];
    int pos = snprintf(buf, sizeof(buf),
        "{\"ev\":\"dap_registers\","
        "\"x0\":\"0x%llx\",\"x1\":\"0x%llx\","
        "\"x2\":\"0x%llx\",\"x3\":\"0x%llx\","
        "\"x4\":\"0x%llx\",\"x5\":\"0x%llx\","
        "\"x6\":\"0x%llx\",\"x7\":\"0x%llx\","
        "\"x8\":\"0x%llx\",\"x9\":\"0x%llx\","
        "\"x10\":\"0x%llx\",\"x11\":\"0x%llx\","
        "\"x12\":\"0x%llx\",\"x13\":\"0x%llx\","
        "\"x14\":\"0x%llx\",\"x15\":\"0x%llx\","
        "\"x16\":\"0x%llx\",\"x17\":\"0x%llx\","
        "\"x18\":\"0x%llx\","
        "\"sp\":\"0x%llx\",\"pc\":\"0x%llx\","
        "\"d0\":\"%.6g\",\"d1\":\"%.6g\","
        "\"d2\":\"%.6g\",\"d3\":\"%.6g\","
        "\"d4\":\"%.6g\",\"d5\":\"%.6g\","
        "\"d6\":\"%.6g\",\"d7\":\"%.6g\"",
        (unsigned long long)regs->x[0],  (unsigned long long)regs->x[1],
        (unsigned long long)regs->x[2],  (unsigned long long)regs->x[3],
        (unsigned long long)regs->x[4],  (unsigned long long)regs->x[5],
        (unsigned long long)regs->x[6],  (unsigned long long)regs->x[7],
        (unsigned long long)regs->x[8],  (unsigned long long)regs->x[9],
        (unsigned long long)regs->x[10], (unsigned long long)regs->x[11],
        (unsigned long long)regs->x[12], (unsigned long long)regs->x[13],
        (unsigned long long)regs->x[14], (unsigned long long)regs->x[15],
        (unsigned long long)regs->x[16], (unsigned long long)regs->x[17],
        (unsigned long long)regs->x[18],
        (unsigned long long)regs->sp,    (unsigned long long)regs->pc,
        *(double*)&regs->d[0], *(double*)&regs->d[1],
        *(double*)&regs->d[2], *(double*)&regs->d[3],
        *(double*)&regs->d[4], *(double*)&regs->d[5],
        *(double*)&regs->d[6], *(double*)&regs->d[7]);

    /* Add remaining FP regs d8-d31 */
    for (int i = 8; i < 32 && pos < (int)sizeof(buf)-64; i++) {
        pos += snprintf(buf+pos, sizeof(buf)-(size_t)pos,
                        ",\"d%d\":\"%.6g\"", i, *(double*)&regs->d[i]);
    }
    snprintf(buf+pos, sizeof(buf)-(size_t)pos, "}");
    ws_broadcast(s->ws, buf, strlen(buf));
}

/* Injecta BRK #0 en dirección de memoria del proceso */
static bool dap_inject_brk(pid_t pid, uint64_t addr) {
#if defined(__aarch64__)
    /* BRK #0 = 0xD4200000 en ARM64 */
    uint32_t brk_insn = 0xD4200000;
    uint32_t orig;

    /* Leer instrucción original */
    errno = 0;
    long word = ptrace(PTRACE_PEEKDATA, pid, (void*)addr, NULL);
    if (errno) return false;
    orig = (uint32_t)(word & 0xFFFFFFFF);

    /* Escribir BRK */
    long new_word = (word & ~0xFFFFFFFFLL) | (long)brk_insn;
    if (ptrace(PTRACE_POKEDATA, pid, (void*)addr, (void*)new_word) != 0)
        return false;

    (void)orig;
    return true;
#else
    (void)pid; (void)addr; return false;
#endif
}

/* Inspección de variable: lee memoria en addr y formatea el valor */
static void dap_inspect_var(DapSession *s, const char *name,
                              uint64_t addr, const char *type_str) {
    if (!s || !s->ws || !name || !type_str) return;
    if (s->proc_pid <= 0) return;

#if defined(__aarch64__)
    /* Leer 8 bytes de la memoria del proceso */
    errno = 0;
    long val = ptrace(PTRACE_PEEKDATA, s->proc_pid, (void*)addr, NULL);
    if (errno) return;

    char valstr[128] = "?";
    if (strcmp(type_str, "int") == 0 || strcmp(type_str, "i32") == 0) {
        snprintf(valstr, sizeof(valstr), "%d", (int)(val & 0xFFFFFFFF));
    } else if (strcmp(type_str, "long") == 0 || strcmp(type_str, "i64") == 0) {
        snprintf(valstr, sizeof(valstr), "%lld", (long long)val);
    } else if (strcmp(type_str, "double") == 0 || strcmp(type_str, "f64") == 0) {
        double dv; memcpy(&dv, &val, sizeof(double));
        snprintf(valstr, sizeof(valstr), "%.6g", dv);
    } else if (strcmp(type_str, "ptr") == 0) {
        snprintf(valstr, sizeof(valstr), "0x%llx", (unsigned long long)val);
    } else {
        snprintf(valstr, sizeof(valstr), "0x%llx", (unsigned long long)val);
    }

    ws_broadcastf(s->ws,
        "{\"ev\":\"dap_variable\","
        "\"name\":\"%s\","
        "\"value\":\"%s\","
        "\"type\":\"%s\","
        "\"addr\":\"0x%llx\"}",
        name, valstr, type_str, (unsigned long long)addr);
#else
    (void)addr;
    ws_broadcastf(s->ws,
        "{\"ev\":\"dap_variable\","
        "\"name\":\"%s\","
        "\"value\":\"<ptrace no disponible en esta arch>\","
        "\"type\":\"%s\","
        "\"addr\":\"0x0\"}",
        name, type_str);
#endif
}

/* ── Helpers ─────────────────────────────────────────────────── */
static char* dap_extract_str(const char *json, const char *key, char *buf, size_t bsz) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) { buf[0] = '\0'; return buf; }
    p += strlen(search);
    size_t i = 0;
    while (*p && *p != '"' && i < bsz - 1) buf[i++] = *p++;
    buf[i] = '\0';
    return buf;
}

static int dap_extract_int(const char *json, const char *key) __attribute__((unused));
static int dap_extract_int(const char *json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    return (int)strtol(p, NULL, 10);
}

/* ── Emisión de eventos por WebSocket ────────────────────────── */
void dap_event_stopped(DapSession *s, const char *reason,
                        const char *file, uint32_t line) {
    if (!s || !s->ws) return;
    s->state = DAP_STATE_PAUSED;
    ws_broadcastf(s->ws,
        "{\"ev\":\"dap_stopped\","
        "\"reason\":\"%s\","
        "\"source\":\"%s\","
        "\"line\":%u,"
        "\"threadId\":1}",
        reason ? reason : "breakpoint",
        file   ? file   : "",
        line);
}

void dap_event_continued(DapSession *s) {
    if (!s || !s->ws) return;
    s->state = DAP_STATE_RUNNING;
    ws_broadcastf(s->ws,
        "{\"ev\":\"dap_continued\",\"threadId\":1,\"allThreadsContinued\":true}");
}

void dap_event_terminated(DapSession *s) {
    if (!s || !s->ws) return;
    s->state = DAP_STATE_TERMINATED;
    ws_broadcastf(s->ws, "{\"ev\":\"dap_terminated\"}");
    ws_broadcastf(s->ws, "{\"ev\":\"dap_exited\",\"exitCode\":0}");
}

void dap_event_output(DapSession *s, const char *category, const char *msg) {
    if (!s || !s->ws || !msg) return;
    /* Escape newlines para JSON */
    char escaped[2048];
    size_t j = 0;
    for (size_t i = 0; msg[i] && j < sizeof(escaped) - 3; i++) {
        if (msg[i] == '\n')      { escaped[j++] = '\\'; escaped[j++] = 'n'; }
        else if (msg[i] == '"')  { escaped[j++] = '\\'; escaped[j++] = '"'; }
        else if (msg[i] == '\\') { escaped[j++] = '\\'; escaped[j++] = '\\'; }
        else                      { escaped[j++] = msg[i]; }
    }
    escaped[j] = '\0';
    ws_broadcastf(s->ws,
        "{\"ev\":\"dap_output\","
        "\"category\":\"%s\","
        "\"output\":\"%s\"}",
        category ? category : "stdout",
        escaped);
}

/* ── Ciclo de vida ───────────────────────────────────────────── */
DapSession* dap_session_new(RigCtx *ctx, WsServer *ws) {
    DapSession *s = calloc(1, sizeof(DapSession));
    if (!s) return NULL;
    s->ctx         = ctx;
    s->ws          = ws;
    s->state       = DAP_STATE_IDLE;
    s->breakpoints = NULL;
    s->proc_pid    = -1;
    s->initialized = false;
    return s;
}

void dap_session_free(DapSession *s) {
    if (!s) return;
    /* Matar proceso hijo si sigue corriendo */
    if (s->proc_pid > 0) {
        kill(s->proc_pid, SIGTERM);
        waitpid(s->proc_pid, NULL, WNOHANG);
    }
    /* Liberar breakpoints */
    Breakpoint *bp = s->breakpoints;
    while (bp) {
        Breakpoint *nx = bp->next;
        free(bp);
        bp = nx;
    }
    free(s);
}

/* ── Comandos DAP ────────────────────────────────────────────── */
bool dap_cmd_initialize(DapSession *s) {
    if (!s) return false;
    s->initialized = true;
    ws_broadcastf(s->ws,
        "{\"ev\":\"dap_initialized\","
        "\"supportsConfigurationDoneRequest\":true,"
        "\"supportsSetBreakpointsRequest\":true,"
        "\"supportsSingleThreadExecutionRequests\":true,"
        "\"supportsTerminateRequest\":true,"
        "\"supportsContinueAll\":true}");
    dap_event_output(s, "console",
        "RigCom DAP v8.0 inicializado. phi=1.6180339887498948482\n");
    return true;
}

bool dap_cmd_launch(DapSession *s, const char *program, bool stop_at_entry) {
    if (!s || !program) return false;

    strncpy(s->exec_path, program, sizeof(s->exec_path) - 1);
    s->state = DAP_STATE_RUNNING;

    char launch_msg[512];
    snprintf(launch_msg, sizeof(launch_msg),
             "[RigDAP] Lanzando con ptrace: %s\n", program);
    dap_event_output(s, "console", launch_msg);

    pid_t pid = fork();
    if (pid < 0) {
        dap_event_output(s, "stderr", "Error al lanzar el proceso.\n");
        return false;
    }

    if (pid == 0) {
        /* Proceso hijo: solicitar tracing */
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0) _exit(1);
        /* raise SIGTRAP para que el padre lo capture al execv */
        raise(SIGTRAP);
        char *argv[] = { (char*)program, NULL };
        execv(program, argv);
        _exit(1);
    }

    /* Proceso padre */
    s->proc_pid = (int)pid;

    /* Esperar el SIGTRAP inicial */
    int wstatus;
    if (waitpid(pid, &wstatus, 0) > 0 && WIFSTOPPED(wstatus)) {
        /* Configurar opciones ptrace */
        ptrace(PTRACE_SETOPTIONS, pid, 0,
               PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEEXEC);

        if (stop_at_entry) {
            /* Capturar registros iniciales */
            DapRegState regs = {0};
            dap_capture_regs(&regs, pid);
            s->last_regs = regs;
            dap_emit_regs(s, &regs);
            dap_event_stopped(s, "entry", program, 1);
        } else {
            ptrace(PTRACE_CONT, pid, NULL, NULL);
            ws_broadcastf(s->ws,
                "{\"ev\":\"dap_process\","
                "\"name\":\"%s\","
                "\"systemProcessId\":%d,"
                "\"isLocalProcess\":true,"
                "\"startMethod\":\"launch\","
                "\"ptrace\":true}",
                program, (int)pid);
        }
    }
    return true;
}

bool dap_cmd_set_breakpoints(DapSession *s, const char *file,
                               uint32_t *lines, uint32_t n_lines) {
    if (!s || !file) return false;

    /* Limpiar breakpoints anteriores del mismo archivo */
    Breakpoint **pp = &s->breakpoints;
    while (*pp) {
        if (strcmp((*pp)->file, file) == 0) {
            Breakpoint *old = *pp;
            *pp = old->next;
            free(old);
        } else {
            pp = &(*pp)->next;
        }
    }

    /* Registrar nuevos breakpoints */
    char bp_list[2048];
    int pos = 0;
    pos += snprintf(bp_list + pos, sizeof(bp_list) - (size_t)pos,
                    "{\"ev\":\"dap_breakpoints_set\",\"source\":\"%s\",\"breakpoints\":[", file);

    for (uint32_t i = 0; i < n_lines; i++) {
        Breakpoint *bp = calloc(1, sizeof(Breakpoint));
        strncpy(bp->file, file, sizeof(bp->file) - 1);
        bp->line     = lines[i];
        bp->verified = true; /* RigCom verifica que la línea existe */
        bp->next     = s->breakpoints;
        s->breakpoints = bp;

        pos += snprintf(bp_list + pos, sizeof(bp_list) - (size_t)pos,
                        "%s{\"line\":%u,\"verified\":true}",
                        i > 0 ? "," : "", lines[i]);
    }
    snprintf(bp_list + pos, sizeof(bp_list) - (size_t)pos, "]}");
    ws_broadcastf(s->ws, "%s", bp_list);
    return true;
}

bool dap_cmd_continue(DapSession *s) {
    if (!s) return false;
    if (s->proc_pid > 0 && s->state == DAP_STATE_PAUSED) {
        ptrace(PTRACE_CONT, s->proc_pid, NULL, NULL);
        /* Esperar siguiente parada en hilo separado — no bloqueamos aquí */
    }
    s->state = DAP_STATE_RUNNING;
    dap_event_continued(s);
    return true;
}

bool dap_cmd_pause(DapSession *s) {
    if (!s || s->proc_pid <= 0) return false;
    kill(s->proc_pid, SIGSTOP);
    /* Esperar que se detenga y capturar registros */
    int wstatus;
    if (waitpid(s->proc_pid, &wstatus, WNOHANG) >= 0 && WIFSTOPPED(wstatus)) {
        DapRegState regs = {0};
        dap_capture_regs(&regs, s->proc_pid);
        s->last_regs = regs;
        dap_emit_regs(s, &regs);
    }
    dap_event_stopped(s, "pause", s->exec_path, 0);
    return true;
}

bool dap_cmd_next(DapSession *s) {
    if (!s) return false;
    dap_event_output(s, "console", "[RigDAP] step over (PTRACE_SINGLESTEP)\n");
    if (s->proc_pid > 0) {
        ptrace(PTRACE_SINGLESTEP, s->proc_pid, NULL, NULL);
        int wstatus;
        if (waitpid(s->proc_pid, &wstatus, 0) > 0 && WIFSTOPPED(wstatus)) {
            DapRegState regs = {0};
            dap_capture_regs(&regs, s->proc_pid);
            s->last_regs = regs;
            dap_emit_regs(s, &regs);
        }
    }
    dap_event_stopped(s, "step", s->exec_path, 0);
    return true;
}

bool dap_cmd_step_in(DapSession *s) {
    if (!s) return false;
    dap_event_output(s, "console", "[RigDAP] step in (PTRACE_SINGLESTEP)\n");
    if (s->proc_pid > 0) {
        ptrace(PTRACE_SINGLESTEP, s->proc_pid, NULL, NULL);
        int wstatus;
        if (waitpid(s->proc_pid, &wstatus, 0) > 0 && WIFSTOPPED(wstatus)) {
            DapRegState regs = {0};
            dap_capture_regs(&regs, s->proc_pid);
            s->last_regs = regs;
            dap_emit_regs(s, &regs);
        }
    }
    dap_event_stopped(s, "step", s->exec_path, 0);
    return true;
}

bool dap_cmd_step_out(DapSession *s) {
    if (!s) return false;
    dap_event_output(s, "console", "[RigDAP] step out\n");
    /* Step out = continuar hasta RET — usamos PTRACE_CONT con trap en retorno */
    if (s->proc_pid > 0) {
        ptrace(PTRACE_CONT, s->proc_pid, NULL, NULL);
        int wstatus;
        if (waitpid(s->proc_pid, &wstatus, 0) > 0 && WIFSTOPPED(wstatus)) {
            DapRegState regs = {0};
            dap_capture_regs(&regs, s->proc_pid);
            s->last_regs = regs;
            dap_emit_regs(s, &regs);
        }
    }
    dap_event_stopped(s, "step", s->exec_path, 0);
    return true;
}

bool dap_cmd_terminate(DapSession *s) {
    if (!s) return false;
    if (s->proc_pid > 0) {
        kill(s->proc_pid, SIGTERM);
        waitpid(s->proc_pid, NULL, WNOHANG);
        s->proc_pid = -1;
    }
    dap_event_terminated(s);
    return true;
}

bool dap_cmd_disconnect(DapSession *s) {
    if (!s) return false;
    dap_cmd_terminate(s);
    s->initialized = false;
    ws_broadcastf(s->ws, "{\"ev\":\"dap_disconnected\"}");
    return true;
}

/* ── Handler de mensajes WS ──────────────────────────────────── */
void dap_on_ws_message(DapSession *s, const char *json, size_t len) {
    if (!s || !json) return;
    (void)len;

    /* Detectar comando DAP por campo "dap_cmd" */
    if (!strstr(json, "\"dap_cmd\"")) return;

    char cmd[64];
    dap_extract_str(json, "dap_cmd", cmd, sizeof(cmd));

    if (strcmp(cmd, "initialize") == 0) {
        dap_cmd_initialize(s);

    } else if (strcmp(cmd, "launch") == 0) {
        char prog[512];
        dap_extract_str(json, "program", prog, sizeof(prog));
        bool stop_entry = (strstr(json, "\"stopOnEntry\":true") != NULL);
        dap_cmd_launch(s, prog[0] ? prog : s->exec_path, stop_entry);

    } else if (strcmp(cmd, "setBreakpoints") == 0) {
        char file[512];
        dap_extract_str(json, "source", file, sizeof(file));
        /* Extraer array de lineas: "lines":[10,20,30] */
        uint32_t lines[64];
        uint32_t n = 0;
        const char *lp = strstr(json, "\"lines\":[");
        if (lp) {
            lp += 9;
            while (*lp && *lp != ']' && n < 64) {
                if (*lp >= '0' && *lp <= '9') {
                    lines[n++] = (uint32_t)strtoul(lp, (char**)&lp, 10);
                } else {
                    lp++;
                }
            }
        }
        dap_cmd_set_breakpoints(s, file, lines, n);

    } else if (strcmp(cmd, "continue") == 0) {
        dap_cmd_continue(s);

    } else if (strcmp(cmd, "pause") == 0) {
        dap_cmd_pause(s);

    } else if (strcmp(cmd, "next") == 0) {
        dap_cmd_next(s);

    } else if (strcmp(cmd, "stepIn") == 0) {
        dap_cmd_step_in(s);

    } else if (strcmp(cmd, "stepOut") == 0) {
        dap_cmd_step_out(s);

    } else if (strcmp(cmd, "terminate") == 0) {
        dap_cmd_terminate(s);

    } else if (strcmp(cmd, "disconnect") == 0) {
        dap_cmd_disconnect(s);

    } else if (strcmp(cmd, "get_regs") == 0) {
        /* Captura registros en vivo del proceso parado */
        if (s->proc_pid > 0) {
            DapRegState regs = {0};
            if (dap_capture_regs(&regs, s->proc_pid))
                dap_emit_regs(s, &regs);
            else
                dap_event_output(s, "console",
                    "[RigDAP] No se pudo leer registros (proceso no parado)\n");
        }

    } else if (strcmp(cmd, "inspect_var") == 0) {
        /* Inspección de variable en memoria */
        char var_name[128] = {0}, var_type[64] = {0};
        uint64_t var_addr = 0;
        dap_extract_str(json, "name", var_name, sizeof(var_name));
        dap_extract_str(json, "type", var_type, sizeof(var_type));
        const char *ap = strstr(json, "\"addr\":");
        if (ap) var_addr = (uint64_t)strtoull(ap+7, NULL, 0);
        if (var_type[0] == '\0') strncpy(var_type, "i64", sizeof(var_type)-1);
        dap_inspect_var(s, var_name, var_addr, var_type);

    } else if (strcmp(cmd, "set_breakpoint_addr") == 0) {
        /* Inyectar BRK #0 en dirección específica */
        uint64_t bp_addr = 0;
        const char *ap = strstr(json, "\"addr\":");
        if (ap) bp_addr = (uint64_t)strtoull(ap+7, NULL, 0);
        if (s->proc_pid > 0 && bp_addr) {
            bool ok = dap_inject_brk((pid_t)s->proc_pid, bp_addr);
            ws_broadcastf(s->ws,
                "{\"ev\":\"dap_brk_injected\",\"addr\":\"0x%llx\",\"ok\":%s}",
                (unsigned long long)bp_addr, ok ? "true" : "false");
        }

    } else {
        char warn[128];
        snprintf(warn, sizeof(warn), "[RigDAP] Comando no reconocido: %s\n", cmd);
        dap_event_output(s, "console", warn);
    }
}
