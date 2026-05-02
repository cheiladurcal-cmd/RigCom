/* ============================================================
   RigCom v8.0 — src/ptrace_dbg.c
   Debugger de Bajo Nivel Real: ptrace() Backend ARM64
   Lee/escribe registros CPU (x0-x30, v0-v31 NEON), memoria
   arbitraria y variables en caliente.
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/ptrace_dbg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <fcntl.h>

/* ── ptrace y registros ARM64 via sys/ptrace.h ── */
#include <sys/ptrace.h>

/* En Android/ARM64, los registros se leen con PTRACE_GETREGSET,
   usando NT_PRSTATUS para GPR y NT_ARM_VFP / NT_ARM_SVE para NEON. */
#ifndef NT_PRSTATUS
#  define NT_PRSTATUS 1
#endif
#ifndef NT_ARM_VFP
#  define NT_ARM_VFP 0x400
#endif

/* ARM64 GPR layout tal como lo devuelve ptrace */
typedef struct {
    uint64_t regs[31];
    uint64_t sp, pc, pstate;
} arm64_gpr_t;

/* ARM64 NEON/FP layout */
typedef struct {
    __uint128_t vregs[32];
    uint32_t    fpsr, fpcr;
} arm64_fp_t;

/* ── BRK #0 = opcode de software breakpoint ARM64 ── */
#define ARM64_BRK0 0xD4200000U

/* ── Utilidades de formato ── */
static void fmt_uint64_hex(char *out, size_t max, uint64_t v) {
    snprintf(out, max, "0x%016llx", (unsigned long long)v);
}

static void fmt_bytes(char *out, size_t max, const uint8_t *b, size_t n) {
    size_t w = 0;
    for (size_t i = 0; i < n && w + 4 < max; i++) {
        if (i) { out[w++] = ' '; }
        snprintf(out + w, max - w, "%02x", b[i]);
        w += 2;
    }
}

/* ── Ciclo de vida ── */
PtraceSession* ptrace_session_new(RigCtx *ctx, WsServer *ws) {
    PtraceSession *s = calloc(1, sizeof(PtraceSession));
    if (!s) return NULL;
    s->ctx   = ctx;
    s->ws    = ws;
    s->state = DBG_STATE_DETACHED;
    return s;
}

static void free_stack(StackFrame *f) {
    while (f) { StackFrame *n = f->next; free(f); f = n; }
}
static void free_locals(DebugVar *v) {
    while (v) { DebugVar *n = v->next; free(v); v = n; }
}
static void free_bps(PtraceBreakpoint *b) {
    while (b) { PtraceBreakpoint *n = b->next; free(b); b = n; }
}
static void free_wps(PtraceWatchpoint *w) {
    while (w) { PtraceWatchpoint *n = w->next; free(w); w = n; }
}

void ptrace_session_free(PtraceSession *s) {
    if (!s) return;
    if (s->pid > 0 && s->state != DBG_STATE_DETACHED)
        ptrace_detach(s);
    free_bps(s->breakpoints);
    free_wps(s->watchpoints);
    free_stack(s->call_stack);
    free_locals(s->locals);
    free(s);
}

/* ── Adjuntar / lanzar ── */
bool ptrace_attach(PtraceSession *s, pid_t pid) {
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0) {
        ws_broadcastf(s->ws,
            "{\"ev\":\"dbg_error\",\"msg\":\"ptrace ATTACH fallido: %s\"}",
            strerror(errno));
        return false;
    }
    int wst; waitpid(pid, &wst, 0);
    s->pid   = pid;
    s->state = DBG_STATE_STOPPED;

    /* Leer registros iniciales */
    ptrace_regs_get(s, &s->regs);

    ws_broadcastf(s->ws,
        "{\"ev\":\"dbg_attached\",\"pid\":%d,"
        "\"pc\":\"0x%016llx\","
        "\"msg\":\"Debugger adjunto — proceso pausado\"}",
        pid, (unsigned long long)s->regs.pc);
    return true;
}

bool ptrace_launch(PtraceSession *s, const char *exe, char *const argv[]) {
    pid_t child = fork();
    if (child < 0) return false;

    if (child == 0) {
        /* Proceso hijo: habilitarse para ptrace y exec */
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        execv(exe, argv);
        _exit(127);
    }

    /* Proceso padre */
    int wst; waitpid(child, &wst, 0);
    s->pid   = child;
    s->state = DBG_STATE_STOPPED;
    ptrace_regs_get(s, &s->regs);

    ws_broadcastf(s->ws,
        "{\"ev\":\"dbg_launched\",\"pid\":%d,\"exe\":\"%s\","
        "\"pc\":\"0x%016llx\","
        "\"msg\":\"Proceso lanzado — esperando en entry point\"}",
        child, exe, (unsigned long long)s->regs.pc);

    /* Reinstalar breakpoints si los había */
    for (PtraceBreakpoint *b = s->breakpoints; b; b = b->next)
        if (b->active)
            ptrace_bp_set(s, b->addr, b->sym_name,
                          b->source_file, b->source_line);
    return true;
}

