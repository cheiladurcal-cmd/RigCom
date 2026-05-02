/* ============================================================
   RigCom v8.0 — include/wsserver.h
   WebSocket + HTTP server embebido
   RFC 6455 — pure POSIX, sin dependencias externas
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef WSSERVER_H
#define WSSERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#define WS_MAX_CLIENTS    16
#define WS_RECV_BUF       8192
#define WS_FRAME_MAX      131072   /* 128 KB max incoming frame */

/* ═══════════════════════════════════════════════════════════════
   CLIENT STATE — frame parser FSM (RFC 6455 TCP fragmentation safe)
   ═══════════════════════════════════════════════════════════════ */

typedef enum {
    WS_FS_HEADER   = 0,   /* waiting for 2-byte base header   */
    WS_FS_LEN16    = 1,   /* waiting for 2-byte extended len  */
    WS_FS_LEN64    = 2,   /* waiting for 8-byte extended len  */
    WS_FS_MASK     = 3,   /* waiting for 4-byte mask key      */
    WS_FS_PAYLOAD  = 4,   /* accumulating payload bytes       */
} WsFsmState;

typedef struct {
    int          fd;
    bool         handshaked;
    char         recv_buf[WS_RECV_BUF];
    int          recv_pos;

    /* FSM per-frame state */
    WsFsmState   fsm;
    uint8_t      fsm_opcode;
    uint64_t     fsm_pay_len;    /* total expected payload bytes  */
    uint64_t     fsm_pay_got;    /* bytes received so far         */
    bool         fsm_masked;
    uint8_t      fsm_mask[4];
    uint8_t      fsm_hdr[10];    /* raw header accumulator        */
    uint8_t      fsm_hdr_got;    /* bytes of header accumulated   */
    uint8_t      fsm_hdr_need;   /* total header bytes expected   */
    char        *fsm_payload;    /* heap-allocated payload buffer */
} WsClient;

/* ═══════════════════════════════════════════════════════════════
   SERVER
   ═══════════════════════════════════════════════════════════════ */
typedef struct WsServer {
    int          server_fd;
    uint16_t     port;
    bool         running;

    WsClient     clients[WS_MAX_CLIENTS];
    int          n_clients;

    pthread_mutex_t send_lock;    /* protects ws_broadcast from threads */

    /* Callback: fired when a WebSocket text message arrives */
    void (*on_message)(struct WsServer *srv, const char *json, size_t len,
                       void *user);
    void  *user;
} WsServer;

/* ── Lifecycle ──────────────────────────────────────────────── */
/* Initialize server socket on given port. Returns 0 on success. */
int  ws_server_init (WsServer *srv, uint16_t port);

/* Main select() loop — blocks until ws_server_stop() */
void ws_server_run  (WsServer *srv);

/* Signal the loop to stop */
void ws_server_stop (WsServer *srv);

/* ── Broadcast ──────────────────────────────────────────────── */
/* Send UTF-8 text frame to ALL connected WebSocket clients      */
void ws_broadcast  (WsServer *srv, const char *text, size_t len);

/* printf-style broadcast (max 4096 chars)                       */
void ws_broadcastf (WsServer *srv, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* WSSERVER_H */
