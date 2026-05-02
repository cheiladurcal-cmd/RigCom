#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — src/pty_term.c
   PTY: pseudo-terminal embebido — stdin/stdout del binario
   compilado conectado al Dashboard vía WebSocket
   Usa posix_openpt / pty / fork + ptrace-ready
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/pty_term.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <pthread.h>

/* posix_openpt disponible en Linux/Android via stdlib.h+fcntl.h */

/* ── Ciclo de vida ──────────────────────────────────────── */
PtySession* pty_session_new(WsServer *srv) {
    PtySession *p = calloc(1, sizeof(PtySession));
    if (!p) return NULL;
    p->master_fd = -1;
    p->child_pid = -1;
    p->state     = PTY_IDLE;
    p->srv       = srv;
    p->echo      = false;
    return p;
}

void pty_session_free(PtySession *p) {
    if (!p) return;
    if (p->master_fd >= 0) { close(p->master_fd); p->master_fd = -1; }
    if (p->child_pid > 0)  { kill(p->child_pid, SIGKILL); p->child_pid = -1; }
    free(p);
}

/* ── Lanzar binario en PTY ─────────────────────────────── */
bool pty_launch(PtySession *p, const char *exec_path,
                char *const argv[], char *const envp[]) {
    if (!p || !exec_path) return false;
    if (p->state == PTY_RUNNING) return false;

    strncpy(p->exec_path, exec_path, sizeof(p->exec_path)-1);

    int master = -1, slave = -1;

#if defined(__linux__) || defined(__ANDROID__)
    /* posix_openpt — portátil en Android/Linux */
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) goto fallback_pipe;
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master); master = -1; goto fallback_pipe;
    }
    char *slave_name = ptsname(master);
    if (!slave_name) { close(master); master = -1; goto fallback_pipe; }
    slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0) { close(master); master = -1; goto fallback_pipe; }

    goto do_fork;

fallback_pipe:
    /* Fallback: pipe simple si no hay PTY */
    {
        int pfd[2];
        if (pipe(pfd) != 0) return false;
        master = pfd[0]; slave = pfd[1];
    }
do_fork:;
/* posix_openpt path covers Android/Linux/macOS via emulation */
#endif

    pid_t pid = fork();
    if (pid < 0) {
        close(master); close(slave); return false;
    }

    if (pid == 0) {
        /* Proceso hijo */
        close(master);
        setsid();
#ifdef TIOCSCTTY
        ioctl(slave, TIOCSCTTY, 0);
#endif
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) close(slave);

        if (envp)
            execve(exec_path, argv, envp);
        else
            execv(exec_path, argv);
        _exit(127);
    }

    /* Proceso padre */
    close(slave);
    p->master_fd = master;
    p->child_pid = pid;
    p->state     = PTY_RUNNING;

    ws_broadcastf(p->srv,
        "{\"ev\":\"pty_launched\","
        "\"exec\":\"%s\","
        "\"pid\":%d}",
        exec_path, (int)pid);

    return true;
}

bool pty_write_input(PtySession *p, const char *data, size_t len) {
    if (!p || p->master_fd < 0 || p->state != PTY_RUNNING)
        return false;
    ssize_t wr = write(p->master_fd, data, len);
    return wr == (ssize_t)len;
}

bool pty_kill(PtySession *p) {
    if (!p || p->child_pid <= 0) return false;
    kill(p->child_pid, SIGTERM);
    usleep(80000);
    kill(p->child_pid, SIGKILL);
    p->state = PTY_EXITED;
    ws_broadcastf(p->srv, "{\"ev\":\"pty_killed\"}");
    return true;
}

void pty_resize(PtySession *p, uint16_t cols, uint16_t rows) {
    if (!p || p->master_fd < 0) return;
#ifdef TIOCSWINSZ
    struct winsize ws = { .ws_row = rows, .ws_col = cols };
    ioctl(p->master_fd, TIOCSWINSZ, &ws);
#endif
    (void)cols; (void)rows;
}