void ptrace_detach(PtraceSession *s) {
    if (s->pid <= 0) return;
    /* Restaurar instrucciones originales en breakpoints */
    for (PtraceBreakpoint *b = s->breakpoints; b; b = b->next) {
        if (b->active) {
            uint32_t orig = b->orig_instr;
            struct iovec iov = { &orig, sizeof(orig) };
            ptrace(PTRACE_POKEDATA, s->pid,
                   (void*)(uintptr_t)b->addr, (void*)(uintptr_t)orig);
            (void)iov;
        }
    }
    ptrace(PTRACE_DETACH, s->pid, NULL, NULL);
    s->pid   = 0;
    s->state = DBG_STATE_DETACHED;
    ws_broadcastf(s->ws, "{\"ev\":\"dbg_detached\","
                         "\"msg\":\"Proceso liberado — corriendo libremente\"}");
}

/* ── Control de ejecución ── */
bool ptrace_continue(PtraceSession *s) {
    if (s->state != DBG_STATE_STOPPED) return false;
    ptrace(PTRACE_CONT, s->pid, NULL, NULL);
    s->state = DBG_STATE_RUNNING;
    ws_broadcastf(s->ws, "{\"ev\":\"dbg_running\","
                         "\"msg\":\"Proceso reanudado\"}");
    return true;
}

bool ptrace_step(PtraceSession *s) {
    if (s->state != DBG_STATE_STOPPED) return false;
    ptrace(PTRACE_SINGLESTEP, s->pid, NULL, NULL);
    int wst; waitpid(s->pid, &wst, 0);
    s->state = DBG_STATE_STOPPED;
    ptrace_regs_get(s, &s->regs);
    ptrace_emit_stopped(s, "step", s->regs.pc);
    return true;
}

bool ptrace_step_over(PtraceSession *s) {
    /* Si la instrucción en PC es BL/BLR, poner bp en PC+4 y continuar */
    uint32_t instr = 0;
    struct iovec iov = { &instr, sizeof(instr) };
    ptrace(PTRACE_GETREGSET, s->pid, (void*)(uintptr_t)NT_PRSTATUS, &iov);
    (void)iov;

    bool is_call = (instr & 0xFC000000U) == 0x94000000U  /* BL  */
                || (instr & 0xFFFFFC1FU) == 0xD63F0000U; /* BLR */
    if (is_call) {
        uint64_t next = s->regs.pc + 4;
        uint32_t orig;
        ptrace_mem_read(s, next, &orig, 4);
        uint32_t brk = ARM64_BRK0;
        ptrace_mem_write(s, next, &brk, 4);
        ptrace(PTRACE_CONT, s->pid, NULL, NULL);
        int wst; waitpid(s->pid, &wst, 0);
        ptrace_mem_write(s, next, &orig, 4);
        s->state = DBG_STATE_STOPPED;
        ptrace_regs_get(s, &s->regs);
        ptrace_emit_stopped(s, "step_over", s->regs.pc);
    } else {
        ptrace_step(s);
    }
    return true;
}

bool ptrace_step_out(PtraceSession *s) {
    /* Leer link register (x30) = dirección de retorno */
    uint64_t ret_addr = s->regs.x[30];
    if (ret_addr == 0) return ptrace_step(s);

    /* Poner breakpoint temporal en ret_addr y continuar */
    uint32_t orig;
    ptrace_mem_read(s, ret_addr, &orig, 4);
    uint32_t brk = ARM64_BRK0;
    ptrace_mem_write(s, ret_addr, &brk, 4);
    ptrace(PTRACE_CONT, s->pid, NULL, NULL);
    int wst; waitpid(s->pid, &wst, 0);
    ptrace_mem_write(s, ret_addr, &orig, 4);
    s->state = DBG_STATE_STOPPED;
    ptrace_regs_get(s, &s->regs);
    ptrace_emit_stopped(s, "step_out", s->regs.pc);
    return true;
}

/* ── Breakpoints ── */
bool ptrace_bp_set(PtraceSession *s, uint64_t addr,
                   const char *sym, const char *file, uint32_t line) {
    /* Verificar si ya existe */
    for (PtraceBreakpoint *b = s->breakpoints; b; b = b->next)
        if (b->addr == addr) { b->active = true; return true; }

    /* Leer instrucción original */
    uint32_t orig = 0;
    if (!ptrace_mem_read(s, addr, &orig, 4)) return false;

    /* Escribir BRK #0 */
    uint32_t brk = ARM64_BRK0;
    if (!ptrace_mem_write(s, addr, &brk, 4)) return false;

    PtraceBreakpoint *bp = calloc(1, sizeof(PtraceBreakpoint));
    if (!bp) return false;
    bp->addr       = addr;
    bp->orig_instr = orig;
    bp->active     = true;
    if (sym)  strncpy(bp->sym_name,    sym,  sizeof(bp->sym_name)-1);
    if (file) strncpy(bp->source_file, file, sizeof(bp->source_file)-1);
    bp->source_line = line;
    bp->next        = s->breakpoints;
    s->breakpoints  = bp;
    s->n_bps++;

    ws_broadcastf(s->ws,
        "{\"ev\":\"dbg_bp_set\","
        "\"addr\":\"0x%016llx\","
        "\"sym\":\"%s\","
        "\"file\":\"%s\","
        "\"line\":%u}",
        (unsigned long long)addr,
        sym  ? sym  : "",
        file ? file : "",
        line);
    return true;
}

