/* ============================================================
   RigCom v8.0 — src/rigcanvas.c
   RigCanvas: framework de UI GLSL immediate-mode.
   Diseño:
     · C puro emite JSON de widgets por WebSocket.
     · Frontend (index.html) tiene un loop requestAnimationFrame
       que recibe el JSON y renderiza cada widget con WebGL 2.0
       usando los shaders GLSL enviados una sola vez al inicio.
     · Clicks/hovers vienen del frontend como eventos WS y son
       procesados por rc_handle_event().
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/rigcanvas.h"
#include "../include/wsserver.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ══════════════════════════════════════════════════════════════
   SHADERS GLSL ES 3.00
   ══════════════════════════════════════════════════════════════ */

const char *RC_VERT_QUAD =
    "#version 300 es\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "uniform vec4 u_rect;\n"      /* x, y, w, h en NDC */
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 p = u_rect.xy + a_pos * u_rect.zw;\n"
    "  v_uv = a_uv;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* ── Glassmorphism Dorado ─────────────────────────────────── */
const char *RC_FRAG_GLASS_GOLD =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_uv;\n"
    "uniform vec4  u_rect;\n"
    "uniform float u_radius;\n"  /* radio de esquinas, px */
    "uniform float u_time;\n"
    "uniform float u_value;\n"   /* [0,1] hover brightness */
    "out vec4 fragColor;\n"
    "\n"
    "float sdRoundBox(vec2 p, vec2 b, float r) {\n"
    "  vec2 q = abs(p) - b + vec2(r);\n"
    "  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "  vec2  uv   = v_uv;\n"
    "  vec2  p    = uv - 0.5;\n"
    "  float rPx  = u_radius / max(u_rect.z, 1.0);\n"
    "  float d    = sdRoundBox(p, vec2(0.5) - vec2(rPx), rPx);\n"
    "  float mask = 1.0 - smoothstep(-0.003, 0.003, d);\n"
    "\n"
    "  float bw   = 0.018;\n"
    "  float bord  = 1.0 - smoothstep(-bw * 0.5, bw * 0.5, d);\n"
    "  float inner = 1.0 - smoothstep(0.0, bw, d + bw * 1.5);\n"
    "\n"
    /* Gradiente dorado: amarillo → cobre */
    "  vec3 gold_hi = vec3(1.00, 0.85, 0.15);\n"
    "  vec3 gold_lo = vec3(0.75, 0.50, 0.08);\n"
    "  vec3 gold    = mix(gold_hi, gold_lo, uv.y);\n"
    "\n"
    /* Pulso sutil */
    "  float pulse  = 0.04 * sin(u_time * 2.0 + uv.x * 6.28);\n"
    "  float bright = 0.28 + u_value * 0.18 + pulse;\n"
    "\n"
    /* Glass fill: fondo semitransparente con tinte dorado tenue */
    "  vec3 glass   = vec3(0.08, 0.06, 0.02) + gold * bright;\n"
    "  vec3 col     = mix(glass, gold, bord * 0.85);\n"
    "\n"
    "  float alpha  = mask * (inner * 0.38 + bord * 0.92);\n"
    "  fragColor    = vec4(col, alpha);\n"
    "}\n";

/* ── Glass Cian (paneles de info) ─────────────────────────── */
const char *RC_FRAG_GLASS_CYAN =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_uv;\n"
    "uniform vec4  u_rect;\n"
    "uniform float u_radius;\n"
    "uniform float u_time;\n"
    "uniform float u_value;\n"
    "out vec4 fragColor;\n"
    "\n"
    "float sdRoundBox(vec2 p, vec2 b, float r) {\n"
    "  vec2 q = abs(p) - b + vec2(r);\n"
    "  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n"
    "void main() {\n"
    "  vec2  p   = v_uv - 0.5;\n"
    "  float rPx = u_radius / max(u_rect.z, 1.0);\n"
    "  float d   = sdRoundBox(p, vec2(0.5) - vec2(rPx), rPx);\n"
    "  float msk = 1.0 - smoothstep(-0.003, 0.003, d);\n"
    "  float bw  = 0.016;\n"
    "  float brd = 1.0 - smoothstep(-bw*0.5, bw*0.5, d);\n"
    "  float inn = 1.0 - smoothstep(0.0, bw, d + bw*1.5);\n"
    "  vec3 cyan_hi = vec3(0.15, 0.90, 0.95);\n"
    "  vec3 cyan_lo = vec3(0.05, 0.45, 0.65);\n"
    "  vec3 cyan    = mix(cyan_hi, cyan_lo, v_uv.y);\n"
    "  vec3 glass   = vec3(0.02, 0.10, 0.14) + cyan * (0.22 + u_value*0.15);\n"
    "  vec3 col     = mix(glass, cyan, brd * 0.80);\n"
    "  fragColor    = vec4(col, msk * (inn*0.35 + brd*0.90));\n"
    "}\n";

