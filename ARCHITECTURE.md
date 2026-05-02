# RigCom v8.0 — Arquitectura Técnica

## Pipeline completo

```
Input: archivo.c / archivo.rigc
         │
         ▼
┌─────────────────────────────────────────┐
│  PREPROCESADOR (preproc.c)              │
│  • #include <sys> / "local"             │
│  • #define / #undef / macros func-like  │
│  • #if / #ifdef / #ifndef / #elif       │
│  • __FILE__ / __LINE__ / __DATE__       │
│  • Rutas configurables vía rigcom.toml  │
└─────────────────────────────────────────┘
         │  texto expandido
         ▼
┌─────────────────────────────────────────┐
│  LEXER (lexer.c)                        │
│  • Tokenizador C11 / .rigc completo     │
│  • Literales: int, float, string, char  │
│  • Keywords: 43 palabras reservadas     │
│  • Operadores: todos los de C11         │
│  • Peek sin consumir (lexer_peek)       │
└─────────────────────────────────────────┘
         │  stream de tokens
         ▼
┌─────────────────────────────────────────┐
│  PARSER (parser.c + ast.c)             │
│  • Recursivo descendente               │
│  • Lookahead de 2 tokens               │
│  • Recuperación de errores (panic mode)│
│  • Arena allocator para todos los nodos│
│  • Soporte completo: func, struct,     │
│    enum, typedef, for, while, do-while │
└─────────────────────────────────────────┘
         │  AST (ASTNode*)
         ▼
┌─────────────────────────────────────────┐
│  TYPE CHECKER (typechecker.c)           │
│  │                                      │
│  ├── SYMBOL TABLE (symtable.c)          │
│  │   • FNV-1a 32-bit hash              │
│  │   • Scopes anidados (push/pop)       │
│  │   • Lookup walk hasta root           │
│  │                                      │
│  └── Checks:                            │
│      • Símbolos no declarados           │
│      • Tipos incompatibles              │
│      • Redefinición en mismo scope      │
│      • break/continue fuera de bucle    │
│      • Retorno incompatible             │
│      • Conversiones implícitas          │
│      • 50+ builtins registrados         │
└─────────────────────────────────────────┘
         │  AST verificado
         ▼
┌─────────────────────────────────────────┐
│  IR EMISSION (rigir.c)                  │
│  • RigIR: SSA, bloques básicos          │
│  • Virtual registers (vregs)            │
│  • Instrucciones: ADD/SUB/MUL/DIV/MOD  │
│    AND/OR/XOR/SHL/SHR, CMP, ALLOCA     │
│    LOAD/STORE/GEP, CALL, RET, BR       │
│  • Pases: const-fold, DCE, copy-prop   │
└─────────────────────────────────────────┘
         │  RigIR optimizado
         ▼
┌─────────────────┬───────────────────────┐
│  LLVM BACKEND   │  ARM64 NATIVE BACKEND │
│  (backend.c)    │  (backend.c)          │
│  • Emite .ll    │  • Instruction Sel.   │
│  • Invoca clang │  • Linear Scan RegAlloc│
│  • Multi-target │  • x0–x18 (19 regs)  │
│                 │  • Spill a stack      │
│                 │  • Epilogue/Prologue  │
└─────────────────┴───────────────────────┘
         │  .o / .s
         ▼
┌─────────────────────────────────────────┐
│  LINKER                                 │
│  • clang -target ... -lm -lpthread      │
│  • Produce binario final                │
└─────────────────────────────────────────┘
```

## Scheduler paralelo

```
N archivos fuente
      │
      ▼
┌─────────────────────────────────────┐
│  Scheduler (sched.c)                │
│  • Ring-buffer work queue (1024)    │
│  • N worker threads (pthreads)      │
│  • Mutex + cond_work + cond_done    │
│  • sched_submit() → no-blocking     │
│  • sched_wait() → barrier           │
│  • sched_map() → parallel for-each  │
└─────────────────────────────────────┘
```

## WebSocket Dashboard

```
rigcom ui [port]
      │
      ▼
┌─────────────────────────────────────┐
│  WsServer (wsserver.c)              │
│  • select() multiplexado            │
│  • SHA-1 propio (FIPS 180-4)        │
│  • Base64 propio                    │
│  • RFC 6455 frames (mask/unmask)    │
│  • Thread-safe ws_broadcast()       │
└─────────────────────────────────────┘
      │                    │
      ▼                    ▼
HTTP GET /         WebSocket ws://
dashboard.html     on_message handler
                         │
                         ├── "build"    → rigcom_build thread
                         ├── "check"    → rigcom_check thread
                         ├── "status"   → broadcast status
                         └── "stop"     → ws_server_stop
```

## Eventos WebSocket

| Evento | Dirección | Payload |
|--------|-----------|---------|
| `ready` | server→client | `{ev, version, phi, schumann}` |
| `phase` | server→client | `{ev, phase, file, idx, total}` |
| `errors` | server→client | `{ev, errors: [...]}` |
| `file_done` | server→client | `{ev, file, idx, errors, ms}` |
| `build_start` | server→client | `{ev, files, cores, backend}` |
| `build_done` | server→client | `{ev, ok, files, errors, warnings, ms, backend}` |
| `status` | server→client | `{ev, state, version, phi}` |
| `build` | client→server | `{cmd: "build", native: bool}` |
| `check` | client→server | `{cmd: "check", file?: string}` |
| `status` | client→server | `{cmd: "status"}` |
| `stop` | client→server | `{cmd: "stop"}` |

---

**φ = 1.6180339887498948482 · P(A) ∈ {0,1}**