bool ptrace_bp_del(PtraceSession *s, uint64_t addr) {
    PtraceBreakpoint **prev = &s->breakpoints;
    for (PtraceBreakpoint *b = s->breakpoints; b; b = b->next) {
        if (b->addr == addr) {
            /* Restaurar instrucción original */
            ptrace_mem_write(s, addr, &b->orig_instr, 4);
            *prev = b->next;
            free(b);
            s->n_bps--;
            ws_broadcastf(s->ws,
                "{\"ev\":\"dbg_bp_del\","
                "\"addr\":\"0x%016llx\"}", (unsigned long long)addr);
            return true;
        }
        prev = &b->next;
    }
    return false;
}

bool ptrace_bp_enable(PtraceSession *s, uint64_t addr) {
    for (PtraceBreakpoint *b = s->breakpoints; b; b = b->next) {
        if (b->addr == addr && !b->active) {
            uint32_t brk = ARM64_BRK0;
            ptrace_mem_write(s, addr, &brk, 4);
            b->active = true;
            return true;
        }
    }
    return false;
}

bool ptrace_bp_disable(PtraceSession *s, uint64_t addr) {
    for (PtraceBreakpoint *b = s->breakpoints; b; b = b->next) {
        if (b->addr == addr && b->active) {
            ptrace_mem_write(s, addr, &b->orig_instr, 4);
            b->active = false;
            return true;
        }
    }
    return false;
}

bool ptrace_bp_sym(PtraceSession *s, const char *sym_name) {
    uint64_t addr = ptrace_sym_to_addr(s, sym_name);
    if (addr == 0) {
        ws_broadcastf(s->ws,
            "{\"ev\":\"dbg_error\","
            "\"msg\":\"Símbolo no encontrado: %s\"}", sym_name);
        return false;
    }
    return ptrace_bp_set(s, addr, sym_name, "", 0);
}

/* ── Watchpoints ARM64 (debug registers via ptrace POKEUSER) ── */
bool ptrace_wp_set(PtraceSession *s, uint64_t addr,
                   size_t size, bool on_r, bool on_w) {
    /* ARM64 tiene 4 debug watch registers (DBGWVR0–3 + DBGWCR0–3).
       Los configuramos vía PTRACE_POKEUSER + offset AArch64 */
    PtraceWatchpoint *wp = calloc(1, sizeof(PtraceWatchpoint));
    if (!wp) return false;
    wp->addr     = addr;
    wp->size     = size;
    wp->on_read  = on_r;
    wp->on_write = on_w;
    wp->next     = s->watchpoints;
    s->watchpoints = wp;
    s->n_wps++;

    /* Para Android, usamos PTRACE_POKEUSER con el offset del DBGWVR:
       offset base = 768 (0x300) para DBGWVR0_EL1, cada uno 8 bytes */
    uint32_t wp_idx = s->n_wps - 1;
    if (wp_idx < 4) {
        unsigned long wvr_off = 768 + wp_idx * 8;
        unsigned long wcr_off = 832 + wp_idx * 8;
        /* BAS: byte address select según size */
        uint32_t bas = (size == 1) ? 0x1 : (size == 2) ? 0x3
                     : (size == 4) ? 0xF : 0xFF;
        /* DBGWCR: E=1, PAC=0b11(EL0+EL1), LSC=read/write, BAS */
        uint32_t wcr = 1U                        /* E: enable      */
                     | (0x3U << 1)               /* PAC: EL0+EL1   */
                     | ((on_r ? 1U : 0U) << 3)   /* LSC bit0: read */
                     | ((on_w ? 2U : 0U) << 3)   /* LSC bit1: write*/
                     | (bas << 5);               /* BAS            */
        ptrace(PTRACE_POKEUSER, s->pid, (void*)wvr_off, (void*)(uintptr_t)addr);
        ptrace(PTRACE_POKEUSER, s->pid, (void*)wcr_off, (void*)(uintptr_t)wcr);
    }

    ws_broadcastf(s->ws,
        "{\"ev\":\"dbg_wp_set\","
        "\"addr\":\"0x%016llx\","
        "\"size\":%zu,"
        "\"read\":%s,\"write\":%s}",
        (unsigned long long)addr, size,
        on_r ? "true" : "false",
        on_w ? "true" : "false");
    return true;
}

bool ptrace_wp_del(PtraceSession *s, uint64_t addr) {
    PtraceWatchpoint **prev = &s->watchpoints;
    uint32_t idx = 0;
    for (PtraceWatchpoint *w = s->watchpoints; w; w = w->next, idx++) {
        if (w->addr == addr) {
            /* Deshabilitar el debug register */
            if (idx < 4) {
                unsigned long wcr_off = 832 + idx * 8;
                ptrace(PTRACE_POKEUSER, s->pid, (void*)wcr_off, (void*)0UL);
            }
            *prev = w->next; free(w); s->n_wps--;
            return true;
        }
        prev = &w->next;
    }
    return false;
}

