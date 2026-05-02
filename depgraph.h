/* ============================================================
   RigCom v8.0 — include/depgraph.h
   DepGraph: Grafo de dependencias #include entre .c/.h
   Builds incrementales < 100 ms — detecta ciclos
   API usada por main.c: DGArg / dg_scan_thread
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#ifndef DEPGRAPH_H
#define DEPGRAPH_H

#include <stdbool.h>
#include <stdint.h>
#include "wsserver.h"

typedef struct DGEdge {
    char          to[256];
    struct DGEdge *next;
} DGEdge;

typedef struct DGNode {
    char     path[256];
    char     name[128];
    bool     is_header;
    uint32_t in_degree;
    uint32_t out_degree;
    DGEdge  *deps;
    bool     visited;
    bool     in_cycle;
    struct DGNode *next;
} DGNode;

typedef struct {
    DGNode  *nodes;
    uint32_t n_nodes;
    uint32_t n_edges;
    uint32_t n_cycles;
    bool     has_cycle;
} DepGraph;

DepGraph* dg_scan          (const char *src_dir, const char *inc_dir);
bool      dg_detect_cycles (DepGraph *g);
char**    dg_topo_order    (DepGraph *g, uint32_t *out_n);
void      dg_emit_ws       (DepGraph *g, WsServer *ws);
void      dg_free          (DepGraph *g);

typedef struct {
    WsServer *srv;
    char      src_dir[256];
    char      inc_dir[256];
} DGArg;

void* dg_scan_thread(void *arg);

#endif /* DEPGRAPH_H */
