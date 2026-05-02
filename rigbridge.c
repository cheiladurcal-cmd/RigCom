#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — src/rigbridge.c
   RigBridge: malla P2P para compilación distribuida
   Protocolo: UDP beacon discovery + TCP job distribution
   ============================================================ */
#include "../include/rigbridge.h"
#include "../include/wsserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <sys/stat.h>
#include <fcntl.h>

/* ── Estado global ──────────────────────────────────────────── */
BridgePeer g_bridge_peers[BRIDGE_MAX_PEERS];
int        g_bridge_n_peers = 0;
static pthread_mutex_t g_peers_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── Utilidades ─────────────────────────────────────────────── */
int64_t bridge_ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void get_hostname(char *out, size_t sz) {
    if (gethostname(out, sz) != 0)
        strncpy(out, "rigcom-device", sz);
    out[sz-1] = '\0';
}

/* Obtiene IP de la interfaz WiFi activa */
static bool get_local_ip(char *out, size_t sz) {
    struct ifaddrs *ifa, *iff;
    if (getifaddrs(&ifa) != 0) return false;

    bool found = false;
    for (iff = ifa; iff; iff = iff->ifa_next) {
        if (!iff->ifa_addr) continue;
        if (iff->ifa_addr->sa_family != AF_INET) continue;
        /* Prefiere wlan0, wlan1, eth0, en0, enp* */
        const char *n = iff->ifa_name;
        if (strncmp(n, "lo", 2) == 0) continue;
        struct sockaddr_in *sa = (struct sockaddr_in *)iff->ifa_addr;
        inet_ntop(AF_INET, &sa->sin_addr, out, (socklen_t)sz);
        found = true;
        /* Prefiere interfaces wireless */
        if (strncmp(n, "wlan", 4) == 0 || strncmp(n, "wl", 2) == 0)
            break;
    }
    freeifaddrs(ifa);
    return found;
}

/* Calcula broadcast addr de 192.168.x.y → 192.168.x.255 */
static void get_broadcast(const char *local_ip, char *bcast, size_t sz) {
    /* Obtiene broadcast real via socket */
    struct ifaddrs *ifa, *iff;
    if (getifaddrs(&ifa) == 0) {
        for (iff = ifa; iff; iff = iff->ifa_next) {
            if (!iff->ifa_addr || !iff->ifa_broadaddr) continue;
            if (iff->ifa_addr->sa_family != AF_INET) continue;
            if (strncmp(iff->ifa_name, "lo", 2) == 0) continue;
            struct sockaddr_in *ba =
                (struct sockaddr_in *)iff->ifa_broadaddr;
            inet_ntop(AF_INET, &ba->sin_addr, bcast, (socklen_t)sz);
            freeifaddrs(ifa);
            return;
        }
        freeifaddrs(ifa);
    }
    /* Fallback: last octet → 255 */
    strncpy(bcast, local_ip, sz);
    char *dot = strrchr(bcast, '.');
    if (dot) snprintf(dot, sz - (size_t)(dot - bcast), ".255");
}

/* ── Peer management ────────────────────────────────────────── */
static void peer_upsert(const char *ip, uint16_t tcp_port,
                        const char *version, const char *hostname) {
    pthread_mutex_lock(&g_peers_lock);
    int64_t now = bridge_ms_now();

    /* Busca si ya existe */
    for (int i = 0; i < g_bridge_n_peers; i++) {
        if (strcmp(g_bridge_peers[i].ip, ip) == 0) {
            g_bridge_peers[i].last_seen_ms = now;
            g_bridge_peers[i].alive = true;
            pthread_mutex_unlock(&g_peers_lock);
            return;
        }
    }
    /* Nuevo peer */
    if (g_bridge_n_peers < BRIDGE_MAX_PEERS) {
        BridgePeer *p = &g_bridge_peers[g_bridge_n_peers++];
        snprintf(p->ip,       sizeof(p->ip),       "%s", ip);
        snprintf(p->version,  sizeof(p->version),  "%s", version);
        snprintf(p->hostname, sizeof(p->hostname), "%s", hostname);
        p->tcp_port    = tcp_port;
        p->alive       = true;
        p->last_seen_ms = now;
        p->load        = 0;
    }
    pthread_mutex_unlock(&g_peers_lock);
}