/* ── Glass Alert (errores) ────────────────────────────────── */
const char *RC_FRAG_GLASS_ALERT =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_uv;\n"
    "uniform vec4  u_rect;\n"
    "uniform float u_radius;\n"
    "uniform float u_time;\n"
    "uniform float u_value;\n"
    "out vec4 fragColor;\n"
    "\n"
    "float sdRoundBox(vec2 p, vec2 b, float r) {\n"
    "  vec2 q = abs(p) - b + vec2(r);\n"
    "  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n"
    "void main() {\n"
    "  vec2  p   = v_uv - 0.5;\n"
    "  float rPx = u_radius / max(u_rect.z, 1.0);\n"
    "  float d   = sdRoundBox(p, vec2(0.5) - vec2(rPx), rPx);\n"
    "  float msk = 1.0 - smoothstep(-0.003, 0.003, d);\n"
    "  float bw  = 0.016;\n"
    "  float brd = 1.0 - smoothstep(-bw*0.5, bw*0.5, d);\n"
    "  float inn = 1.0 - smoothstep(0.0, bw, d + bw*1.5);\n"
    "  float flk = 0.05 * sin(u_time * 8.0);\n"   /* parpadeo sutil */
    "  vec3 red  = mix(vec3(1.0,0.25,0.15), vec3(0.65,0.08,0.05), v_uv.y);\n"
    "  vec3 col  = mix(vec3(0.15,0.02,0.02) + red*(0.22+flk), red, brd*0.85);\n"
    "  fragColor = vec4(col, msk*(inn*0.38 + brd*0.92));\n"
    "}\n";

/* ── Barra de progreso ────────────────────────────────────── */
const char *RC_FRAG_PROGRESS =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_uv;\n"
    "uniform float u_value;\n"   /* progreso [0,1] */
    "uniform float u_time;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  float x    = v_uv.x;\n"
    "  float fill = step(x, u_value);\n"
    "  float edge = abs(x - u_value);\n"
    "  float glow = fill * exp(-edge * 40.0) * 0.6;\n"
    "  vec3 gold  = mix(vec3(1.0,0.85,0.15), vec3(0.75,0.50,0.08), v_uv.y);\n"
    "  vec3 bg    = vec3(0.10, 0.08, 0.04);\n"
    "  float wave = 0.05 * sin(v_uv.x * 18.0 - u_time * 4.0);\n"
    "  vec3 col   = mix(bg, gold + vec3(glow + wave), fill);\n"
    "  float ry   = v_uv.y; float rnd = ry*(1.0-ry)*4.0;\n"
    "  fragColor  = vec4(col, rnd * (fill*0.9 + (1.0-fill)*0.25));\n"
    "}\n";

/* ── Gauge circular ───────────────────────────────────────── */
const char *RC_FRAG_GAUGE =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_uv;\n"
    "uniform float u_value;\n"
    "uniform float u_time;\n"
    "out vec4 fragColor;\n"
    "#define PI 3.14159265359\n"
    "void main() {\n"
    "  vec2  p   = v_uv - 0.5;\n"
    "  float r   = length(p);\n"
    "  float a   = atan(p.y, p.x);\n"
    "  float aN  = mod(a + PI, 2.0*PI) / (2.0*PI);\n"
    "  float ring_in  = 0.36;\n"
    "  float ring_out = 0.48;\n"
    "  float inRing = step(ring_in, r) * step(r, ring_out);\n"
    "  float fill   = step(aN, u_value);\n"
    "  vec3 gold = mix(vec3(1.0,0.85,0.15), vec3(0.75,0.50,0.08), aN);\n"
    "  vec3 bg   = vec3(0.12, 0.09, 0.03);\n"
    "  float glow = exp(-abs(r - (ring_in+ring_out)*0.5) * 30.0);\n"
    "  vec3 col  = mix(bg, gold * (1.0 + glow * 0.4), fill);\n"
    "  fragColor = vec4(col, inRing * (fill*0.9 + (1.0-fill)*0.2));\n"
    "}\n";

