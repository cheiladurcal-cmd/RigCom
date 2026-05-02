/* ============================================================
   RigCom v8.0 — src/wsserver.c
   WebSocket + HTTP server — RFC 6455
   SHA-1 propio · Base64 propio · select() multiplexado
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/wsserver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ═══════════════════════════════════════════════════════════════
   SHA-1  (FIPS 180-4, public domain)
   ═══════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buf[64];
} SHA1Ctx;

#define ROL32(x,n) (((x)<<(n))|((x)>>(32-(n))))

static void sha1_transform(SHA1Ctx *ctx) {
    uint32_t a,b,c,d,e,f,k,temp;
    uint32_t W[80];
    uint8_t *p = ctx->buf;
    for (int i=0;i<16;i++,p+=4)
        W[i]=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
             ((uint32_t)p[2]<<8)|(uint32_t)p[3];
    for (int i=16;i<80;i++)
        W[i]=ROL32(W[i-3]^W[i-8]^W[i-14]^W[i-16],1);
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2];
    d=ctx->state[3]; e=ctx->state[4];
    for (int i=0;i<80;i++) {
        if      (i<20){f=(b&c)|(~b&d);       k=0x5A827999u;}
        else if (i<40){f=b^c^d;               k=0x6ED9EBA1u;}
        else if (i<60){f=(b&c)|(b&d)|(c&d);  k=0x8F1BBCDCu;}
        else           {f=b^c^d;               k=0xCA62C1D6u;}
        temp=ROL32(a,5)+f+e+k+W[i];
        e=d; d=c; c=ROL32(b,30); b=a; a=temp;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c;
    ctx->state[3]+=d; ctx->state[4]+=e;
}
static void sha1_init(SHA1Ctx *ctx) {
    ctx->state[0]=0x67452301u; ctx->state[1]=0xEFCDAB89u;
    ctx->state[2]=0x98BADCFEu; ctx->state[3]=0x10325476u;
    ctx->state[4]=0xC3D2E1F0u; ctx->count=0;
}
static void sha1_update(SHA1Ctx *ctx, const uint8_t *data, size_t len) {
    uint32_t off = (uint32_t)(ctx->count & 63);
    ctx->count += len;
    while (len--) {
        ctx->buf[off++] = *data++;
        if (off==64) { sha1_transform(ctx); off=0; }
    }
}
static void sha1_final(SHA1Ctx *ctx, uint8_t digest[20]) {
    uint32_t off = (uint32_t)(ctx->count & 63);
    ctx->buf[off++]=0x80;
    if (off>56) { memset(ctx->buf+off,0,64-off); sha1_transform(ctx); off=0; }
    memset(ctx->buf+off,0,56-off);
    uint64_t bits=ctx->count*8;
    for (int i=7;i>=0;i--) { ctx->buf[56+i]=(uint8_t)(bits&0xff); bits>>=8; }
    sha1_transform(ctx);
    for (int i=0;i<5;i++) {
        digest[i*4+0]=(uint8_t)(ctx->state[i]>>24);
        digest[i*4+1]=(uint8_t)(ctx->state[i]>>16);
        digest[i*4+2]=(uint8_t)(ctx->state[i]>>8);
        digest[i*4+3]=(uint8_t)(ctx->state[i]);
    }
}
static void sha1(const uint8_t *data, size_t len, uint8_t digest[20]) {
    SHA1Ctx ctx; sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, digest);
}

/* ═══════════════════════════════════════════════════════════════
   BASE64 ENCODE
   ═══════════════════════════════════════════════════════════════ */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const uint8_t *in, size_t ilen, char *out, size_t olen) {
    size_t needed = 4 * ((ilen + 2) / 3) + 1;
    if (olen < needed) return -1;
    size_t i=0, j=0;
    while (i+2 < ilen) {
        out[j++]=B64[ in[i]>>2];
        out[j++]=B64[(in[i]&0x3)<<4|(in[i+1]>>4)];
        out[j++]=B64[(in[i+1]&0xf)<<2|(in[i+2]>>6)];
        out[j++]=B64[ in[i+2]&0x3f];
        i+=3;
    }
    if (i<ilen) {
        out[j++]=B64[in[i]>>2];
        if (i+1<ilen) {
            out[j++]=B64[(in[i]&0x3)<<4|(in[i+1]>>4)];
            out[j++]=B64[(in[i+1]&0xf)<<2];
        } else {
            out[j++]=B64[(in[i]&0x3)<<4];
            out[j++]='=';
        }
        out[j++]='=';
    }
    out[j]='\0';
    return (int)j;
}

