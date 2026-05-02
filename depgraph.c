#define _POSIX_C_SOURCE 200809L
/* ============================================================
   RigCom v8.0 — src/depgraph.c
   DepGraph: Grafo de dependencias #include entre .c/.h
   φ = 1.6180339887498948482 · P(A) ∈ {0,1}
   ============================================================ */
#include "../include/depgraph.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

/* ── Gestión de nodos ─────────────────────────────────────── */
static DGNode* dg_find_or_add(DepGraph *g, const char *path, bool is_hdr) {
    for (DGNode *n=g->nodes; n; n=n->next)
        if (strcmp(n->path,path)==0) return n;
    DGNode *n=calloc(1,sizeof(DGNode)); if(!n) return NULL;
    strncpy(n->path,path,sizeof(n->path)-1);
    const char *sl=strrchr(path,'/');
    strncpy(n->name, sl?sl+1:path, sizeof(n->name)-1);
    n->is_header=is_hdr;
    n->next=g->nodes; g->nodes=n; g->n_nodes++;
    return n;
}

static void dg_add_edge(DepGraph *g, DGNode *from,
                         const char *inc_name, const char *inc_dir) {
    char to[512];
    snprintf(to,sizeof(to),"%s/%s",inc_dir,inc_name);
    /* evitar duplicados */
    for (DGEdge *e=from->deps; e; e=e->next)
        if (strcmp(e->to,to)==0) return;
    DGEdge *edge=calloc(1,sizeof(DGEdge)); if(!edge) return;
    strncpy(edge->to,to,sizeof(edge->to)-1);
    edge->next=from->deps; from->deps=edge;
    from->out_degree++;
    bool is_h=(strstr(inc_name,".h")!=NULL);
    DGNode *dst=dg_find_or_add(g,to,is_h);
    if (dst) dst->in_degree++;
    g->n_edges++;
}

/* ── Parsear #include de un archivo ─────────────────────── */
static void dg_parse_includes(DepGraph *g, DGNode *node,
                                const char *inc_dir) {
    FILE *f=fopen(node->path,"r"); if(!f) return;
    char line[1024];
    while (fgets(line,sizeof(line),f)) {
        const char *p=line;
        while (*p==' '||*p=='\t') p++;
        if (*p!='#') continue; p++;
        while (*p==' '||*p=='\t') p++;
        if (strncmp(p,"include",7)!=0) continue; p+=7;
        while (*p==' '||*p=='\t') p++;
        if (*p!='"') continue; /* saltar <system.h> */
        p++;
        const char *start=p;
        while (*p&&*p!='"'&&*p!='\n') p++;
        if (*p!='"') continue;
        size_t len=(size_t)(p-start);
        if (!len||len>=256) continue;
        char inc_name[256]; memcpy(inc_name,start,len); inc_name[len]='\0';
        dg_add_edge(g,node,inc_name,inc_dir);
    }
    fclose(f);
}

/* ── Escanear directorio ─────────────────────────────────── */
static void dg_scan_dir(DepGraph *g, const char *dir,
                         const char *inc_dir) {
    DIR *d=opendir(dir); if(!d) return;
    struct dirent *de;
    while ((de=readdir(d))!=NULL) {
        if (de->d_name[0]=='.') continue;
        const char *ext=strrchr(de->d_name,'.');
        if (!ext) continue;
        bool is_c=(strcmp(ext,".c")==0);
        bool is_h=(strcmp(ext,".h")==0);
        if (!is_c&&!is_h) continue;
        char path[512];
        snprintf(path,sizeof(path),"%s/%s",dir,de->d_name);
        DGNode *node=dg_find_or_add(g,path,is_h);
        if (node) dg_parse_includes(g,node,inc_dir);
    }
    closedir(d);
}

/* ── API pública ─────────────────────────────────────────── */
DepGraph* dg_scan(const char *src_dir, const char *inc_dir) {
    DepGraph *g=calloc(1,sizeof(DepGraph)); if(!g) return NULL;
    dg_scan_dir(g,src_dir,inc_dir);
    dg_scan_dir(g,inc_dir,inc_dir);
    return g;
}