/* ══════════════════════════════════════════════════════════════
   CICLO DE VIDA
   ══════════════════════════════════════════════════════════════ */

RigCanvas* rc_init(WsServer *srv) {
    RigCanvas *rc = calloc(1, sizeof(RigCanvas));
    if (!rc) return NULL;
    rc->srv = srv;
    rc->shaders_sent = false;
    rc->dirty = false;
    rc_send_shaders(rc);
    return rc;
}

void rc_free(RigCanvas *rc) {
    if (!rc) return;
    RCWidget *w = rc->widgets;
    while (w) { RCWidget *nx = w->next; free(w); w = nx; }
    free(rc);
}

/* ══════════════════════════════════════════════════════════════
   ENVÍO DE SHADERS
   ══════════════════════════════════════════════════════════════ */

static char* json_escape(const char *s, char *buf, size_t sz) {
    size_t wi = 0;
    buf[0] = '\0';
    for (; *s && wi + 4 < sz; s++) {
        if (*s == '"')  { buf[wi++] = '\\'; buf[wi++] = '"'; }
        else if (*s == '\\') { buf[wi++] = '\\'; buf[wi++] = '\\'; }
        else if (*s == '\n') { buf[wi++] = '\\'; buf[wi++] = 'n'; }
        else if (*s == '\r') { buf[wi++] = '\\'; buf[wi++] = 'r'; }
        else buf[wi++] = *s;
    }
    buf[wi] = '\0';
    return buf;
}

void rc_send_shaders(RigCanvas *rc) {
    if (!rc || !rc->srv) return;

    /* Buffer para escapar shaders (~4 KB por shader) */
    char *esc = malloc(8192);
    if (!esc) return;

#define SEND_SHADER(name, src_str) \
    json_escape((src_str), esc, 8192); \
    ws_broadcastf(rc->srv, \
        "{\"ev\":\"canvas_shader\",\"name\":\"%s\",\"src\":\"%s\"}", \
        (name), esc);

    SEND_SHADER("vert_quad",      RC_VERT_QUAD);
    SEND_SHADER("frag_gold",      RC_FRAG_GLASS_GOLD);
    SEND_SHADER("frag_cyan",      RC_FRAG_GLASS_CYAN);
    SEND_SHADER("frag_alert",     RC_FRAG_GLASS_ALERT);
    SEND_SHADER("frag_progress",  RC_FRAG_PROGRESS);
    SEND_SHADER("frag_gauge",     RC_FRAG_GAUGE);

#undef SEND_SHADER

    free(esc);
    rc->shaders_sent = true;

    ws_broadcastf(rc->srv,
        "{\"ev\":\"canvas_ready\","
         "\"shaders\":5,"
         "\"phi\":1.6180339887498948482}");
}

/* ══════════════════════════════════════════════════════════════
   IMMEDIATE-MODE FRAME
   ══════════════════════════════════════════════════════════════ */

void rc_begin_frame(RigCanvas *rc) {
    if (!rc) return;
    RCWidget *w = rc->widgets;
    while (w) { RCWidget *nx = w->next; free(w); w = nx; }
    rc->widgets   = NULL;
    rc->n_widgets = 0;
    rc->dirty     = false;
}

static RCWidget* rc_add_widget(RigCanvas *rc) {
    if (!rc) return NULL;
    RCWidget *w = calloc(1, sizeof(RCWidget));
    if (!w) return NULL;
    w->enabled  = true;
    w->next     = rc->widgets;
    rc->widgets = w;
    rc->n_widgets++;
    rc->dirty = true;
    return w;
}

bool rc_button(RigCanvas *rc, const char *id, const char *label,
                float x, float y, float w, float h) {
    RCWidget *wgt = rc_add_widget(rc);
    if (!wgt) return false;
    strncpy(wgt->id,    id,    sizeof(wgt->id)-1);
    strncpy(wgt->label, label, sizeof(wgt->label)-1);
    wgt->kind  = RC_BUTTON;
    wgt->x = x; wgt->y = y; wgt->w = w; wgt->h = h;
    wgt->theme = RC_THEME_GOLD;
    wgt->argb  = 0xFFFFD700;
    /* ¿fue pulsado en este frame? */
    return (rc->clicked_id[0] &&
            strncmp(rc->clicked_id, id, sizeof(wgt->id)-1) == 0);
}