/* ═══════════════════════════════════════════════════════════════
   WEBSOCKET HANDSHAKE
   ═══════════════════════════════════════════════════════════════ */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static bool ws_do_handshake(int fd, const char *request) {
    /* Find Sec-WebSocket-Key */
    const char *key_hdr = strstr(request, "Sec-WebSocket-Key:");
    if (!key_hdr) key_hdr = strstr(request, "sec-websocket-key:");
    if (!key_hdr) return false;

    key_hdr += 18; /* skip header name */
    while (*key_hdr == ' ') key_hdr++;

    char key[64];
    int ki = 0;
    while (key_hdr[ki] && key_hdr[ki] != '\r' && key_hdr[ki] != '\n' && ki < 63) {
        key[ki] = key_hdr[ki]; ki++;
    }
    key[ki] = '\0';

    /* Rewrite: loop above has a subtle bug — fix with explicit copy */
    {
        const char *p = key_hdr;
        int n = 0;
        while (*p && *p != '\r' && *p != '\n' && n < 63)
            key[n++] = *p++;
        key[n] = '\0';
    }

    /* Concatenate with GUID and SHA1 */
    char combined[256];
    int clen = snprintf(combined, sizeof(combined), "%s%s", key, WS_GUID);

    uint8_t digest[20];
    sha1((const uint8_t *)combined, (size_t)clen, digest);

    char accept[64];
    b64_encode(digest, 20, accept, sizeof(accept));

    /* Send 101 Switching Protocols */
    char resp[512];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n",
        accept);

    return write(fd, resp, (size_t)rlen) == rlen;
}

/* ═══════════════════════════════════════════════════════════════
   WEBSOCKET FRAME — SEND (server→client, no masking)
   ═══════════════════════════════════════════════════════════════ */
static void ws_send_text(int fd, const char *text, size_t len) {
    uint8_t hdr[10];
    int     hlen;
    hdr[0] = 0x81; /* FIN + opcode=1 (text) */

    if (len < 126) {
        hdr[1] = (uint8_t)len;
        hlen = 2;
    } else if (len < 65536) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)(len & 0xff);
        hlen = 4;
    } else {
        hdr[1] = 127;
        for (int i=0;i<8;i++)
            hdr[2+i] = (uint8_t)(len >> (56 - 8*i));
        hlen = 10;
    }

    /* Atomic send: header + payload */
    struct iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len  = (size_t)hlen;
    iov[1].iov_base = (void *)text;
    iov[1].iov_len  = len;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
    writev(fd, iov, 2);
#pragma GCC diagnostic pop

}

/* ═══════════════════════════════════════════════════════════════
   WEBSOCKET FRAME — RECEIVE (client→server, masked)
   Returns payload in buf (unmasked), or NULL on error/close.
   ═══════════════════════════════════════════════════════════════ */
/* ── ws_read_frame: FSM-based TCP fragmentation-safe reader ───
   The client's WsFsmState machine advances through:
     HEADER → (LEN16|LEN64)? → MASK? → PAYLOAD → done
   Each call to recv() can deliver any amount of bytes; the FSM
   picks up exactly where it left off on the next call.
   Returns: >0  = payload_out has N bytes ready
             0  = need more data (not an error)
            -1  = connection closed or protocol error
   ─────────────────────────────────────────────────────────── */
