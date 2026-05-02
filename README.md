# RigCom v8.0 — OMEGA BUILD

**Plataforma de Compilación Industrial para ARM64 · Honor 400 · Termux**

φ = 1.6180339887498948482 · P(A) ∈ {0,1}
Autores: Richard Felipe Urbina (Yon) + Edgar José Gabriel Mora (Gabo)

---

## Novedades v7.0

### ptrace Debugger (`ptrace_dbg.c`)
GDB embebido en el Dashboard. Conecta directamente al kernel via `ptrace()`:
- Lee/escribe registros ARM64 completos: `x0–x30`, `sp`, `pc`, `pstate`
- Lee/escribe registros NEON: `v0–v31` (128 bits cada uno)
- Modifica variables en memoria en caliente mientras el proceso corre
- Breakpoints por dirección (`dbg_attach`) o por nombre de símbolo (`dbg_cmd`)
- Stack unwinding, variables locales, watchpoints de memoria
- Comandos WS: `dbg_attach {pid}` · `dbg_launch {exe}` · `dbg_cmd {cmd}`

### HoloTrace (`holo_trace.c`)
Portal 3D conectado al DAP. Los nodos del grafo 3D del dashboard **brillan** cuando
una función es ejecutada:
- Grafo de llamadas con decay de energía (Schumann decay ≈ 7.83 Hz)
- Emite eventos `holo_glow` y `holo_graph` al frontend WebGL
- Comandos WS: `holo_start {pid}` · `holo_graph` · `holo_line {line}`

### NeonForge (`neon_forge.c`)
Auto-vectorización SIMD real sobre el IR de RigCom:
- Detecta bucles matemáticos vectorizables en el IR
- Emite instrucciones ARM64 NEON (`vld1q/vmulq/vaddq`), registros `v0–v31`
- Genera `build/riglib_neon.h` con macros SIMD portables
- Speedup teórico: **4x floats / 8x int16** por ciclo de CPU
- Comando WS: `neon_forge {file}` · Evento respuesta: `neon_forge_result`

### JNI-Zero (`jni_zero.c`)
Genera automáticamente el glue JNI desde anotaciones en C puro:
```c
// Escribe esto en tu archivo .c:
[[rigcom::export]] int suma(int a, int b) { return a + b; }
```
RigCom genera:
- `build/_JNI_GLUE.c` — wrappers con firma JNI correcta
- `build/RigBridge.java` — clase Java que invoca las funciones C
- Comando WS: `jni_zero_scan {file, pkg, out_dir}`

### APK v7 (`apkpack.c`)
- **Manifest v2**: `minSdk=24`, `targetSdk=34`, `extractNativeLibs=true`, icono adaptativo
- **Firma V1+V2+V3**: RSA-4096, keystore persistente `rigcom_master.keystore`
- **Iconos adaptativos**: 5 densidades (mdpi→xxxhdpi), generados por Python/ImageMagick
- **Multi-ABI**: `arm64-v8a` + `armeabi-v7a` + `x86_64`
- **APK Explorer**: listar, leer, editar y reempaquetar APKs

---

## Build

```bash
# En Termux / UserLand Ubuntu
cd RIGCOM_V8
make                    # Build estándar
make install-termux     # Instalar en $PREFIX/bin
make SAFESTACK=1        # Con dual-stack protection
```

## Comandos WebSocket v7.0

| Comando           | Parámetros                  | Descripción                        |
|-------------------|-----------------------------|------------------------------------|
| `dbg_attach`      | `{pid: N}`                  | Adjuntar ptrace a proceso          |
| `dbg_launch`      | `{exe: "path"}`             | Lanzar ejecutable bajo ptrace      |
| `dbg_cmd`         | `{cmd: "...", ...}`         | Comando de debugger (step, bp, ...) |
| `holo_start`      | `{pid: N}`                  | Iniciar traza holográfica          |
| `holo_graph`      | —                           | Emitir snapshot del grafo 3D       |
| `holo_line`       | `{line: "stdout_line"}`     | Parsear línea de stdout para traza |
| `neon_forge`      | `{file: "src.c"}`           | Auto-vectorizar con NEON           |
| `jni_zero_scan`   | `{file, pkg, out_dir}`      | Generar glue JNI + RigBridge.java  |
| `build_apk`       | `{so, apk}`                 | Empaquetar APK V1/V2/V3            |

---

## Arquitectura v7.0

```
RigCom v8.0
├── src/
│   ├── main.c              ← CLI + WS dispatcher (v7: 5 nuevos comandos)
│   ├── apkpack.c           ← APK v2 Manifest · V1/V2/V3 · Iconos  [NUEVO]
│   ├── ptrace_dbg.c        ← GDB embebido: ptrace ARM64            [NUEVO]
│   ├── holo_trace.c        ← Portal 3D live trace                  [NUEVO]
│   ├── neon_forge.c        ← Auto-SIMD NeonForge                   [NUEVO]
│   ├── jni_zero.c          ← JNI-Zero C→Java interop               [NUEVO]
│   └── ... (25 módulos v6 intactos)
└── include/
    ├── ptrace_dbg.h / holo_trace.h / neon_forge.h / jni_zero.h    [NUEVO]
    └── apkpack.h           ← API extendida con apk_gen_icons       [ACTUALIZADO]
```