/* ── Lectura/escritura de memoria vía /proc/PID/mem ── */
bool ptrace_mem_read(PtraceSession *s, uint64_t addr,
                     void *buf, size_t len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", (int)s->pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        /* Fallback: PTRACE_PEEKDATA (lee 8 bytes a la vez) */
        uint8_t *out = (uint8_t*)buf;
        for (size_t i = 0; i < len; i += 8) {
            errno = 0;
            long w = ptrace(PTRACE_PEEKDATA, s->pid,
                            (void*)(uintptr_t)(addr + i), NULL);
            if (errno != 0) return false;
            size_t cp = (i + 8 <= len) ? 8 : (len - i);
            memcpy(out + i, &w, cp);
        }
        return true;
    }
    ssize_t rd = pread(fd, buf, len, (off_t)addr);
    close(fd);
    return (size_t)rd == len;
}

bool ptrace_mem_write(PtraceSession *s, uint64_t addr,
                      const void *buf, size_t len) {
    /* /proc/PID/mem en Android permite escritura si somos owner */
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", (int)s->pid);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        ssize_t wr = pwrite(fd, buf, len, (off_t)addr);
        close(fd);
        if ((size_t)wr == len) return true;
    }
    /* Fallback: PTRACE_POKEDATA */
    const uint8_t *in = (const uint8_t*)buf;
    for (size_t i = 0; i < len; i += 8) {
        long word = 0;
        size_t cp = (i + 8 <= len) ? 8 : (len - i);
        if (cp < 8) ptrace(PTRACE_PEEKDATA, s->pid,
                           (void*)(uintptr_t)(addr+i), &word);
        memcpy(&word, in + i, cp);
        if (ptrace(PTRACE_POKEDATA, s->pid,
                   (void*)(uintptr_t)(addr+i), (void*)word) != 0)
            return false;
    }
    return true;
}

bool ptrace_mem_read_str(PtraceSession *s, uint64_t addr,
                          char *out, size_t max) {
    for (size_t i = 0; i < max - 1; i++) {
        uint8_t b = 0;
        if (!ptrace_mem_read(s, addr + i, &b, 1)) return false;
        out[i] = (char)b;
        if (b == 0) return true;
    }
    out[max-1] = '\0';
    return true;
}

/* ── Registros ARM64 ── */
bool ptrace_regs_get(PtraceSession *s, Arm64Regs *out) {
    /* GPR via PTRACE_GETREGSET NT_PRSTATUS */
    arm64_gpr_t gpr = {0};
    struct iovec iov = { &gpr, sizeof(gpr) };
    if (ptrace(PTRACE_GETREGSET, s->pid,
               (void*)(uintptr_t)NT_PRSTATUS, &iov) != 0)
        return false;

    memcpy(out->x, gpr.regs, sizeof(out->x));
    out->sp     = gpr.sp;
    out->pc     = gpr.pc;
    out->pstate = gpr.pstate;

    /* NEON/FP via PTRACE_GETREGSET NT_ARM_VFP */
    arm64_fp_t fp = {0};
    iov.iov_base = &fp;
    iov.iov_len  = sizeof(fp);
    if (ptrace(PTRACE_GETREGSET, s->pid,
               (void*)(uintptr_t)NT_ARM_VFP, &iov) == 0) {
        for (int i = 0; i < 32; i++) {
            memcpy(out->v[i], &fp.vregs[i], 16);
        }
    }
    return true;
}

bool ptrace_regs_set(PtraceSession *s, const Arm64Regs *regs) {
    arm64_gpr_t gpr = {0};
    memcpy(gpr.regs, regs->x, sizeof(gpr.regs));
    gpr.sp     = regs->sp;
    gpr.pc     = regs->pc;
    gpr.pstate = regs->pstate;
    struct iovec iov = { &gpr, sizeof(gpr) };
    if (ptrace(PTRACE_SETREGSET, s->pid,
               (void*)(uintptr_t)NT_PRSTATUS, &iov) != 0)
        return false;

    arm64_fp_t fp = {0};
    for (int i = 0; i < 32; i++)
        memcpy(&fp.vregs[i], regs->v[i], 16);
    iov.iov_base = &fp;
    iov.iov_len  = sizeof(fp);
    ptrace(PTRACE_SETREGSET, s->pid,
           (void*)(uintptr_t)NT_ARM_VFP, &iov);
    return true;
}

bool ptrace_reg_get_by_name(PtraceSession *s, const char *reg,
                             uint64_t *lo, uint64_t *hi) {
    ptrace_regs_get(s, &s->regs);
    if (lo) *lo = 0;
    if (hi) *hi = 0;

    if (!strncmp(reg, "x", 1) || !strncmp(reg, "X", 1)) {
        int n = atoi(reg + 1);
        if (n >= 0 && n < 31 && lo) { *lo = s->regs.x[n]; return true; }
    }
    if (!strcasecmp(reg, "sp")) { if (lo) *lo = s->regs.sp; return true; }
    if (!strcasecmp(reg, "pc")) { if (lo) *lo = s->regs.pc; return true; }
    if (!strcasecmp(reg, "lr") || !strcasecmp(reg, "x30")) {
        if (lo) *lo = s->regs.x[30]; return true;
    }
    if (!strncmp(reg, "v", 1) || !strncmp(reg, "V", 1)) {
        int n = atoi(reg + 1);
        if (n >= 0 && n < 32) {
            if (lo) *lo = s->regs.v[n][0];
            if (hi) *hi = s->regs.v[n][1];
            return true;
        }
    }
    return false;
}