static int ws_read_frame(WsClient *cl, char *payload_out, size_t payload_max) {
    uint8_t tmp[4096];

    int n = (int)recv(cl->fd, tmp, sizeof(tmp), 0);
    if (n <= 0) return -1;

    for (int ni = 0; ni < n; ni++) {
        uint8_t byte = tmp[ni];

        switch (cl->fsm) {

        case WS_FS_HEADER:
            cl->fsm_hdr[cl->fsm_hdr_got++] = byte;
            if (cl->fsm_hdr_got < 2) break;

            cl->fsm_opcode = cl->fsm_hdr[0] & 0x0f;
            cl->fsm_masked = (cl->fsm_hdr[1] & 0x80) != 0;
            {
                uint8_t raw_len = cl->fsm_hdr[1] & 0x7f;
                if (raw_len == 126) {
                    cl->fsm = WS_FS_LEN16;
                    cl->fsm_hdr_got  = 0;
                    cl->fsm_hdr_need = 2;
                } else if (raw_len == 127) {
                    cl->fsm = WS_FS_LEN64;
                    cl->fsm_hdr_got  = 0;
                    cl->fsm_hdr_need = 8;
                } else {
                    cl->fsm_pay_len = raw_len;
                    cl->fsm_pay_got = 0;
                    if (cl->fsm_masked) {
                        cl->fsm = WS_FS_MASK;
                        cl->fsm_hdr_got = 0;
                    } else {
                        cl->fsm = WS_FS_PAYLOAD;
                        if (cl->fsm_pay_len == 0) goto frame_done;
                        if (!cl->fsm_payload)
                            cl->fsm_payload = malloc(cl->fsm_pay_len + 1);
                    }
                }
            }
            break;

        case WS_FS_LEN16:
            cl->fsm_hdr[cl->fsm_hdr_got++] = byte;
            if (cl->fsm_hdr_got < 2) break;
            cl->fsm_pay_len = ((uint64_t)cl->fsm_hdr[0] << 8) | cl->fsm_hdr[1];
            cl->fsm_pay_got = 0;
            cl->fsm_hdr_got = 0;
            cl->fsm = cl->fsm_masked ? WS_FS_MASK : WS_FS_PAYLOAD;
            if (!cl->fsm_masked && !cl->fsm_payload)
                cl->fsm_payload = malloc(cl->fsm_pay_len + 1);
            break;

        case WS_FS_LEN64:
            cl->fsm_hdr[cl->fsm_hdr_got++] = byte;
            if (cl->fsm_hdr_got < 8) break;
            cl->fsm_pay_len = 0;
            for (int k = 0; k < 8; k++)
                cl->fsm_pay_len = (cl->fsm_pay_len << 8) | cl->fsm_hdr[k];
            cl->fsm_pay_got = 0;
            cl->fsm_hdr_got = 0;
            cl->fsm = cl->fsm_masked ? WS_FS_MASK : WS_FS_PAYLOAD;
            if (!cl->fsm_masked && !cl->fsm_payload)
                cl->fsm_payload = malloc(cl->fsm_pay_len + 1);
            break;

        case WS_FS_MASK:
            cl->fsm_mask[cl->fsm_hdr_got++] = byte;
            if (cl->fsm_hdr_got < 4) break;
            cl->fsm_hdr_got = 0;
            cl->fsm = WS_FS_PAYLOAD;
            if (!cl->fsm_payload)
                cl->fsm_payload = malloc(cl->fsm_pay_len + 1);
            if (cl->fsm_pay_len == 0) goto frame_done;
            break;

        case WS_FS_PAYLOAD:
            if (cl->fsm_payload && cl->fsm_pay_got < cl->fsm_pay_len) {
                uint8_t decoded = byte;
                if (cl->fsm_masked)
                    decoded ^= cl->fsm_mask[cl->fsm_pay_got % 4];
                cl->fsm_payload[cl->fsm_pay_got] = (char)decoded;
            }
            cl->fsm_pay_got++;

            if (cl->fsm_pay_got >= cl->fsm_pay_len) {
frame_done:;
                /* Handle control frames inline */
                if (cl->fsm_opcode == 8) {
                    free(cl->fsm_payload); cl->fsm_payload = NULL;
                    cl->fsm = WS_FS_HEADER; cl->fsm_hdr_got = 0;
                    return -1;
                }
                if (cl->fsm_opcode == 9) {
                    uint8_t pong[2] = {0x8A, 0x00};
                    (void)write(cl->fd, pong, 2);
                    free(cl->fsm_payload); cl->fsm_payload = NULL;
                    cl->fsm = WS_FS_HEADER; cl->fsm_hdr_got = 0;
                    break;
                }

                /* Data frame ready */
                size_t copy_len = cl->fsm_pay_len;
                if (copy_len >= payload_max) copy_len = payload_max - 1;
                if (cl->fsm_payload && copy_len > 0)
                    memcpy(payload_out, cl->fsm_payload, copy_len);
                payload_out[copy_len] = '\0';

                free(cl->fsm_payload); cl->fsm_payload = NULL;
                cl->fsm = WS_FS_HEADER; cl->fsm_hdr_got = 0;
                memset(cl->fsm_mask, 0, 4);

                return (int)copy_len;
            }
            break;
        }
    }

    return 0; /* need more data */
}