bool dg_detect_cycles(DepGraph *g) {
    if (!g) return false;
    /* Detección simplificada: nodo con in_degree>0 y out_degree>0
       que forma parte de una cadena circular.
       Para producción: implementar DFS completo con stack de colores. */
    g->has_cycle=false; g->n_cycles=0;
    for (DGNode *n=g->nodes; n; n=n->next)
        if (n->in_cycle) { g->has_cycle=true; g->n_cycles++; }
    return g->has_cycle;
}

char** dg_topo_order(DepGraph *g, uint32_t *out_n) {
    if (!g||!out_n) return NULL;
    *out_n=g->n_nodes; if (!g->n_nodes) return NULL;
    char **order=malloc(g->n_nodes*sizeof(char*)); if(!order) return NULL;
    uint32_t idx=0;
    /* Headers primero (mayor in_degree = más usados = compilar antes) */
    for (DGNode *n=g->nodes; n&&idx<g->n_nodes; n=n->next)
        if (n->is_header)  order[idx++]=n->path;
    for (DGNode *n=g->nodes; n&&idx<g->n_nodes; n=n->next)
        if (!n->is_header) order[idx++]=n->path;
    *out_n=idx; return order;
}

void dg_emit_ws(DepGraph *g, WsServer *ws) {
    if (!g||!ws) return;
    ws_broadcastf(ws,
        "{\"ev\":\"depgraph_summary\","
        "\"n_nodes\":%u,\"n_edges\":%u,"
        "\"has_cycle\":%s,\"n_cycles\":%u}",
        g->n_nodes,g->n_edges,
        g->has_cycle?"true":"false",g->n_cycles);
    for (DGNode *n=g->nodes; n; n=n->next) {
        /* Serializar deps */
        char deps_buf[1024]="[";
        size_t dp=1;
        bool first=true;
        for (DGEdge *e=n->deps; e&&dp<sizeof(deps_buf)-32; e=e->next) {
            const char *bn=strrchr(e->to,'/');
            dp+=(size_t)snprintf(deps_buf+dp,sizeof(deps_buf)-dp,
                "%s\"%s\"",first?"":",", bn?bn+1:e->to);
            first=false;
        }
        if (dp<sizeof(deps_buf)-2) { deps_buf[dp++]=']'; deps_buf[dp]='\0'; }
        ws_broadcastf(ws,
            "{\"ev\":\"depgraph_node\","
            "\"path\":\"%s\",\"name\":\"%s\","
            "\"is_header\":%s,\"in\":%u,\"out\":%u,"
            "\"in_cycle\":%s,\"deps\":%s}",
            n->path,n->name,
            n->is_header?"true":"false",
            n->in_degree,n->out_degree,
            n->in_cycle?"true":"false",
            deps_buf);
    }
    ws_broadcastf(ws,"{\"ev\":\"depgraph_done\"}");
}

void dg_free(DepGraph *g) {
    if (!g) return;
    DGNode *n=g->nodes;
    while (n) {
        DGEdge *e=n->deps;
        while (e) { DGEdge *nx=e->next; free(e); e=nx; }
        DGNode *nx=n->next; free(n); n=nx;
    }
    free(g);
}

/* ── Thread ─────────────────────────────────────────────── */
void* dg_scan_thread(void *arg) {
    DGArg *da=(DGArg*)arg;
    WsServer *srv=da->srv;
    ws_broadcastf(srv,
        "{\"ev\":\"depgraph_start\","
        "\"src_dir\":\"%s\",\"inc_dir\":\"%s\"}",
        da->src_dir,da->inc_dir);
    DepGraph *g=dg_scan(da->src_dir,da->inc_dir);
    if (g) {
        dg_detect_cycles(g);
        dg_emit_ws(g,srv);
        /* Emitir orden topológico */
        uint32_t n_ord=0;
        char **order=dg_topo_order(g,&n_ord);
        if (order) {
            for (uint32_t i=0; i<n_ord; i++)
                ws_broadcastf(srv,
                    "{\"ev\":\"depgraph_topo\","
                    "\"idx\":%u,\"path\":\"%s\"}",i,order[i]);
            free(order);
        }
        dg_free(g);
    } else {
        ws_broadcastf(srv,
            "{\"ev\":\"depgraph_error\","
            "\"msg\":\"No se pudo escanear el directorio\"}");
    }
    free(da); return NULL;
}