bool ptrace_reg_set_by_name(PtraceSession *s, const char *reg,
                              uint64_t val_lo, uint64_t val_hi) {
    ptrace_regs_get(s, &s->regs);
    if (!strncmp(reg, "x", 1) || !strncmp(reg, "X", 1)) {
        int n = atoi(reg + 1);
        if (n >= 0 && n < 31) { s->regs.x[n] = val_lo; }
    } else if (!strcasecmp(reg, "sp")) {
        s->regs.sp = val_lo;
    } else if (!strcasecmp(reg, "pc")) {
        s->regs.pc = val_lo;
    } else if (!strncmp(reg, "v", 1) || !strncmp(reg, "V", 1)) {
        int n = atoi(reg + 1);
        if (n >= 0 && n < 32) {
            s->regs.v[n][0] = val_lo;
            s->regs.v[n][1] = val_hi;
        }
    } else { return false; }

    return ptrace_regs_set(s, &s->regs);
}

/* ── Stack unwinding via frame pointer (FP/LR convención ARM64) ── */
StackFrame* ptrace_unwind_stack(PtraceSession *s, uint32_t max_depth) {
    ptrace_regs_get(s, &s->regs);
    StackFrame *head = NULL, **tail = &head;
    uint64_t fp = s->regs.x[29]; /* x29 = frame pointer */
    uint64_t lr = s->regs.x[30]; /* x30 = link register */
    uint64_t pc = s->regs.pc;
    uint32_t depth = 0;

    while (depth < max_depth && pc != 0) {
        StackFrame *f = calloc(1, sizeof(StackFrame));
        if (!f) break;
        f->pc = pc;
        f->sp = fp;

        /* Resolver símbolo desde /proc/PID/maps + nm (simplificado) */
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
            "addr2line -e /proc/%d/exe -f -s 0x%llx 2>/dev/null",
            (int)s->pid, (unsigned long long)pc);
        FILE *p = popen(cmd, "r");
        if (p) {
            char fn[128]={0}, loc[256]={0};
            if (fgets(fn, sizeof(fn), p)) {
                size_t l = strlen(fn);
                if (l > 0 && fn[l-1]=='\n') fn[l-1]='\0';
                strncpy(f->fn_name, fn, sizeof(f->fn_name)-1);
            }
            if (fgets(loc, sizeof(loc), p)) {
                char *col = strrchr(loc, ':');
                if (col) {
                    f->line = (uint32_t)strtoul(col+1, NULL, 10);
                    *col = '\0';
                    strncpy(f->file, loc, sizeof(f->file)-1);
                }
            }
            pclose(p);
        }
        if (!f->fn_name[0])
            snprintf(f->fn_name, sizeof(f->fn_name),
                     "0x%016llx", (unsigned long long)pc);

        *tail = f; tail = &f->next;
        depth++;

        /* Siguiente frame: leer [fp] = saved_fp, [fp+8] = saved_lr */
        if (fp == 0) break;
        uint64_t saved_fp = 0, saved_lr = 0;
        if (!ptrace_mem_read(s, fp,   &saved_fp, 8)) break;
        if (!ptrace_mem_read(s, fp+8, &saved_lr, 8)) break;
        pc = saved_lr;
        fp = saved_fp;
        (void)lr;
    }
    return head;
}

/* ── Resolución de símbolos ── */
uint64_t ptrace_sym_to_addr(PtraceSession *s, const char *sym) {
    /* Buscar en /proc/PID/maps la base del ejecutable principal,
       luego usar nm -D para encontrar el offset del símbolo. */
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", (int)s->pid);
    FILE *mf = fopen(maps_path, "r");
    if (!mf) return 0;

    char exe_path[512] = {0};
    uint64_t exe_base = 0;
    char line[512];
    while (fgets(line, sizeof(line), mf)) {
        /* formato: addr-addr rwxp offset dev inode path */
        if (strstr(line, " r-xp ") && !exe_path[0]) {
            /* Primera región ejecutable = base del binario */
            sscanf(line, "%llx-", (unsigned long long*)&exe_base);
            char *tab = strchr(line, '/');
            if (!tab) tab = strstr(line, "/data");
            if (tab) {
                strncpy(exe_path, tab, sizeof(exe_path)-1);
                size_t ll = strlen(exe_path);
                while (ll > 0 && (exe_path[ll-1]=='\n'||exe_path[ll-1]=='\r'))
                    exe_path[--ll] = '\0';
            }
        }
    }
    fclose(mf);
    if (!exe_path[0]) return 0;

    /* nm -D para símbolo */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "nm -D --defined-only '%s' 2>/dev/null | grep ' %s$'",
        exe_path, sym);
    FILE *np = popen(cmd, "r");
    if (!np) return 0;
    uint64_t offset = 0;
    if (fgets(line, sizeof(line), np))
        sscanf(line, "%llx", (unsigned long long*)&offset);
    pclose(np);
    if (!offset) return 0;

    /* Para PIE: dirección real = base + offset */
    return exe_base + offset;
}

