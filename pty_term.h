/* ============================================================
   RigCom v8.0 — include/pty_term.h
   PTY: pseudo-terminal embebido para la UI
   Conecta stdin/stdout del binario compilado al Dashboard
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef PTY_TERM_H
#define PTY_TERM_H

#include "wsserver.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* ── Estado del PTY ──────────────────────────────────────── */
typedef enum {
    PTY_IDLE      = 0,
    PTY_RUNNING,
    PTY_PAUSED,
    PTY_EXITED,
} PtyState;

typedef struct {
    int       master_fd;    /* fd del master PTY               */
    pid_t     child_pid;    /* PID del proceso hijo            */
    PtyState  state;
    WsServer *srv;          /* para emitir pty_output eventos  */
    char      exec_path[512];
    int       exit_code;
    bool      echo;         /* echo de input al terminal       */
} PtySession;

/* ── API pública ─────────────────────────────────────────── */
PtySession* pty_session_new  (WsServer *srv);
void        pty_session_free (PtySession *p);

/* Lanza el binario en el PTY */
bool pty_launch(PtySession *p, const char *exec_path,
                char *const argv[], char *const envp[]);

/* Escribe input del usuario al proceso */
bool pty_write_input(PtySession *p, const char *data, size_t len);

/* Mata el proceso */
bool pty_kill(PtySession *p);

/* Resize de terminal (cols x rows) */
void pty_resize(PtySession *p, uint16_t cols, uint16_t rows);

/* Thread lector: lee stdout y emite pty_output por WS */
void* pty_reader_thread(void *arg);

/* Emite evento WS: {"ev":"pty_output","data":"...","eof":false} */
void pty_emit_output(PtySession *p, const char *data, size_t len);
void pty_emit_exit  (PtySession *p, int code);

/* Thread arg para lanzar con pthread */
typedef struct {
    WsServer *srv;
    char      exec_path[512];
    char      args[512];     /* espacio-separados */
} PtyLaunchArg;

void* pty_launch_thread(void *arg);

#endif /* PTY_TERM_H */