static void peers_expire(void) {
    pthread_mutex_lock(&g_peers_lock);
    int64_t now = bridge_ms_now();
    for (int i = 0; i < g_bridge_n_peers; i++) {
        if (now - g_bridge_peers[i].last_seen_ms > BRIDGE_TIMEOUT_MS)
            g_bridge_peers[i].alive = false;
    }
    pthread_mutex_unlock(&g_peers_lock);
}

/* ── Emite evento WS con peers actuales ─────────────────────── */
void rigbridge_emit_peers(WsServer *srv) {
    pthread_mutex_lock(&g_peers_lock);

    char buf[4096];
    char *p = buf;
    size_t rem = sizeof(buf);
    int n;

    n = snprintf(p, rem,
        "{\"ev\":\"bridge_peers\",\"n\":%d,\"peers\":[",
        g_bridge_n_peers);
    p += n; rem -= (size_t)n;

    bool first = true;
    for (int i = 0; i < g_bridge_n_peers; i++) {
        BridgePeer *bp = &g_bridge_peers[i];
        n = snprintf(p, rem,
            "%s{\"ip\":\"%s\",\"port\":%u,"
            "\"hostname\":\"%s\",\"version\":\"%s\","
            "\"alive\":%s,\"load\":%d}",
            first ? "" : ",",
            bp->ip, bp->tcp_port,
            bp->hostname, bp->version,
            bp->alive ? "true" : "false",
            bp->load);
        if (n < 0 || (size_t)n >= rem) break;
        p += n; rem -= (size_t)n;
        first = false;
    }
    snprintf(p, rem, "]}");
    pthread_mutex_unlock(&g_peers_lock);

    ws_broadcast(srv, buf, strlen(buf));
}

/* ══════════════════════════════════════════════════════════════
   THREAD: UDP scanner — manda beacon y escucha respuestas
   ══════════════════════════════════════════════════════════════ */