/* ── Lectura de variables locales (simplificada via DWARF/readelf) ── */
DebugVar* ptrace_read_locals(PtraceSession *s) {
    /* Obtener el pc actual para buscar las variables del frame */
    uint64_t pc = s->regs.pc;
    /* Usar addr2line + DWARF para obtener variables locales */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "readelf --debug-dump=info /proc/%d/exe 2>/dev/null | "
        "grep -A2 'DW_AT_name\\|DW_AT_location' | head -200",
        (int)s->pid);
    /* La implementación completa de DWARF parsing requiere libdw.
       Aquí emitimos las variables que podemos inferir de los registros. */
    DebugVar *head = NULL, **tail = &head;

    /* Registros de argumento típicos ARM64: x0–x7 son los primeros 8 parámetros */
    static const char *arg_regs[] = {"x0","x1","x2","x3","x4","x5","x6","x7"};
    for (int i = 0; i < 8; i++) {
        DebugVar *v = calloc(1, sizeof(DebugVar));
        if (!v) break;
        snprintf(v->name, sizeof(v->name), "arg%d (%s)", i, arg_regs[i]);
        v->addr = 0; /* registro, no memoria */
        uint64_t val = (i < 31) ? s->regs.x[i] : 0;
        memcpy(v->raw, &val, 8);
        v->size = 8;
        snprintf(v->value_str, sizeof(v->value_str),
                 "0x%016llx (%llu)", (unsigned long long)val,
                 (unsigned long long)val);
        strncpy(v->type_str, "uint64_t", sizeof(v->type_str)-1);
        *tail = v; tail = &v->next;
    }
    (void)pc; (void)cmd;
    return head;
}

/* ── Emisión de eventos WebSocket ── */
void ptrace_emit_stopped(PtraceSession *s, const char *reason,
                          uint64_t addr) {
    ptrace_regs_get(s, &s->regs);
    char sym[128] = {0};

    /* Buscar si la dirección coincide con algún breakpoint */
    for (PtraceBreakpoint *b = s->breakpoints; b; b = b->next) {
        if (b->addr == addr || b->addr == addr - 4) {
            strncpy(sym, b->sym_name, sizeof(sym)-1);
            break;
        }
    }

    ws_broadcastf(s->ws,
        "{\"ev\":\"dbg_stopped\","
        "\"reason\":\"%s\","
        "\"addr\":\"0x%016llx\","
        "\"sym\":\"%s\","
        "\"pc\":\"0x%016llx\","
        "\"sp\":\"0x%016llx\","
        "\"lr\":\"0x%016llx\"}",
        reason,
        (unsigned long long)addr,
        sym,
        (unsigned long long)s->regs.pc,
        (unsigned long long)s->regs.sp,
        (unsigned long long)s->regs.x[30]);

    /* Emitir registros, stack y locales automáticamente */
    ptrace_emit_regs(s);
    free_stack(s->call_stack);
    s->call_stack = ptrace_unwind_stack(s, 16);
    ptrace_emit_stack(s);
    free_locals(s->locals);
    s->locals = ptrace_read_locals(s);
    ptrace_emit_locals(s);
}

void ptrace_emit_regs(PtraceSession *s) {
    /* Emitir GPR */
    ws_broadcastf(s->ws,
        "{\"ev\":\"dbg_regs\","
        "\"pc\":\"0x%016llx\","
        "\"sp\":\"0x%016llx\","
        "\"x0\":\"0x%016llx\","
        "\"x1\":\"0x%016llx\","
        "\"x2\":\"0x%016llx\","
        "\"x3\":\"0x%016llx\","
        "\"x29\":\"0x%016llx\","
        "\"x30\":\"0x%016llx\","
        "\"pstate\":\"0x%016llx\"}",
        (unsigned long long)s->regs.pc,
        (unsigned long long)s->regs.sp,
        (unsigned long long)s->regs.x[0],
        (unsigned long long)s->regs.x[1],
        (unsigned long long)s->regs.x[2],
        (unsigned long long)s->regs.x[3],
        (unsigned long long)s->regs.x[29],
        (unsigned long long)s->regs.x[30],
        (unsigned long long)s->regs.pstate);

    /* Emitir registros NEON v0–v7 (los más usados) */
    for (int i = 0; i < 8; i++) {
        ws_broadcastf(s->ws,
            "{\"ev\":\"dbg_vreg\","
            "\"r\":\"v%d\","
            "\"lo\":\"0x%016llx\","
            "\"hi\":\"0x%016llx\"}",
            i,
            (unsigned long long)s->regs.v[i][0],
            (unsigned long long)s->regs.v[i][1]);
    }
}

void ptrace_emit_stack(PtraceSession *s) {
    ws_broadcastf(s->ws, "{\"ev\":\"dbg_stack_start\"}");
    uint32_t depth = 0;
    for (StackFrame *f = s->call_stack; f; f = f->next, depth++) {
        ws_broadcastf(s->ws,
            "{\"ev\":\"dbg_frame\","
            "\"depth\":%u,"
            "\"pc\":\"0x%016llx\","
            "\"fn\":\"%s\","
            "\"file\":\"%s\","
            "\"line\":%u}",
            depth,
            (unsigned long long)f->pc,
            f->fn_name,
            f->file,
            f->line);
    }
    ws_broadcastf(s->ws, "{\"ev\":\"dbg_stack_end\",\"depth\":%u}", depth);
}