/* ── Emit ───────────────────────────────────────────────── */
void pty_emit_output(PtySession *p, const char *data, size_t len) {
    if (!p || !p->srv || !data || !len) return;
    /* Escapa para JSON — max 4x */
    size_t esz = len * 4 + 64;
    char *esc = malloc(esz);
    if (!esc) return;
    size_t j = 0;
    for (size_t i = 0; i < len && j < esz - 8; i++) {
        unsigned char c = (unsigned char)data[i];
        if      (c == '"')  { esc[j++]='\\'; esc[j++]='"';  }
        else if (c == '\\') { esc[j++]='\\'; esc[j++]='\\'; }
        else if (c == '\n') { esc[j++]='\\'; esc[j++]='n';  }
        else if (c == '\r') { esc[j++]='\\'; esc[j++]='r';  }
        else if (c == '\t') { esc[j++]='\\'; esc[j++]='t';  }
        else if (c < 0x20)  { /* omitir control */ }
        else                { esc[j++] = (char)c; }
    }
    esc[j] = '\0';
    ws_broadcastf(p->srv,
        "{\"ev\":\"pty_output\",\"data\":\"%s\",\"eof\":false}", esc);
    free(esc);
}

void pty_emit_exit(PtySession *p, int code) {
    if (!p || !p->srv) return;
    p->state     = PTY_EXITED;
    p->exit_code = code;
    ws_broadcastf(p->srv,
        "{\"ev\":\"pty_exit\",\"code\":%d,\"exec\":\"%s\"}",
        code, p->exec_path);
}

/* ── Thread lector ─────────────────────────────────────── */
void* pty_reader_thread(void *arg) {
    PtySession *p = (PtySession *)arg;
    char buf[4096];

    while (p->state == PTY_RUNNING && p->master_fd >= 0) {
        ssize_t nr = read(p->master_fd, buf, sizeof(buf));
        if (nr <= 0) {
            /* EOF o error — proceso terminó */
            int status = 0;
            waitpid(p->child_pid, &status, WNOHANG);
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            pty_emit_exit(p, code);
            break;
        }
        pty_emit_output(p, buf, (size_t)nr);
    }
    return NULL;
}

/* ── Thread de lanzamiento completo ────────────────────── */
void* pty_launch_thread(void *arg) {
    PtyLaunchArg *la = (PtyLaunchArg *)arg;
    WsServer *srv    = la->srv;
    char exec_path[512]; memcpy(exec_path, la->exec_path, sizeof(exec_path));
    char args_str[512];  memcpy(args_str,  la->args,      sizeof(args_str));
    free(la);

    /* Verificar que el binario existe */
    if (access(exec_path, X_OK) != 0) {
        ws_broadcastf(srv,
            "{\"ev\":\"pty_error\","
            "\"msg\":\"Binario no encontrado o sin permisos: %s\"}",
            exec_path);
        return NULL;
    }

    PtySession *p = pty_session_new(srv);
    if (!p) return NULL;

    /* Armar argv */
    char *argv_arr[32];
    int argc = 0;
    argv_arr[argc++] = exec_path;
    if (args_str[0]) {
        char *tok = strtok(args_str, " ");
        while (tok && argc < 31) {
            argv_arr[argc++] = tok;
            tok = strtok(NULL, " ");
        }
    }
    argv_arr[argc] = NULL;

    if (!pty_launch(p, exec_path, argv_arr, NULL)) {
        ws_broadcastf(srv,
            "{\"ev\":\"pty_error\","
            "\"msg\":\"No se pudo lanzar el proceso\"}");
        pty_session_free(p);
        return NULL;
    }

    /* Iniciar lector en hilo separado */
    pthread_t rt;
    pthread_create(&rt, NULL, pty_reader_thread, p);
    pthread_detach(rt);

    /* Esperar a que termine para limpiar */
    waitpid(p->child_pid, NULL, 0);
    pty_session_free(p);
    return NULL;
}