/* ═══════════════════════════════════════════════════════════════
   HTTP REQUEST HANDLER
   ═══════════════════════════════════════════════════════════════ */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
static void serve_dashboard(int fd) {
    /* Try to load dashboard.html from disk */
    FILE *f = fopen("dashboard.html", "rb");
    if (!f) f = fopen("../dashboard.html", "rb");

    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        char *html = malloc((size_t)sz + 1);
        if (html) {
            (void)fread(html, 1, (size_t)sz, f);
            html[sz] = '\0';
            char hdr[256];
            int hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Content-Length: %ld\r\n"
                "Connection: close\r\n\r\n", sz);
            (void)write(fd, hdr, (size_t)hlen);
            (void)write(fd, html, (size_t)sz);
            free(html);
        }
        fclose(f);
    } else {
        const char *body = "<html><body><h1>RigCom v8.0</h1>"
                           "<p>dashboard.html no encontrado.</p></body></html>";
        char hdr[256];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
            "Content-Length: %zu\r\n\r\n", strlen(body));
        (void)write(fd, hdr, (size_t)hlen);
        (void)write(fd, body, strlen(body));
    }
}

#pragma GCC diagnostic pop
/* ═══════════════════════════════════════════════════════════════
   HANDLE NEW TCP CONNECTION
   ═══════════════════════════════════════════════════════════════ */
static void handle_new_conn(WsServer *srv, int fd) {
    /* Read HTTP request */
    char req[4096];
    int  rn = (int)recv(fd, req, sizeof(req)-1, 0);
    if (rn <= 0) { close(fd); return; }
    req[rn] = '\0';

    bool is_ws_upgrade =
        (strstr(req, "Upgrade: websocket") != NULL ||
         strstr(req, "upgrade: websocket") != NULL);

    if (is_ws_upgrade) {
        if (!ws_do_handshake(fd, req)) { close(fd); return; }
        if (srv->n_clients >= WS_MAX_CLIENTS) { close(fd); return; }
        int idx = srv->n_clients++;
        memset(&srv->clients[idx], 0, sizeof(WsClient));
        srv->clients[idx].fd          = fd;
        srv->clients[idx].handshaked  = true;

        /* Announce server state to this new client */
        ws_broadcastf(srv, "{\"ev\":\"ready\",\"version\":\"4.0.0\","
                      "\"phi\":\"1.6180339887498948482\","
                      "\"schumann\":7.83}");
    } else {
        /* Plain HTTP */
        if (strncmp(req, "GET / ", 6) == 0 ||
            strncmp(req, "GET /index", 10) == 0) {
            serve_dashboard(fd);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
        } else {
            const char *r404 = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            write(fd, r404, strlen(r404));
        }
#pragma GCC diagnostic pop
        close(fd);
    }
}