void ptrace_emit_locals(PtraceSession *s) {
    ws_broadcastf(s->ws, "{\"ev\":\"dbg_locals_start\"}");
    for (DebugVar *v = s->locals; v; v = v->next) {
        ws_broadcastf(s->ws,
            "{\"ev\":\"dbg_local\","
            "\"name\":\"%s\","
            "\"type\":\"%s\","
            "\"value\":\"%s\","
            "\"addr\":\"0x%016llx\"}",
            v->name, v->type_str, v->value_str,
            (unsigned long long)v->addr);
    }
    ws_broadcastf(s->ws, "{\"ev\":\"dbg_locals_end\"}");
}

void ptrace_emit_memory(PtraceSession *s, uint64_t addr,
                         const uint8_t *data, size_t len) {
    char hex[512] = {0};
    fmt_bytes(hex, sizeof(hex), data, len < 64 ? len : 64);
    ws_broadcastf(s->ws,
        "{\"ev\":\"dbg_memory\","
        "\"addr\":\"0x%016llx\","
        "\"len\":%zu,"
        "\"hex\":\"%s\"}",
        (unsigned long long)addr, len, hex);
}

/* ── Loop principal del debugger ── */
void* ptrace_dbg_thread(void *arg) {
    PtraceThreadArg *a = (PtraceThreadArg*)arg;
    PtraceSession   *s = a->session;

    bool ok;
    if (a->attach_pid > 0) {
        ok = ptrace_attach(s, a->attach_pid);
    } else if (a->exe[0]) {
        char *argv[] = { a->exe, NULL };
        ok = ptrace_launch(s, a->exe, argv);
    } else {
        ws_broadcastf(s->ws,
            "{\"ev\":\"dbg_error\","
            "\"msg\":\"ptrace_dbg_thread: pid o exe requerido\"}");
        free(a); return NULL;
    }
    if (!ok) { free(a); return NULL; }

    /* Esperar eventos del proceso: SIGTRAP = breakpoint/step,
       SIGILL = BRK #0, SIGSEGV = crash, SIGTERM = fin */
    while (s->state != DBG_STATE_EXITED) {
        int wst = 0;
        pid_t wp = waitpid(s->pid, &wst, WNOHANG);
        if (wp == 0) { usleep(5000); continue; } /* nada todavía */
        if (wp < 0)  { break; }

        if (WIFEXITED(wst)) {
            s->state     = DBG_STATE_EXITED;
            s->exit_code = WEXITSTATUS(wst);
            ws_broadcastf(s->ws,
                "{\"ev\":\"dbg_exited\","
                "\"code\":%d,"
                "\"msg\":\"Proceso terminado\"}",
                s->exit_code);
            break;
        }

        if (WIFSTOPPED(wst)) {
            int sig = WSTOPSIG(wst);
            s->state = DBG_STATE_STOPPED;
            ptrace_regs_get(s, &s->regs);

            const char *reason =
                (sig == SIGTRAP) ? "breakpoint" :
                (sig == SIGILL)  ? "brk_instr"  :
                (sig == SIGSEGV) ? "segfault"   :
                (sig == SIGBUS)  ? "bus_error"  : "signal";

            ptrace_emit_stopped(s, reason, s->regs.pc);

            /* Si es breakpoint de software: retroceder PC un step
               y restaurar instrucción original */
            if (sig == SIGTRAP || sig == SIGILL) {
                uint64_t bp_addr = s->regs.pc - 4;
                for (PtraceBreakpoint *b = s->breakpoints; b; b = b->next) {
                    if (b->addr == bp_addr && b->active) {
                        /* Restaurar instrucción original */
                        ptrace_mem_write(s, bp_addr, &b->orig_instr, 4);
                        /* Retroceder PC */
                        s->regs.pc = bp_addr;
                        ptrace_regs_set(s, &s->regs);
                        break;
                    }
                }
            }
            /* No reanudar automáticamente — esperar comando del usuario */
        }
    }
    free(a);
    return NULL;
}

/* ── Parsing de comandos JSON del dashboard ── */
/* Extrae un string de un JSON mínimo: {"key":"value"} */
static void dbg_json_str(const char *json, const char *key,
                          char *out, size_t max) {
    char pat[128]; snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) { out[0]='\0'; return; }
    p += strlen(pat);
    while (*p==' '||*p=='\t') p++;
    if (*p=='"') { p++;
        size_t i=0;
        while (*p && *p!='"' && i<max-1) out[i++]=*p++;
        out[i]='\0';
    } else { out[0]='\0'; }
}
static uint64_t dbg_json_hex(const char *json, const char *key) {
    char v[32]={0}; dbg_json_str(json, key, v, sizeof(v));
    if (!v[0]) return 0;
    return (uint64_t)strtoull(v, NULL, 16);
}
static long dbg_json_int(const char *json, const char *key) {
    char v[32]={0}; dbg_json_str(json, key, v, sizeof(v));
    return v[0] ? strtol(v, NULL, 10) : 0;
}