void rc_panel(RigCanvas *rc, const char *id, const char *title,
               float x, float y, float w, float h, RCTheme theme) {
    RCWidget *wgt = rc_add_widget(rc);
    if (!wgt) return;
    strncpy(wgt->id,    id,    sizeof(wgt->id)-1);
    strncpy(wgt->label, title, sizeof(wgt->label)-1);
    wgt->kind  = RC_PANEL;
    wgt->x = x; wgt->y = y; wgt->w = w; wgt->h = h;
    wgt->theme = theme;
    wgt->argb  = 0x80404020;
}

void rc_text(RigCanvas *rc, const char *id, const char *text,
              float x, float y, uint32_t argb) {
    RCWidget *wgt = rc_add_widget(rc);
    if (!wgt) return;
    strncpy(wgt->id,    id,   sizeof(wgt->id)-1);
    strncpy(wgt->label, text, sizeof(wgt->label)-1);
    wgt->kind  = RC_TEXT;
    wgt->x = x; wgt->y = y; wgt->w = 0; wgt->h = 0;
    wgt->argb  = argb;
    wgt->theme = RC_THEME_CLEAN;
}

void rc_progress(RigCanvas *rc, const char *id, float val,
                  float x, float y, float w, float h) {
    RCWidget *wgt = rc_add_widget(rc);
    if (!wgt) return;
    strncpy(wgt->id, id, sizeof(wgt->id)-1);
    wgt->kind  = RC_PROGRESS;
    wgt->value = val < 0.0f ? 0.0f : (val > 1.0f ? 1.0f : val);
    wgt->x = x; wgt->y = y; wgt->w = w; wgt->h = h;
    wgt->theme = RC_THEME_GOLD;
    wgt->argb  = 0xFFFFD700;
}

void rc_gauge(RigCanvas *rc, const char *id, const char *label,
               float val, float cx, float cy, float r) {
    RCWidget *wgt = rc_add_widget(rc);
    if (!wgt) return;
    strncpy(wgt->id,    id,    sizeof(wgt->id)-1);
    strncpy(wgt->label, label, sizeof(wgt->label)-1);
    wgt->kind  = RC_GAUGE;
    wgt->value = val < 0.0f ? 0.0f : (val > 1.0f ? 1.0f : val);
    wgt->x = cx - r; wgt->y = cy - r;
    wgt->w = r * 2.0f; wgt->h = r * 2.0f;
    wgt->theme = RC_THEME_GOLD;
    wgt->argb  = 0xFFFFD700;
}

void rc_badge(RigCanvas *rc, const char *id, const char *text,
               float x, float y, RCTheme theme) {
    RCWidget *wgt = rc_add_widget(rc);
    if (!wgt) return;
    strncpy(wgt->id,    id,   sizeof(wgt->id)-1);
    strncpy(wgt->label, text, sizeof(wgt->label)-1);
    wgt->kind  = RC_BADGE;
    wgt->x = x; wgt->y = y;
    wgt->w = (float)(strlen(text) * 8 + 16);
    wgt->h = 22.0f;
    wgt->theme = theme;
}

/* ══════════════════════════════════════════════════════════════
   END FRAME — serializar y emitir por WebSocket
   ══════════════════════════════════════════════════════════════ */

static const char* theme_name(RCTheme t) {
    switch(t) {
    case RC_THEME_GOLD:  return "gold";
    case RC_THEME_CYAN:  return "cyan";
    case RC_THEME_ALERT: return "alert";
    default:             return "clean";
    }
}

static const char* kind_name(RCWidgetKind k) {
    switch(k) {
    case RC_BUTTON:   return "button";
    case RC_PANEL:    return "panel";
    case RC_TEXT:     return "text";
    case RC_PROGRESS: return "progress";
    case RC_GAUGE:    return "gauge";
    case RC_BADGE:    return "badge";
    default:          return "unknown";
    }
}

