/* ============================================================
   RigCom v8.0 — include/rigcanvas.h
   RigCanvas: framework de UI con shaders GLSL.
   API immediate-mode en C → emite comandos de dibujado vía
   WebSocket → frontend renderiza en WebGL 2.0 con shaders
   glassmorphism dorado acelerado por GPU.
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef RIGCANVAS_H
#define RIGCANVAS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "wsserver.h"

/* ── Tipos de widget ───────────────────────────────────────── */
typedef enum {
    RC_BUTTON   = 0,
    RC_PANEL    = 1,
    RC_TEXT     = 2,
    RC_PROGRESS = 3,
    RC_GAUGE    = 4,
    RC_BADGE    = 5
} RCWidgetKind;

/* ── Paleta de temas ───────────────────────────────────────── */
typedef enum {
    RC_THEME_GOLD   = 0,   /* glassmorphism dorado (default)   */
    RC_THEME_CYAN   = 1,   /* glass cian — paneles info        */
    RC_THEME_ALERT  = 2,   /* rojo rubí — errores              */
    RC_THEME_CLEAN  = 3    /* blanco/gris plano — sin shader   */
} RCTheme;

/* ── Descriptor de widget ──────────────────────────────────── */
typedef struct RCWidget {
    char         id[64];
    RCWidgetKind kind;
    float        x, y, w, h;
    char         label[256];
    float        value;      /* [0.0, 1.0] para PROGRESS/GAUGE */
    uint32_t     argb;       /* color base ARGB                */
    RCTheme      theme;
    bool         enabled;
    bool         hovered;    /* actualizado por rc_handle_event */
    bool         pressed;
    struct RCWidget *next;
} RCWidget;

/* ── Estado global del canvas ──────────────────────────────── */
typedef struct {
    WsServer   *srv;
    RCWidget   *widgets;
    uint32_t    n_widgets;
    bool        shaders_sent; /* shaders ya enviados al frontend */
    bool        dirty;
    char        clicked_id[64]; /* último click recibido del WS */
    char        hover_id[64];
} RigCanvas;

/* ── API pública ───────────────────────────────────────────── */

/* Crea instancia y envía shaders GLSL al frontend */
RigCanvas* rc_init    (WsServer *srv);
void       rc_free    (RigCanvas *rc);

/* Envía los programas GLSL al frontend (1 vez es suficiente) */
void       rc_send_shaders (RigCanvas *rc);

/* ── Immediate-mode frame ──────────────────────────────────── */

/* Limpia la lista de widgets del frame anterior */
void rc_begin_frame (RigCanvas *rc);

/* Declara un botón; devuelve true si fue pulsado esta frame */
bool rc_button  (RigCanvas *rc,
                  const char *id, const char *label,
                  float x, float y, float w, float h);

/* Panel decorativo (glassmorphism, no interactivo) */
void rc_panel   (RigCanvas *rc,
                  const char *id, const char *title,
                  float x, float y, float w, float h,
                  RCTheme theme);

/* Etiqueta de texto */
void rc_text    (RigCanvas *rc,
                  const char *id, const char *text,
                  float x, float y, uint32_t argb);

/* Barra de progreso [0.0…1.0] */
void rc_progress(RigCanvas *rc,
                  const char *id, float val,
                  float x, float y, float w, float h);

/* Gauge circular [0.0…1.0] */
void rc_gauge   (RigCanvas *rc,
                  const char *id, const char *label,
                  float val, float cx, float cy, float r);

/* Badge de estado (contador / chip) */
void rc_badge   (RigCanvas *rc,
                  const char *id, const char *text,
                  float x, float y, RCTheme theme);

/* Serializa y envía todos los widgets como un evento WS */
void rc_end_frame (RigCanvas *rc);

/* Procesa un evento WS entrante del frontend
   (canvas_click / canvas_hover) */
void rc_handle_event (RigCanvas *rc, const char *json_evt);

/* Emite estadísticas del canvas */
void rc_emit_stats (RigCanvas *rc);

/* ── Shaders GLSL ES 3.00 (embebidos como strings) ─────────── */
extern const char *RC_VERT_QUAD;          /* vértice genérico    */
extern const char *RC_FRAG_GLASS_GOLD;    /* glassmorphism dorado*/
extern const char *RC_FRAG_GLASS_CYAN;    /* glass cian          */
extern const char *RC_FRAG_GLASS_ALERT;   /* glass rojo          */
extern const char *RC_FRAG_PROGRESS;      /* barra de progreso   */
extern const char *RC_FRAG_GAUGE;         /* gauge circular      */

#endif /* RIGCANVAS_H */