void ptrace_handle_cmd(PtraceSession *s, const char *json) {
    char cmd[64]={0}; dbg_json_str(json, "cmd", cmd, sizeof(cmd));
    if (!cmd[0]) dbg_json_str(json, "action", cmd, sizeof(cmd));

    if (!strcmp(cmd, "attach")) {
        long pid = dbg_json_int(json, "pid");
        if (pid > 0) ptrace_attach(s, (pid_t)pid);

    } else if (!strcmp(cmd, "launch")) {
        char exe[512]={0}; dbg_json_str(json, "exe", exe, sizeof(exe));
        if (exe[0]) {
            char *argv[] = {exe, NULL};
            ptrace_launch(s, exe, argv);
        }

    } else if (!strcmp(cmd, "continue") || !strcmp(cmd, "run")) {
        ptrace_continue(s);

    } else if (!strcmp(cmd, "step")) {
        ptrace_step(s);

    } else if (!strcmp(cmd, "step_over") || !strcmp(cmd, "next")) {
        ptrace_step_over(s);

    } else if (!strcmp(cmd, "step_out") || !strcmp(cmd, "finish")) {
        ptrace_step_out(s);

    } else if (!strcmp(cmd, "bp_set")) {
        char sym[128]={0}, file[256]={0};
        uint64_t addr = dbg_json_hex(json, "addr");
        long     line = dbg_json_int(json, "line");
        dbg_json_str(json, "sym",  sym,  sizeof(sym));
        dbg_json_str(json, "file", file, sizeof(file));
        if (sym[0] && !addr) addr = ptrace_sym_to_addr(s, sym);
        if (addr) ptrace_bp_set(s, addr, sym, file, (uint32_t)line);

    } else if (!strcmp(cmd, "bp_del")) {
        uint64_t addr = dbg_json_hex(json, "addr");
        if (addr) ptrace_bp_del(s, addr);

    } else if (!strcmp(cmd, "bp_sym")) {
        char sym[128]={0}; dbg_json_str(json, "sym", sym, sizeof(sym));
        if (sym[0]) ptrace_bp_sym(s, sym);

    } else if (!strcmp(cmd, "wp_set")) {
        uint64_t addr = dbg_json_hex(json, "addr");
        long     size = dbg_json_int(json, "size");
        char     mode[8]={0}; dbg_json_str(json, "mode", mode, sizeof(mode));
        if (addr && size > 0) {
            bool on_r = strchr(mode,'r') != NULL;
            bool on_w = strchr(mode,'w') != NULL || !mode[0];
            ptrace_wp_set(s, addr, (size_t)size, on_r, on_w);
        }

    } else if (!strcmp(cmd, "wp_del")) {
        uint64_t addr = dbg_json_hex(json, "addr");
        if (addr) ptrace_wp_del(s, addr);

    } else if (!strcmp(cmd, "read_mem")) {
        uint64_t addr = dbg_json_hex(json, "addr");
        long     len  = dbg_json_int(json, "len");
        if (!len || len > 4096) len = 64;
        if (addr) {
            uint8_t buf[4096]={0};
            if (ptrace_mem_read(s, addr, buf, (size_t)len))
                ptrace_emit_memory(s, addr, buf, (size_t)len);
        }

    } else if (!strcmp(cmd, "write_mem")) {
        uint64_t addr = dbg_json_hex(json, "addr");
        char     val[64]={0}; dbg_json_str(json, "value", val, sizeof(val));
        if (addr && val[0]) {
            uint64_t v = strtoull(val, NULL, 16);
            ptrace_mem_write(s, addr, &v, 8);
            ws_broadcastf(s->ws,
                "{\"ev\":\"dbg_mem_written\","
                "\"addr\":\"0x%016llx\","
                "\"value\":\"0x%016llx\"}",
                (unsigned long long)addr,
                (unsigned long long)v);
        }

    } else if (!strcmp(cmd, "read_reg")) {
        char reg[16]={0}; dbg_json_str(json, "reg", reg, sizeof(reg));
        if (reg[0]) {
            uint64_t lo=0, hi=0;
            ptrace_reg_get_by_name(s, reg, &lo, &hi);
            ws_broadcastf(s->ws,
                "{\"ev\":\"dbg_reg\","
                "\"reg\":\"%s\","
                "\"lo\":\"0x%016llx\","
                "\"hi\":\"0x%016llx\"}",
                reg, (unsigned long long)lo, (unsigned long long)hi);
        }

    } else if (!strcmp(cmd, "write_reg")) {
        char reg[16]={0}, val[32]={0};
        dbg_json_str(json, "reg",   reg, sizeof(reg));
        dbg_json_str(json, "value", val, sizeof(val));
        if (reg[0] && val[0]) {
            uint64_t v = strtoull(val, NULL, 16);
            ptrace_reg_set_by_name(s, reg, v, 0);
            ws_broadcastf(s->ws,
                "{\"ev\":\"dbg_reg_written\","
                "\"reg\":\"%s\","
                "\"value\":\"0x%016llx\"}",
                reg, (unsigned long long)v);
        }

    } else if (!strcmp(cmd, "regs")) {
        ptrace_regs_get(s, &s->regs);
        ptrace_emit_regs(s);

    } else if (!strcmp(cmd, "stack")) {
        free_stack(s->call_stack);
        s->call_stack = ptrace_unwind_stack(s, 32);
        ptrace_emit_stack(s);

    } else if (!strcmp(cmd, "locals")) {
        free_locals(s->locals);
        s->locals = ptrace_read_locals(s);
        ptrace_emit_locals(s);

    } else if (!strcmp(cmd, "detach")) {
        ptrace_detach(s);

    } else {
        ws_broadcastf(s->ws,
            "{\"ev\":\"dbg_error\","
            "\"msg\":\"Comando desconocido: %s\"}", cmd);
    }
    (void)fmt_uint64_hex;
}