void rc_end_frame(RigCanvas *rc) {
    if (!rc || !rc->srv || !rc->dirty) return;

    /* Estimar tamaño máximo: 512 bytes por widget */
    size_t bufsz = (size_t)(rc->n_widgets + 1) * 512 + 64;
    char  *buf   = malloc(bufsz);
    if (!buf) return;

    size_t wp = 0;
    wp += (size_t)snprintf(buf + wp, bufsz - wp,
        "{\"ev\":\"canvas_frame\",\"n\":%u,\"widgets\":[",
        rc->n_widgets);

    /* Los widgets se insertaron en orden inverso; iterar así */
    bool first = true;
    for (RCWidget *w = rc->widgets; w && wp + 400 < bufsz; w = w->next) {
        /* Escapar label */
        char esc_label[512] = {0};
        size_t ei = 0;
        for (const char *s = w->label; *s && ei < 508; s++) {
            if (*s == '"' || *s == '\\') esc_label[ei++] = '\\';
            esc_label[ei++] = *s;
        }
        wp += (size_t)snprintf(buf + wp, bufsz - wp,
            "%s{"
            "\"id\":\"%s\","
            "\"kind\":\"%s\","
            "\"x\":%.2f,\"y\":%.2f,\"w\":%.2f,\"h\":%.2f,"
            "\"label\":\"%s\","
            "\"value\":%.4f,"
            "\"argb\":%u,"
            "\"theme\":\"%s\","
            "\"hovered\":%s,"
            "\"pressed\":%s,"
            "\"enabled\":%s"
            "}",
            first ? "" : ",",
            w->id, kind_name(w->kind),
            w->x, w->y, w->w, w->h,
            esc_label,
            w->value,
            w->argb,
            theme_name(w->theme),
            w->hovered  ? "true" : "false",
            w->pressed  ? "true" : "false",
            w->enabled  ? "true" : "false");
        first = false;
    }
    wp += (size_t)snprintf(buf + wp, bufsz - wp, "]}");

    ws_broadcast(rc->srv, buf, wp);
    free(buf);

    /* Limpiar clicked_id (ya fue consumido por rc_button) */
    rc->clicked_id[0] = '\0';
    rc->dirty = false;
}

/* ══════════════════════════════════════════════════════════════
   EVENTOS ENTRANTES DEL FRONTEND
   ══════════════════════════════════════════════════════════════ */

static void ws_json_str_local(const char *json, const char *key,
                               char *out, size_t sz) {
    out[0] = '\0';
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) return;
    p += strlen(search);
    size_t i = 0;
    while (*p && *p != '"' && i < sz - 1) {
        if (*p == '\\' && *(p+1)) { p++; }
        out[i++] = *p++;
    }
    out[i] = '\0';
}

void rc_handle_event(RigCanvas *rc, const char *json_evt) {
    if (!rc || !json_evt) return;

    char ev_type[32] = {0};
    ws_json_str_local(json_evt, "ev", ev_type, sizeof(ev_type));

    if (strcmp(ev_type, "canvas_click") == 0) {
        ws_json_str_local(json_evt, "id",
                          rc->clicked_id, sizeof(rc->clicked_id));
        /* Marcar widget como pressed */
        for (RCWidget *w = rc->widgets; w; w = w->next)
            w->pressed = (strncmp(w->id, rc->clicked_id, sizeof(w->id)-1) == 0);

    } else if (strcmp(ev_type, "canvas_hover") == 0) {
        char hid[64] = {0};
        ws_json_str_local(json_evt, "id", hid, sizeof(hid));
        strncpy(rc->hover_id, hid, sizeof(rc->hover_id)-1);
        for (RCWidget *w = rc->widgets; w; w = w->next)
            w->hovered = (strncmp(w->id, hid, sizeof(w->id)-1) == 0);

    } else if (strcmp(ev_type, "canvas_request_shaders") == 0) {
        rc_send_shaders(rc);
    }
}

/* ══════════════════════════════════════════════════════════════
   ESTADÍSTICAS
   ══════════════════════════════════════════════════════════════ */

void rc_emit_stats(RigCanvas *rc) {
    if (!rc || !rc->srv) return;
    ws_broadcastf(rc->srv,
        "{\"ev\":\"canvas_stats\","
         "\"n_widgets\":%u,"
         "\"shaders_sent\":%s,"
         "\"clicked_id\":\"%s\"}",
        rc->n_widgets,
        rc->shaders_sent ? "true" : "false",
        rc->clicked_id);
}
