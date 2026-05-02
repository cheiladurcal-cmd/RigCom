/* ============================================================
   RigCom v8.0 — src/holo_trace.c
   Holographic Execution Trace
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/holo_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── Gestión de nodos ── */
static HoloNode* holo_find_or_create(HoloSession *s, const char *name) {
    for (HoloNode *n = s->nodes; n; n = n->next) {
        if (strcmp(n->name, name) == 0) return n;
    }
    HoloNode *n = calloc(1, sizeof(HoloNode));
    if (!n) return NULL;
    strncpy(n->name, name, sizeof(n->name)-1);
    n->id     = s->n_nodes++;
    n->energy = 0.0;
    n->next   = s->nodes;
    s->nodes  = n;
    return n;
}

static void holo_add_edge(HoloSession *s, uint32_t from, uint32_t to) {
    for (HoloEdge *e = s->edges; e; e = e->next) {
        if (e->from_id == from && e->to_id == to) { e->weight++; return; }
    }
    HoloEdge *e = calloc(1, sizeof(HoloEdge));
    if (!e) return;
    e->from_id = from; e->to_id = to; e->weight = 1;
    e->next    = s->edges;
    s->edges   = e;
    s->n_edges++;
}

/* ── Ciclo de vida ── */
HoloSession* holo_session_new(RigCtx *ctx, WsServer *ws) {
    HoloSession *s = calloc(1, sizeof(HoloSession));
    if (!s) return NULL;
    s->ctx        = ctx;
    s->ws         = ws;
    s->decay_rate = 0.04;
    return s;
}

void holo_session_free(HoloSession *s) {
    if (!s) return;
    HoloNode *n = s->nodes; while(n){HoloNode *nx=n->next;free(n);n=nx;}
    HoloEdge *e = s->edges; while(e){HoloEdge *nx=e->next;free(e);e=nx;}
    free(s);
}

bool holo_attach(HoloSession *s, int pid) {
    s->ptrace_pid = pid;
    s->active     = true;
    ws_broadcastf(s->ws,
        "{\"ev\":\"holo_attached\",\"pid\":%d,"
        "\"msg\":\"Traza holográfica activa\"}", pid);
    return true;
}

/* ── Registro de llamadas ── */
void holo_on_call(HoloSession *s, const char *fn_name,
                   const char *from_fn, const char *file, uint32_t line) {
    if (!s || !fn_name) return;
    HoloNode *n = holo_find_or_create(s, fn_name);
    if (!n) return;
    n->call_count++;
    n->energy = 1.0; /* máximo glow */
    if (file[0]) strncpy(n->file, file, sizeof(n->file)-1);
    if (line)     n->line = line;

    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    n->last_ts_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    if (from_fn && from_fn[0]) {
        HoloNode *src = holo_find_or_create(s, from_fn);
        if (src) holo_add_edge(s, src->id, n->id);
    }
    holo_emit_glow(s, fn_name);
}

/* ── Emitir eventos WebSocket ── */
void holo_emit_glow(HoloSession *s, const char *fn_name) {
    HoloNode *n = NULL;
    for (HoloNode *it = s->nodes; it; it = it->next)
        if (strcmp(it->name, fn_name)==0) { n=it; break; }
    if (!n) return;
    ws_broadcastf(s->ws,
        "{\"ev\":\"holo_glow\","
        "\"id\":%u,"
        "\"fn\":\"%s\","
        "\"energy\":%.4f,"
        "\"calls\":%llu,"
        "\"file\":\"%s\","
        "\"line\":%u}",
        n->id, n->name, n->energy,
        (unsigned long long)n->call_count,
        n->file, n->line);
}

