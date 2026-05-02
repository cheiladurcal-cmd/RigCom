# RigCom ROADMAP — v8.0 → v9.0

**Estado:** v8.0 OMEGA BUILD (Estable)  
**Siguiente:** v9.0 NEXUS (Q2 2026)  
**Visión Final:** v10.0 SINGULARITY (2027)

---

## 📋 Tabla de Contenidos

1. [v8.0 Status (Actual)](#v80-status)
2. [v9.0 Roadmap (10 semanas)](#v90-roadmap)
3. [v10.0 Vision (Largo plazo)](#v100-vision)
4. [Métricas de Éxito](#métricas-de-éxito)
5. [Stack Tecnológico](#stack-tecnológico)

---

## v8.0 Status

### ✅ Completado

| Feature | Módulo | Estado |
|---------|--------|--------|
| **Compilación C11** | `lexer.c`, `parser.c`, `ast.c` | ✅ STABLE |
| **Type Checking** | `typechecker.c`, `symtable.c` | ✅ STABLE |
| **RigIR (SSA)** | `rigir.c` | ✅ STABLE |
| **Backend LLVM** | `backend.c` | ✅ STABLE |
| **Backend ARM64** | `backend.c` | ✅ STABLE |
| **Optimizaciones** | `gvn.c` (DCE, const-fold) | ⚠️ BASIC |
| **WebSocket Dashboard** | `wsserver.c` + `dashboard.html` | ✅ STABLE |
| **Oracle (AST)** | `oracle_ip.c` | ⚠️ TEXT-BASED |
| **ptrace Debugger** | `ptrace_dbg.c` | ⚠️ STUB |
| **APK Builder** | `apkpack.c` | ✅ STABLE |
| **RigBridge (P2P)** | `rigbridge.c` | ⚠️ BASIC |
| **NeonForge (SIMD)** | `neon_forge.c` | ⚠️ BASIC |
| **JNI-Zero** | `jni_zero.c` | ✅ STABLE |

### ⚠️ En Progreso / Mejorable

- Instruction Selection (muy genérico)
- GVN (solo copy-prop básico)
- Float Backend (sin d0-d31 nativo)
- Oracle inter-procedural (análisis de texto)
- Debugger real (solo framework)
- Terminal embebida (stub)
- Memory hardening (sin guard pages)
- Sandbox RigBridge (sin restricciones)

---

## v9.0 Roadmap

### 🎯 Objetivo Principal

**"Advanced Compiler Optimizations + Intelligent Analysis + Distributed Safety"**

Convertir RigCom en un compilador competitivo con optimizaciones del nivel de LLVM, análisis de seguridad inter-procedural y red distribuida resiliente.

---

### FASE 1: Instruction Selection & Pattern Matching (Semanas 1-2)

**Objetivo:** Pasar de 1:1 IR→ASM a detección de patrones ARM64-específicos.

#### Tareas

| # | Tarea | Dificultad | Est. Horas | Responsable |
|---|-------|-----------|-----------|-------------|
| 1.1 | Diseñar tabla de patrones (MADD, SHL+ADD, etc) | 🟢 Fácil | 4 | Yon |
| 1.2 | Implementar `ir_pattern_match()` | 🟡 Medio | 8 | Yon |
| 1.3 | Integrar con `backend.c` | 🟡 Medio | 6 | Yon |
| 1.4 | Escribir tests de cobertura | 🟢 Fácil | 4 | Gabo |
| 1.5 | Benchmarks: ganancia % en temps compile | 🟡 Medio | 6 | Gabo |

**Archivos a crear/modificar:**
```c
// include/instr_select.h (NUEVO)
typedef struct {
    IROpKind left, right;
    IROpKind result;
    const char *arm64_instr;
    int savings;  // cuántas instrucciones ahorras
} PatternRule;

extern PatternRule g_patterns[];
extern int g_n_patterns;

// src/backend.c (MODIFICAR)
// Añadir: static void emit_arm64_with_patterns(IRModule *m)

// tests/test_instr_select.c (NUEVO)
void test_madd_pattern(void);
void test_shift_add(void);
```

**Métricas de Éxito:**
- [ ] ≥ 20 patrones registrados
- [ ] 5-15% reducción en instrucciones generadas
- [ ] 0 regresiones en tests existentes
- [ ] Documentación de cada patrón

---

### FASE 2: Global Value Numbering Completo (Semanas 3-4)

**Objetivo:** Eliminar cálculos redundantes que el DCE actual no ve.

#### Tareas

| # | Tarea | Dificultad | Est. Horas |
|---|-------|-----------|-----------|
| 2.1 | Tabla hash de expresiones (SSA operands) | 🟡 Medio | 8 |
| 2.2 | `gvn_compute_canonical_form()` | 🟡 Medio | 10 |
| 2.3 | Integración con `ir_pass_dce()` | 🟡 Medio | 6 |
| 2.4 | Tests: redundancy detection | 🟢 Fácil | 5 |
| 2.5 | Benchmark vs LLVM O2 | 🟡 Medio | 6 |

**Formato de Expresión Canónica:**
```c
// Antes de GVN:
// x = a + b
// y = b + a   ← diferente en orden, mismo valor
// z = y + x

// Después de GVN:
// x = a + b
// y = x       ← reutiliza x (forma canónica)
// z = x + x   ← constante evalable a 2*x
```

**Métricas de Éxito:**
- [ ] Detectar ≥ 90% de redundancias comunes
- [ ] No afectar correctitud (tests passing)
- [ ] Benchmarks: 3-8% speedup en código real

---

### FASE 3: Punto Flotante Nativo (Semanas 5-6)

**Objetivo:** Usar d0-d31 (registros NEON) para aritmética floating-point nativa.

#### Tareas

| # | Tarea | Dificultad | Est. Horas |
|---|-------|-----------|-----------|
| 3.1 | Detectar `double`/`float` en IR | 🟢 Fácil | 3 |
| 3.2 | Allocador de registros d0-d31 | 🟡 Medio | 10 |
| 3.3 | Generador de instrucciones FP | 🟡 Medio | 12 |
| 3.4 | Conversión int↔double (SCVTF/FCVTZS) | 🟡 Medio | 4 |
| 3.5 | Tests de precisión | 🟢 Fácil | 5 |
| 3.6 | Benchmarks: doble vs genérico | 🟡 Medio | 4 |

**Instrucciones Soportadas:**
```arm64
FADDD d0, d0, d1   // Add double
FSUBD d0, d0, d1   // Sub double
FMULD d0, d0, d1   // Mul double
FDIVD d0, d0, d1   // Div double
FMAXD d0, d0, d1   // Max double
FSQRTD d0, d0      // Sqrt double
FABSD d0, d0       // Abs double
SCVTF d0, x0       // Int→Double
FCVTZS x0, d0      // Double→Int
```

**Métricas de Éxito:**
- [ ] 100% de `double` compilado a FP nativo
- [ ] Precisión IEEE 754 (tests)
- [ ] 20-40% speedup en FP loops vs genérico

---

### FASE 4: Oracle Inter-procedural Real (Semanas 7-8)

**Objetivo:** Rastrear variables desde creación hasta liberación, multi-archivo.

#### Arquitectura

```
┌──────────────────────────────────────────────┐
│ ORACLE v2 — Inter-procedural                 │
├──────────────────────────────────────────────┤
│                                              │
│ ┌─ File 1: main.c                           │
│ │  ptr = malloc(100)                         │
│ │  process_data(ptr)     ← seguir llamada    │
│ │                                            │
│ └─ File 2: util.c                           │
│    void process_data(void *p) {              │
│      helper(p)           ← seguir llamada    │
│      free(p)  ✓ liberado aquí               │
│    }                                         │
│                                              │
│ Resultado: ✓ NO hay leak                     │
│                                              │
└──────────────────────────────────────────────┘
```

#### Tareas

| # | Tarea | Dificultad | Est. Horas |
|---|-------|-----------|-----------|
| 4.1 | Call graph generator (multi-archivo) | 🟡 Medio | 10 |
| 4.2 | Escape analysis (¿dónde escapa ptr?) | 🔴 Difícil | 16 |
| 4.3 | Pointer tracking (store→load) | 🔴 Difícil | 12 |
| 4.4 | Validación con AST (no strings) | 🟡 Medio | 8 |
| 4.5 | Cache inter-procedural (SHA-256) | 🟡 Medio | 6 |
| 4.6 | Tests: casos complejos | 🔴 Difícil | 12 |
| 4.7 | Dashboard: visualización del grafo | 🟡 Medio | 8 |

**Estructura de Datos:**

```c
// include/oracle_v2.h
typedef struct {
    const char *file;
    uint32_t line;
    const char *var_name;
    enum { ALLOC, FREE, PARAM, ESCAPE, UNKNOWN } kind;
} VariableEvent;

typedef struct {
    VariableEvent *events;
    uint32_t n_events;
    bool has_leak;
    const char *leak_reason;
} VariableTrace;

typedef struct {
    const char **files;
    uint32_t n_files;
    VariableTrace *traces;
    uint32_t n_traces;
} OracleContext;
```

**Métricas de Éxito:**
- [ ] Rastrear variables multi-archivo
- [ ] 0% falsos positivos en suite de tests
- [ ] Análisis completa en < 500ms para 50 archivos
- [ ] Dashboard muestra grafo de escape

---

### FASE 5: Debugger ptrace Completo (Semanas 9-10)

**Objetivo:** Breakpoints, watchpoints, call stack, variables locales.

#### Tareas

| # | Tarea | Dificultad | Est. Horas |
|---|-------|-----------|-----------|
| 5.1 | Framework ptrace (ya existe: `ptrace_dbg.c`) | ✅ Hecho | 0 |
| 5.2 | Manejo de SIGTRAP en breakpoints | 🟡 Medio | 8 |
| 5.3 | Step-over / Step-into | 🟡 Medio | 6 |
| 5.4 | Call stack unwinding (CFI) | 🔴 Difícil | 12 |
| 5.5 | Variables locales (DWARF parsing) | 🔴 Difícil | 14 |
| 5.6 | Terminal embebida (PTY) | 🟡 Medio | 10 |
| 5.7 | UI: panel de debugger en Dashboard | 🟡 Medio | 12 |
| 5.8 | Tests: debug simple.c, struct.c | 🟢 Fácil | 6 |

**Comandos DAP Soportados:**
```
CMD_LAUNCH {exe, args, cwd}
CMD_ATTACH {pid}
CMD_SET_BREAKPOINT {file, line}
CMD_CONTINUE
CMD_STEP_OVER / STEP_INTO
CMD_READ_REGISTERS
CMD_READ_MEMORY {addr, size}
CMD_READ_LOCAL_VARIABLES
CMD_CALL_STACK
CMD_TERMINATE
```

**Métricas de Éxito:**
- [ ] Breakpoints funcionales (10+ test cases)
- [ ] Call stack legible (≥ 50 frames)
- [ ] Variables locales correctas (struct/array/ptr)
- [ ] Interfaz intuititiva en Dashboard

---

### FASE 6: RigBridge Inteligente (Semanas 9-10, paralelo)

**Objetivo:** Caching distribuido + load balancing por temperatura.

#### A. Shared Object Caching

```
Peer A compila main.c → SHA256: abc123def456
  ↓ almacena en .rigcache/abc123def456.o
  
Peer B pide compilar main.c (idéntico)
  ↓ calcula SHA256: abc123def456
  ↓ RigCache: ¿abc123def456 ∈ red? SÍ
  ↓ descarga desde peer A (sin compilar)
  
Resultado: 95% menos tráfico + 10x más rápido
```

**Tareas:**
- [ ] SHA-256 per-file caching
- [ ] Validación de integridad (CRC32 extra)
- [ ] TTL configurable (por defecto 7 días)
- [ ] Eviction policy (LRU)
- [ ] Tests: multi-peer scenario

#### B. Load Balancing por Temperatura

```c
// En cada peer:
int temp = read_thermal();  // °C
int battery = read_battery(); // %

// Reglas:
if (temp > 70°C || battery < 20%) {
    respond(RIGBRIDGE_BUSY);
    // otros peers no envían trabajo
}

// Dashboard muestra:
// 🟢 peer1: 45°C · 85% battery
// 🟡 peer2: 68°C · 15% battery (THROTTLED)
// 🔴 peer3: Offline
```

**Tareas:**
- [ ] `rigbridge_get_health()` (temp, battery)
- [ ] Loadbalancer respeta health
- [ ] Métricas en Dashboard
- [ ] Historial de throttling
- [ ] Tests: temperatura simulada

---

## v10.0 Vision

### 🎆 Objetivo a Largo Plazo

**"Self-Optimizing Distributed Compiler Network"**

RigCom como plataforma de compilación de próxima generación: auto-optimización basada en ML, red resiliente y análisis de seguridad del nivel enterprise.

#### Características Propuestas

| Característica | Descripción | Estimado |
|---|---|---|
| **ML-based Optimization** | Modelo entrenado predice qué optimizaciones aplicar | v10.0 Q4 2026 |
| **Hardware-Aware Backend** | Detecta CPU (Snapdragon/Exynos) y optimiza | v10.0 Q4 2026 |
| **Formal Verification** | Verifica correctitud de transformaciones IR | v10.1 2027 |
| **Distributed Cache Network** | Blockchain-based integrity (opcional) | v10.1 2027 |
| **Custom DSL (RigScript++)** | Lenguaje optimizado para ARM64 | v10.0 Q4 2026 |
| **IDE Integration** | VSCode / JetBrains plugins | v10.0 Q4 2026 |

---

## Métricas de Éxito

### v9.0 KPIs

| Métrica | Objetivo | Baseline (v8.0) |
|---------|----------|-----------------|
| **Compile Speed (LLVM backend)** | < 1s para 50K loc | 2-3s |
| **Compile Speed (ARM64 native)** | < 2s para 50K loc | 3-5s |
| **Code Quality (vs LLVM O2)** | ≥ 90% parity | ~70% |
| **Memory Efficiency** | ≤ 512MB para 100K loc | 800MB |
| **Test Coverage** | ≥ 85% | 72% |
| **Bug Reports / Month** | ≤ 5 | N/A |
| **Build Time Reduction (RigBridge)** | ≥ 40% en red | baseline |
| **Oracle False Positives** | ≤ 2% | 8-12% |
| **Debugger Frame Rate (Dashboard)** | ≥ 60 FPS | 30 FPS |

---

## Stack Tecnológico

### Dependencias (v9.0)

```
RigCom v9.0
├── Compiler
│   ├── LLVM 16+ (backend opcional)
│   └── clang 14+ (linker)
├── Runtime
│   ├── glibc 2.31+ (Linux)
│   ├── libpthread (POSIX)
│   └── libm (math)
├── Network
│   ├── socket API (RFC 6455 WebSocket)
│   └── ZeroMQ (opcional, para mesh)
└── Debug
    ├── ptrace (Linux/Android)
    └── DWARF 4 (debug symbols)
```

### Arquitectura de Código

```
src/ (28 módulos, ~750KB)
├── Parsers (3)
│   ├── frontend_c.c        (C11)
│   ├── rigscript.c         (RigScript)
│   └── ...
├── Optimizations (5)
│   ├── gvn.c               (value numbering)
│   ├── instr_select.c      (NEW v9.0)
│   ├── ...
├── Backend (2)
│   ├── backend.c           (LLVM + ARM64)
│   └── ...
├── Runtime (8)
│   ├── ptrace_dbg.c        (debugger)
│   ├── pty_term.c          (terminal)
│   ├── rigbridge.c         (P2P)
│   ├── rigcache.c          (cache)
│   └── ...
└── Utilities (10)
    ├── wsserver.c          (WebSocket)
    ├── error.c             (diagnostics)
    ├── ...
```

---

## Timeline Estimado

```
┌─────────────────────────────────────────────────────────────────┐
│ 2026 Q2 (Mayo-Junio)                                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Semana 1-2: Instruction Selection                              │
│  ████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ 20%                  │
│                                                                 │
│  Semana 3-4: GVN Completo                                       │
│  ░░░░████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ 40%                  │
│                                                                 │
│  Semana 5-6: Float Backend + Debugger (paralelo)                │
│  ░░░░░░░░████░░░░░░░░░░░░░░░░░░░░░░░░░░░ 60%                  │
│                                                                 │
│  Semana 7-8: Oracle v2 + RigBridge                              │
│  ░░░░░░░░░░░░████░░░░░░░░░░░░░░░░░░░░░░░ 80%                  │
│                                                                 │
│  Semana 9-10: Integration + Docs + Release                      │
│  ░░░░░░░░░░░░░░░░████░░░░░░░░░░░░░░░░░░░ 100%                 │
│                                                                 │
│  📅 Lanzamiento: Finales de Junio 2026                          │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## Contribuciones Bienvenidas

Para contribuir a alguna de estas fases:

1. 👀 Revisa [CONTRIBUTING.md](./CONTRIBUTING.md)
2. 🎯 Elige una tarea del roadmap
3. 💬 Comenta en [Discussions](https://github.com/cheiladurcal-cmd/RigCom/discussions)
4. 🔧 Implementa + tests
5. 📝 Abre PR con descripción clara

---

## FAQ

**P: ¿Puedo trabajar en múltiples fases?**  
R: Sí, pero prioriza Fase 1-2 primero (fundamentales).

**P: ¿Cuál es la prioridad?**  
R: Instruction Selection > GVN > Float > Oracle > Debugger

**P: ¿Hay financiamiento?**  
R: No en este momento, es open-source comunitario.

**P: ¿Dónde reporto bugs?**  
R: [Issues](https://github.com/cheiladurcal-cmd/RigCom/issues) o [conduct@rigcom.dev](mailto:conduct@rigcom.dev)

---

**RigCom v8.0 OMEGA BUILD**  
*φ = 1.6180339887498948482 · P(A) ∈ {0,1}*

Última actualización: 2026-05-02