/* ═══════════════════════════════════════════════════════════════
   REMOVE DISCONNECTED CLIENT
   ═══════════════════════════════════════════════════════════════ */
static void remove_client(WsServer *srv, int idx) {
    close(srv->clients[idx].fd);
    /* Shift remaining clients */
    for (int i = idx; i < srv->n_clients - 1; i++)
        srv->clients[i] = srv->clients[i+1];
    srv->n_clients--;
}

/* ═══════════════════════════════════════════════════════════════
   ws_server_init
   ═══════════════════════════════════════════════════════════════ */
int ws_server_init(WsServer *srv, uint16_t port) {
    memset(srv, 0, sizeof(WsServer));
    srv->port    = port;
    srv->running = true;
    pthread_mutex_init(&srv->send_lock, NULL);

    srv->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->server_fd < 0) {
        perror("[wsserver] socket");
        return -1;
    }

    int opt = 1;
    setsockopt(srv->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Non-blocking server socket */
    int flags = fcntl(srv->server_fd, F_GETFL, 0);
    fcntl(srv->server_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(srv->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[wsserver] bind");
        close(srv->server_fd);
        return -1;
    }
    if (listen(srv->server_fd, 8) < 0) {
        perror("[wsserver] listen");
        close(srv->server_fd);
        return -1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   ws_server_run — blocking select() loop
   ═══════════════════════════════════════════════════════════════ */
void ws_server_run(WsServer *srv) {
    char payload[WS_FRAME_MAX];

    while (srv->running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv->server_fd, &rfds);
        int maxfd = srv->server_fd;

        for (int i = 0; i < srv->n_clients; i++) {
            if (srv->clients[i].handshaked) {
                FD_SET(srv->clients[i].fd, &rfds);
                if (srv->clients[i].fd > maxfd)
                    maxfd = srv->clients[i].fd;
            }
        }

        struct timeval tv = {0, 200000}; /* 200ms timeout */
        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* New connection */
        if (FD_ISSET(srv->server_fd, &rfds)) {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int cfd = accept(srv->server_fd, (struct sockaddr *)&caddr, &clen);
            if (cfd >= 0) {
                /* Set client socket to blocking */
                int flags = fcntl(cfd, F_GETFL, 0);
                fcntl(cfd, F_SETFL, flags & ~O_NONBLOCK);
                handle_new_conn(srv, cfd);
            }
        }

        /* Service connected WS clients */
        for (int i = srv->n_clients - 1; i >= 0; i--) {
            WsClient *cl = &srv->clients[i];
            if (!cl->handshaked) continue;
            if (!FD_ISSET(cl->fd, &rfds)) continue;

            int plen = ws_read_frame(cl, payload, sizeof(payload));
            if (plen < 0) {
                remove_client(srv, i);
                continue;
            }
            if (plen > 0 && srv->on_message)
                srv->on_message(srv, payload, (size_t)plen, srv->user);
        }
    }

    /* Cleanup */
    for (int i = 0; i < srv->n_clients; i++)
        close(srv->clients[i].fd);
    close(srv->server_fd);
}

/* ── Stop ─────────────────────────────────────────────────── */
void ws_server_stop(WsServer *srv) {
    srv->running = false;
}

/* ═══════════════════════════════════════════════════════════════
   ws_broadcast — thread-safe send to all WS clients
   ═══════════════════════════════════════════════════════════════ */
void ws_broadcast(WsServer *srv, const char *text, size_t len) {
    if (!srv || !text || len == 0) return;
    pthread_mutex_lock(&srv->send_lock);
    for (int i = 0; i < srv->n_clients; i++) {
        if (srv->clients[i].handshaked)
            ws_send_text(srv->clients[i].fd, text, len);
    }
    pthread_mutex_unlock(&srv->send_lock);
}

void ws_broadcastf(WsServer *srv, const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) ws_broadcast(srv, buf, (size_t)n);
}