void holo_emit_graph(HoloSession *s) {
    /* Emitir todos los nodos */
    ws_broadcastf(s->ws,
        "{\"ev\":\"holo_graph_start\","
        "\"n_nodes\":%u,\"n_edges\":%u}",
        s->n_nodes, s->n_edges);

    for (HoloNode *n = s->nodes; n; n = n->next) {
        ws_broadcastf(s->ws,
            "{\"ev\":\"holo_node\","
            "\"id\":%u,\"fn\":\"%s\","
            "\"calls\":%llu,\"energy\":%.4f,"
            "\"file\":\"%s\",\"line\":%u}",
            n->id, n->name,
            (unsigned long long)n->call_count,
            n->energy, n->file, n->line);
    }
    for (HoloEdge *e = s->edges; e; e = e->next) {
        ws_broadcastf(s->ws,
            "{\"ev\":\"holo_edge\","
            "\"from\":%u,\"to\":%u,\"weight\":%llu}",
            e->from_id, e->to_id,
            (unsigned long long)e->weight);
    }
    ws_broadcastf(s->ws, "{\"ev\":\"holo_graph_end\"}");
}

/* ── Decay de energía ── */
void holo_tick(HoloSession *s) {
    for (HoloNode *n = s->nodes; n; n = n->next) {
        n->energy -= s->decay_rate;
        if (n->energy < 0.0) n->energy = 0.0;
        if (n->energy > 0.01) {
            ws_broadcastf(s->ws,
                "{\"ev\":\"holo_decay\","
                "\"id\":%u,\"energy\":%.4f}",
                n->id, n->energy);
        }
    }
}

/* ── Parser de líneas PTY ──
   Detecta patrones comunes de traza:
   • GDB/LLDB: "Breakpoint 1, main_loop (...)"
   • printf RigTrace: "[RIG] fn_name file:line"
   • Sanitizer: "#0 0x... in fn_name file:line"              */
void holo_parse_line(HoloSession *s, const char *line) {
    if (!line || !line[0]) return;

    char fn[128]={0}, file[256]={0};
    uint32_t lno = 0;

    /* Patrón: [RIG] nombre_función archivo:línea */
    if (strstr(line, "[RIG]")) {
        const char *p = strstr(line, "[RIG]") + 5;
        while (*p == ' ') p++;
        int i = 0;
        while (*p && *p != ' ' && i < 127) fn[i++] = *p++;
        fn[i] = '\0';
        const char *colon = strrchr(p, ':');
        if (colon) {
            lno = (uint32_t)strtoul(colon+1, NULL, 10);
            int flen = (int)(colon - p - 1);
            if (flen > 0 && flen < 255)
                strncpy(file, p+1, (size_t)flen);
        }
        if (fn[0]) holo_on_call(s, fn, "", file, lno);
        return;
    }

    /* Patrón: sanitizer "#N 0x... in fn_name " */
    if (strstr(line, " in ")) {
        const char *in = strstr(line, " in ") + 4;
        int i = 0;
        while (*in && *in != ' ' && *in != '(' && i < 127)
            fn[i++] = *in++;
        fn[i] = '\0';
        if (fn[0] && fn[0] != '_')
            holo_on_call(s, fn, "", "", 0);
        return;
    }

    /* Patrón GDB: "Breakpoint N, fn_name (" */
    if (strncmp(line, "Breakpoint", 10) == 0) {
        const char *comma = strchr(line, ',');
        if (comma) {
            comma++;
            while (*comma == ' ') comma++;
            int i = 0;
            while (*comma && *comma != ' ' && *comma != '(' && i < 127)
                fn[i++] = *comma++;
            fn[i] = '\0';
            if (fn[0]) holo_on_call(s, fn, "", "", 0);
        }
    }
}

/* ── Thread principal ── */
void* holo_trace_thread(void *arg) {
    HoloThreadArg *a = (HoloThreadArg*)arg;
    HoloSession   *s = a->session;
    holo_attach(s, a->pid);

    /* Loop: tick cada 100ms, emitir grafo cada 5s */
    uint32_t ticks = 0;
    while (s->active) {
        usleep(100000); /* 100ms */
        holo_tick(s);
        ticks++;
        if (ticks % 50 == 0) { /* cada 5s */
            holo_emit_graph(s);
        }
    }
    free(a);
    return NULL;
}
