# RigCom v8.0 — Roadmap Técnico

## Visión

Compilador C de grado industrial, completo, con frontend 100% propio,
preprocesador autónomo, IR intermedia propia (RigIR SSA), backend dual adaptativo
(LLVM + ARM64 nativo), type-checking integrado, dashboard WebSocket comercial
embebido, errores en español, scheduler N-core, y bootstrapping completo.

---

## Estado de implementación

### Fase 1 — v3.0 ✅ Base funcional

| Tarea | Estado |
|-------|--------|
| Estructura modular (include/ + src/) | ✅ |
| RigCtx + memory pool (arena allocator) | ✅ |
| Sistema de errores con sugerencias | ✅ |
| CLI: build · check · run · ui | ✅ |
| RigIR: bloques, SSA, LLVM + ARM64 export | ✅ |
| Dashboard UI glassmorphism | ✅ |
| Makefile + rigcom.toml | ✅ |

---

### Fase 2 — v3.1 ✅ Análisis semántico real

| Tarea | Estado |
|-------|--------|
| Lexer: tokenizar `.c` / `.rigc` | ✅ |
| Parser: construir AST recursivo descendente | ✅ |
| Symbol table (SymTable) con scopes anidados + FNV-1a hash | ✅ |
| Type checker: detectar errores de tipo | ✅ |
| Errores semánticos con puntero a línea + sugerencia | ✅ |
| TOML parser autónomo | ✅ |

---

### Fase 3 — v3.3 ✅ Preprocesador autónomo

| Tarea | Estado |
|-------|--------|
| Resolver `#define` / `#undef` (object-like + function-like) | ✅ |
| Resolver `#include <sys>` y `"local"` | ✅ |
| Directivas condicionales: `#if` / `#ifdef` / `#ifndef` / `#elif` / `#else` / `#endif` | ✅ |
| Macros predefinidos: `__FILE__` / `__LINE__` / `__DATE__` / `defined()` | ✅ |
| Motor de expansión de macros función-like con parámetros | ✅ |
| Rutas de búsqueda configurables desde rigcom.toml | ✅ |
| Integración en pipeline antes del lexer | ✅ |

---

### Fase 4 — v3.5 ✅ Infraestructura completa

| Tarea | Estado |
|-------|--------|
| Scheduler pthreads N-core (ring-buffer work queue) | ✅ |
| ARM64 Instruction Selector | ✅ |
| Register Allocator (Linear Scan, x0–x18) | ✅ |
| WebSocket Server RFC 6455 (SHA-1 propio · Base64 propio) | ✅ |
| Pipeline WS live: fases en tiempo real (preprocess → lex → parse → typecheck) | ✅ |

---

### Fase 5 — v4.0 ✅ Bootstrapping + CLI completo

| Tarea | Estado |
|-------|--------|
| CLI completo: build · check · run · ui · bootstrap · bench · info | ✅ |
| Pipeline full: Preproc → Lex → Parse → AST → TypeCheck → RigIR → Backend | ✅ |
| WebSocket bridge en pipeline (eventos por fase, por archivo) | ✅ |
| rigcom_ui con WS server real RFC 6455 (sin python) | ✅ |
| Bootstrap: RigCom compila su propio frontend | ✅ |
| Benchmark integrado (3 runs: cold · warm · ARM64) | ✅ |
| rigcom info: info de sistema completa | ✅ |

---

## Stack técnico

| Componente | Tecnología |
|------------|------------|
| Lenguaje frontend | C11 (sin deps externas) |
| Preprocesador | RFC-compatible, implementación propia |
| Backend | LLVM 15+ C API + ARM64 nativo |
| WebSocket | RFC 6455, SHA-1 propio, Base64 propio |
| Paralelismo | pthreads |
| UI | HTML5 + CSS3 + WebSocket |
| Build | Make |
| Tests | C11 unit tests |
| Target primario | aarch64-linux-android (Snapdragon 7 Gen 3) |
| Config | TOML (parser propio) |

---

## Evolución v2.x → v4.0

| v2.x (orquestador) | v3.0 | v4.0 |
|--------------------|------|------|
| `system("clang ...")` | libLLVM en memoria | Pipeline completo integrado |
| Sin preprocesador | Sin preprocesador | Preprocesador autónomo |
| Sin type-checking | Semantic analyzer base | Type checker operacional |
| Sin IR intermedia | RigIR (SSA) | RigIR + pases de optimización |
| Compilación secuencial | Scheduler N-core | Scheduler N-core + work queue |
| Headers manuales | Headers sintéticos | Preprocesador completo |
| Backend único (clang) | Backend dual (LLVM + ARM64) | Backend dual + regalloc |
| UI via python HTTP | WebSocket base | WebSocket RFC 6455 propio |
| Sin bootstrap | Sin bootstrap | Bootstrap: self-compilation |

---

**φ = 1.6180339887498948482 · P(A) ∈ {0,1}**