void *rigbridge_scan_thread(void *arg) {
    BridgeScanArg *ba  = (BridgeScanArg *)arg;
    WsServer      *srv = ba->srv;
    int dur_ms = ba->duration_ms > 0 ? ba->duration_ms : 4000;
    free(ba);

    ws_broadcastf(srv,
        "{\"ev\":\"bridge_status\",\"stage\":\"scanning\","
        "\"msg\":\"Enviando beacon UDP en LAN...\"}");

    /* ── Crea socket UDP ── */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ws_broadcastf(srv,
            "{\"ev\":\"bridge_status\",\"stage\":\"error\","
            "\"msg\":\"No se pudo crear socket UDP\"}");
        return NULL;
    }

    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    /* Bind para recibir respuestas */
    struct sockaddr_in local = {0};
    local.sin_family      = AF_INET;
    local.sin_port        = htons(BRIDGE_UDP_PORT);
    local.sin_addr.s_addr = INADDR_ANY;
    bind(sock, (struct sockaddr *)&local, sizeof(local));

    /* Timeout de recepción */
    struct timeval tv = {.tv_sec = 0, .tv_usec = 300000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* ── Construye beacon ── */
    BridgeBeacon beacon = {0};
    beacon.magic    = BRIDGE_MAGIC;
    beacon.type     = 0;
    beacon.tcp_port = BRIDGE_TCP_PORT;
    strncpy(beacon.version, "4.0.0", sizeof(beacon.version)-1);
    get_hostname(beacon.hostname, sizeof(beacon.hostname));

    char local_ip[64] = "127.0.0.1";
    get_local_ip(local_ip, sizeof(local_ip));

    char bcast_ip[64];
    get_broadcast(local_ip, bcast_ip, sizeof(bcast_ip));

    struct sockaddr_in baddr = {0};
    baddr.sin_family      = AF_INET;
    baddr.sin_port        = htons(BRIDGE_UDP_PORT);
    inet_pton(AF_INET, bcast_ip, &baddr.sin_addr);

    int64_t t0 = bridge_ms_now();
    int prev_n = g_bridge_n_peers;

    while ((bridge_ms_now() - t0) < dur_ms) {
        /* Envía beacon */
        sendto(sock, &beacon, sizeof(beacon), 0,
               (struct sockaddr *)&baddr, sizeof(baddr));

        /* Escucha respuestas */
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        BridgeBeacon resp = {0};
        ssize_t nr = recvfrom(sock, &resp, sizeof(resp), 0,
                              (struct sockaddr *)&from, &from_len);
        if (nr == (ssize_t)sizeof(BridgeBeacon) &&
            resp.magic == BRIDGE_MAGIC && resp.type == 1) {
            char peer_ip[64];
            inet_ntop(AF_INET, &from.sin_addr, peer_ip, sizeof(peer_ip));

            /* Ignoramos nuestra propia IP */
            if (strcmp(peer_ip, local_ip) == 0) continue;

            peer_upsert(peer_ip, ntohs(resp.tcp_port),
                        resp.version, resp.hostname);
        }

        /* Expira peers viejos */
        peers_expire();

        /* Emite update si hay cambio */
        int cur_n;
        pthread_mutex_lock(&g_peers_lock);
        cur_n = g_bridge_n_peers;
        pthread_mutex_unlock(&g_peers_lock);

        if (cur_n != prev_n) {
            rigbridge_emit_peers(srv);
            prev_n = cur_n;
        }

        usleep(BRIDGE_BEACON_MS * 1000);
    }

    close(sock);

    /* Emite resultado final */
    int total;
    pthread_mutex_lock(&g_peers_lock);
    total = g_bridge_n_peers;
    pthread_mutex_unlock(&g_peers_lock);

    if (total == 0) {
        ws_broadcastf(srv,
            "{\"ev\":\"bridge_status\",\"stage\":\"done\","
            "\"msg\":\"No se encontraron peers RigCom en la red\","
            "\"n\":0}");
    } else {
        ws_broadcastf(srv,
            "{\"ev\":\"bridge_status\",\"stage\":\"done\","
            "\"msg\":\"%d peer(s) encontrado(s)\",\"n\":%d}",
            total, total);
        rigbridge_emit_peers(srv);
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
   THREAD: compilación distribuida
   Divide .c files entre peers + compila localmente el resto
   ══════════════════════════════════════════════════════════════ */

/* Obtiene lista de .c files del proyecto */
static int list_c_files(char **out, int max) {
    FILE *fp = popen("find src -name '*.c' 2>/dev/null", "r");
    if (!fp) return 0;
    int n = 0;
    char line[512];
    while (n < max && fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len > 0) out[n++] = strdup(line);
    }
    pclose(fp);
    return n;
}

/* Envía archivo fuente a peer via TCP y pide compilación */
static bool send_job_to_peer(const BridgePeer *peer,
                              const char *src_file,
                              uint32_t job_id,
                              WsServer *srv) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(peer->tcp_port);
    inet_pton(AF_INET, peer->ip, &sa.sin_addr);

    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(sock);
        return false;
    }

    /* Lee el archivo fuente */
    FILE *fp = fopen(src_file, "r");
    if (!fp) { close(sock); return false; }
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    rewind(fp);
    char *src_data = malloc((size_t)fsz + 1);
    (void)fread(src_data, 1, (size_t)fsz, fp);
    src_data[fsz] = '\0';
    fclose(fp);

    /* Envía header + source */
    BridgeMsgHdr hdr = {0};
    hdr.magic    = BRIDGE_MAGIC;
    hdr.type     = BRIDGE_JOB_COMPILE;
    hdr.job_id   = job_id;
    hdr.data_len = (uint32_t)fsz;

    send(sock, &hdr, sizeof(hdr), 0);
    send(sock, src_data, (size_t)fsz, 0);
    free(src_data);

    /* Espera resultado */
    BridgeMsgHdr resp = {0};
    ssize_t nr = recv(sock, &resp, sizeof(resp), MSG_WAITALL);
    bool ok = (nr == (ssize_t)sizeof(BridgeMsgHdr) &&
               resp.magic == BRIDGE_MAGIC &&
               resp.type  == BRIDGE_JOB_RESULT);

    if (ok && resp.data_len > 0) {
        /* Recibe .o object file */
        char obj_name[512];
        snprintf(obj_name, sizeof(obj_name),
                 "build/obj/remote_%u.o", job_id);
        FILE *of = fopen(obj_name, "wb");
        if (of) {
            char rbuf[4096];
            uint32_t remaining = resp.data_len;
            while (remaining > 0) {
                size_t chunk = remaining < sizeof(rbuf) ?
                               remaining : sizeof(rbuf);
                nr = recv(sock, rbuf, chunk, 0);
                if (nr <= 0) break;
                fwrite(rbuf, 1, (size_t)nr, of);
                remaining -= (uint32_t)nr;
            }
            fclose(of);
        }
    }

    close(sock);

    ws_broadcastf(srv,
        "{\"ev\":\"bridge_job_done\","
        "\"job_id\":%u,\"file\":\"%s\","
        "\"peer\":\"%s\",\"ok\":%s}",
        job_id, src_file, peer->ip,
        ok ? "true" : "false");

    return ok;
}

