/* ============================================================
   RigCom v8.0 — include/rigbridge.h
   RigBridge: compilación distribuida P2P vía WiFi
   Descubrimiento UDP · distribución TCP · merge de objetos
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef RIGBRIDGE_H
#define RIGBRIDGE_H

#include "wsserver.h"
#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>

/* ── Protocolo ──────────────────────────────────────────────── */
#define BRIDGE_UDP_PORT    9771
#define BRIDGE_TCP_PORT    9772
#define BRIDGE_MAGIC       0x52474252u  /* "RGBR" little-endian */
#define BRIDGE_MAX_PEERS   16
#define BRIDGE_BEACON_MS   1500         /* intervalo beacon ms  */
#define BRIDGE_TIMEOUT_MS  6000         /* peer timeout ms      */

/* ── Paquete UDP de descubrimiento ──────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t magic;        /* BRIDGE_MAGIC              */
    uint8_t  type;         /* 0=beacon 1=ack            */
    uint16_t tcp_port;     /* puerto TCP del listener   */
    char     version[16];  /* "4.0.0"                   */
    char     hostname[32]; /* nombre del dispositivo    */
} BridgeBeacon;

/* ── Peer descubierto ───────────────────────────────────────── */
typedef struct {
    char     ip[64];
    uint16_t tcp_port;
    char     version[16];
    char     hostname[32];
    bool     alive;
    int64_t  last_seen_ms;
    int      load;         /* 0-100 estimado             */
} BridgePeer;

/* ── Mensaje TCP de trabajo ─────────────────────────────────── */
#define BRIDGE_JOB_COMPILE  1
#define BRIDGE_JOB_RESULT   2
#define BRIDGE_JOB_ERROR    3

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  type;         /* BRIDGE_JOB_* */
    uint32_t job_id;
    uint32_t data_len;
    /* seguido de data_len bytes de payload */
} BridgeMsgHdr;

/* ── Argumentos para threads ────────────────────────────────── */
typedef struct {
    WsServer *srv;
    int       duration_ms; /* cuánto tiempo escanear */
} BridgeScanArg;

typedef struct {
    WsServer *srv;
    char    **src_files;
    int       n_files;
} BridgeBuildArg;

/* ── Estado global de peers ─────────────────────────────────── */
extern BridgePeer g_bridge_peers[BRIDGE_MAX_PEERS];
extern int        g_bridge_n_peers;

/* ── API pública ────────────────────────────────────────────── */

/* Thread: escanea peers via UDP broadcast por duration_ms ms */
void *rigbridge_scan_thread(void *arg);

/* Thread: distribuye compilación entre peers descubiertos */
void *rigbridge_build_thread(void *arg);

/* Emite estado actual de peers → WS bridge_peers event */
void  rigbridge_emit_peers(WsServer *srv);

/* Thread: listener TCP para recibir trabajos de peers */
void *rigbridge_listener_thread(void *arg);

/* Tiempo actual en ms */
int64_t bridge_ms_now(void);

#endif /* RIGBRIDGE_H */