void *rigbridge_build_thread(void *arg) {
    BridgeBuildArg *ba  = (BridgeBuildArg *)arg;
    WsServer       *srv = ba->srv;
    free(ba);

    /* Recopila .c files del proyecto */
    char *files[256];
    int n_files = list_c_files(files, 256);

    if (n_files == 0) {
        ws_broadcastf(srv,
            "{\"ev\":\"bridge_status\",\"stage\":\"error\","
            "\"msg\":\"No se encontraron archivos .c en src/\"}");
        return NULL;
    }

    /* Cuenta peers vivos */
    pthread_mutex_lock(&g_peers_lock);
    int n_alive = 0;
    int alive_idx[BRIDGE_MAX_PEERS];
    for (int i = 0; i < g_bridge_n_peers; i++)
        if (g_bridge_peers[i].alive)
            alive_idx[n_alive++] = i;
    pthread_mutex_unlock(&g_peers_lock);

    ws_broadcastf(srv,
        "{\"ev\":\"bridge_status\",\"stage\":\"distributing\","
        "\"msg\":\"Distribuyendo %d archivos entre %d peer(s) + local\","
        "\"files\":%d,\"peers\":%d}",
        n_files, n_alive, n_files, n_alive);

    /* Distribución: round-robin entre peers, resto local */
    int local_jobs = 0, remote_jobs = 0;
    uint32_t job_id = 1;

    for (int i = 0; i < n_files; i++) {
        if (n_alive > 0) {
            pthread_mutex_lock(&g_peers_lock);
            BridgePeer *peer = &g_bridge_peers[alive_idx[i % n_alive]];
            pthread_mutex_unlock(&g_peers_lock);

            ws_broadcastf(srv,
                "{\"ev\":\"bridge_job_start\","
                "\"job_id\":%u,\"file\":\"%s\","
                "\"peer\":\"%s\"}",
                job_id, files[i], peer->ip);

            bool ok = send_job_to_peer(peer, files[i], job_id, srv);
            if (ok) { remote_jobs++; }
            else    { local_jobs++;  /* fallback local */ }
        } else {
            local_jobs++;
        }
        job_id++;
    }

    /* Compila archivos locales restantes */
    if (local_jobs > 0) {
        ws_broadcastf(srv,
            "{\"ev\":\"bridge_status\",\"stage\":\"local\","
            "\"msg\":\"Compilando %d archivos localmente...\","
            "\"n\":%d}", local_jobs, local_jobs);
        (void)system("make 2>&1 | tail -10");
    }

    ws_broadcastf(srv,
        "{\"ev\":\"bridge_done\","
        "\"ok\":true,"
        "\"local\":%d,\"remote\":%d,\"total\":%d}",
        local_jobs, remote_jobs, n_files);

    for (int i = 0; i < n_files; i++) free(files[i]);
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
   THREAD: listener TCP — acepta trabajos de otros peers
   ══════════════════════════════════════════════════════════════ */
void *rigbridge_listener_thread(void *arg) {
    (void)arg;

    int srv_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_sock < 0) return NULL;

    int reuse = 1;
    setsockopt(srv_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in sa = {0};
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(BRIDGE_TCP_PORT);
    sa.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv_sock, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(srv_sock, 8) != 0) {
        close(srv_sock);
        return NULL;
    }

    while (1) {
        struct sockaddr_in client = {0};
        socklen_t clen = sizeof(client);
        int csock = accept(srv_sock, (struct sockaddr *)&client, &clen);
        if (csock < 0) continue;

        /* Recibe header */
        BridgeMsgHdr hdr = {0};
        if (recv(csock, &hdr, sizeof(hdr), MSG_WAITALL) !=
            (ssize_t)sizeof(BridgeMsgHdr) ||
            hdr.magic != BRIDGE_MAGIC) {
            close(csock);
            continue;
        }

        if (hdr.type == BRIDGE_JOB_COMPILE && hdr.data_len > 0 &&
            hdr.data_len < (1 << 20)) {
            /* Recibe source */
            char *src = malloc(hdr.data_len + 1);
            recv(csock, src, hdr.data_len, MSG_WAITALL);
            src[hdr.data_len] = '\0';

            /* Escribe a archivo temp y compila */
            char tmp_c[128], tmp_o[128];
            snprintf(tmp_c, sizeof(tmp_c),
                     "/tmp/rigbridge_%u.c", hdr.job_id);
            snprintf(tmp_o, sizeof(tmp_o),
                     "/tmp/rigbridge_%u.o", hdr.job_id);

            FILE *fp = fopen(tmp_c, "w");
            if (fp) { fputs(src, fp); fclose(fp); }
            free(src);

            char cmd[512];
            snprintf(cmd, sizeof(cmd),
                     "clang -std=c11 -O2 -c %s -o %s 2>/dev/null",
                     tmp_c, tmp_o);
            int rc = system(cmd);
            unlink(tmp_c);

            /* Envía objeto compilado */
            BridgeMsgHdr resp = {0};
            resp.magic  = BRIDGE_MAGIC;
            resp.job_id = hdr.job_id;

            if (rc == 0) {
                resp.type = BRIDGE_JOB_RESULT;
                FILE *of = fopen(tmp_o, "rb");
                if (of) {
                    fseek(of, 0, SEEK_END);
                    long osz = ftell(of);
                    rewind(of);
                    resp.data_len = (uint32_t)osz;
                    send(csock, &resp, sizeof(resp), 0);
                    char obuf[4096];
                    size_t nr;
                    while ((nr = fread(obuf, 1, sizeof(obuf), of)) > 0)
                        send(csock, obuf, nr, 0);
                    fclose(of);
                    unlink(tmp_o);
                } else {
                    resp.data_len = 0;
                    send(csock, &resp, sizeof(resp), 0);
                }
            } else {
                resp.type     = BRIDGE_JOB_ERROR;
                resp.data_len = 0;
                send(csock, &resp, sizeof(resp), 0);
            }
        }
        close(csock);
    }
    close(srv_sock);
    return NULL;
}
