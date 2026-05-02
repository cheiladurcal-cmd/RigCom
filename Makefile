# ============================================================
# RigCom v8.0 — Makefile
# Intelligent Compiler Platform · φ = 1.6180339887498948482
# Author: Richard Felipe Urbina & Edgar José Gabriel Mora
# v8: NeuralCache · ASTHeal · DepGraph · RigLib · RigCanvas
#     + ptrace Debugger · HoloTrace · NeonForge · JNI-Zero
#     + APK v2 Manifest · V1/V2/V3 Signing · Iconos Adaptativos
# ============================================================

CC = clang
CFLAGS  = -std=c11 -Wall -Wextra -O3 -fPIC \
           -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE \
           -Wno-unused-result -Wno-format-truncation \
           -Wno-unused-parameter -Wno-unknown-pragmas
SYSINC  = -isystem /usr/include
USERINC = -I./include
LDFLAGS = -lpthread -lm

# ARM64 / Snapdragon cross-compile flags
ARM64_FLAGS = -march=armv8-a+crypto+neon -mtune=cortex-a76 -ffast-math
# SafeStack (habilitar con: make SAFESTACK=1)
ifdef SAFESTACK
CFLAGS  += -fsanitize=safe-stack
LDFLAGS += -fsanitize=safe-stack
endif

SRC_DIR = src
INC_DIR = include
BLD_DIR = build
OBJ_DIR = $(BLD_DIR)/obj
BIN_DIR = $(BLD_DIR)/bin

SOURCES = \
    $(SRC_DIR)/main.c           \
    $(SRC_DIR)/rigctx.c         \
    $(SRC_DIR)/error.c          \
    $(SRC_DIR)/rigir.c          \
    $(SRC_DIR)/backend.c        \
    $(SRC_DIR)/lexer.c          \
    $(SRC_DIR)/ast.c            \
    $(SRC_DIR)/parser.c         \
    $(SRC_DIR)/symtable.c       \
    $(SRC_DIR)/typechecker.c    \
    $(SRC_DIR)/sched.c          \
    $(SRC_DIR)/toml.c           \
    $(SRC_DIR)/wsserver.c       \
    $(SRC_DIR)/preproc.c        \
    $(SRC_DIR)/apkpack.c        \
    $(SRC_DIR)/rigdap.c         \
    $(SRC_DIR)/rigpack.c        \
    $(SRC_DIR)/rigbridge.c      \
    $(SRC_DIR)/frontend_c.c     \
    $(SRC_DIR)/gvn.c            \
    $(SRC_DIR)/oracle_ip.c      \
    $(SRC_DIR)/pty_term.c       \
    $(SRC_DIR)/arena_harden.c   \
    $(SRC_DIR)/rigcache.c       \
    $(SRC_DIR)/rigscript.c      \
    $(SRC_DIR)/ptrace_dbg.c     \
    $(SRC_DIR)/holo_trace.c     \
    $(SRC_DIR)/neon_forge.c     \
    $(SRC_DIR)/jni_zero.c       \
    $(SRC_DIR)/neural_cache.c   \
    $(SRC_DIR)/ast_heal.c       \
    $(SRC_DIR)/depgraph.c       \
    $(SRC_DIR)/riglib.c         \
    $(SRC_DIR)/rigcanvas.c

OBJECTS = $(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
TARGET  = $(BIN_DIR)/rigcom

# ─── Default target ───────────────────────────────────────────
.PHONY: all
all: dirs $(TARGET)
	@echo ""
	@echo "\033[35m╔════════════════════════════════════════════════════════╗"
	@echo "║  ✓ RigCom v8.0 — OMEGA BUILD compilado exitosamente   ║"
	@echo "║  📦 $(TARGET)"
	@echo "║  φ = 1.6180339887498948482                              ║"
	@echo "║  NeuralCache · ASTHeal · DepGraph · RigLib · RigCanvas  ║"
	@echo "║  ptrace · HoloTrace · NeonForge · JNI-Zero · APKv2     ║"
	@echo "║  Dr. R.F.Urbina · Dr. E.J.G.Mora                       ║"
	@echo "╚════════════════════════════════════════════════════════╝\033[0m"
	@echo ""

# ─── Create directories ───────────────────────────────────────
.PHONY: dirs
dirs:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR) .rigcache

# ─── Compile object files ─────────────────────────────────────
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | dirs
	@echo "  \033[36mCC\033[0m  $<"
	@$(CC) $(CFLAGS) $(SYSINC) $(USERINC) -c $< -o $@

# ─── Link binary ──────────────────────────────────────────────
$(TARGET): $(OBJECTS)
	@echo "  \033[33mLD\033[0m  rigcom v8.0"
	@$(CC) $(OBJECTS) $(LDFLAGS) -o $@

# ─── Install ──────────────────────────────────────────────────
.PHONY: install
install: $(TARGET)
	@mkdir -p /usr/local/bin
	@cp $(TARGET) /usr/local/bin/rigcom
	@chmod +x /usr/local/bin/rigcom
	@echo "\033[32m✓ Instalado → /usr/local/bin/rigcom\033[0m"

# ─── Termux / Android install ─────────────────────────────────
.PHONY: install-termux
install-termux: $(TARGET)
	@cp $(TARGET) $(PREFIX)/bin/rigcom
	@chmod +x $(PREFIX)/bin/rigcom
	@echo "\033[32m✓ Instalado → $(PREFIX)/bin/rigcom\033[0m"

# ─── Tests ────────────────────────────────────────────────────
.PHONY: test
test: $(TARGET)
	@echo "🧪 Smoke tests v8.0"
	@$(TARGET) --version 2>/dev/null || $(TARGET) info
	@echo "\033[32m✓ Smoke tests OK\033[0m"

# ─── Unit tests ───────────────────────────────────────────────
.PHONY: test-unit
test-unit:
	@echo "  🧪 Compilando tests..."
	@$(CC) $(CFLAGS) tests/test_basic.c \
	    $(filter-out $(SRC_DIR)/main.c, $(SOURCES)) \
	    $(LDFLAGS) -o build/bin/test_basic
	@echo "  🧪 Ejecutando tests..."
	@./build/bin/test_basic

# ─── Format code ──────────────────────────────────────────────
.PHONY: format
format:
	@clang-format -i $(SRC_DIR)/*.c $(INC_DIR)/*.h

# ─── Static analysis ──────────────────────────────────────────
.PHONY: analyze
analyze:
	@clang --analyze $(SOURCES) -I$(INC_DIR)

# ─── Clean ────────────────────────────────────────────────────
.PHONY: clean
clean:
	@rm -rf $(BLD_DIR)
	@echo "\033[32m✓ Limpiado\033[0m"

.PHONY: clean-cache
clean-cache:
	@rm -rf .rigcache
	@echo "\033[32m✓ Caché limpiado\033[0m"

# ─── Help ─────────────────────────────────────────────────────
.PHONY: help
help:
	@echo "RigCom v8.0 — Build System"
	@echo ""
	@echo "Targets:"
	@echo "  make                 Build rigcom v8.0"
	@echo "  make SAFESTACK=1     Build con SafeStack dual-pila"
	@echo "  make install         Install to /usr/local/bin"
	@echo "  make install-termux  Install to Termux PREFIX/bin"
	@echo "  make test            Run smoke tests"
	@echo "  make test-unit       Run unit tests"
	@echo "  make format          Format code (clang-format)"
	@echo "  make analyze         Static analysis"
	@echo "  make clean           Remove build artifacts"
	@echo "  make clean-cache     Remove .rigcache directory"
	@echo "  make dist            Create release zip"

# ─── Release zip ──────────────────────────────────────────────
.PHONY: dist
dist: clean all
	@echo "  📦 Creando paquete de distribución v8.0"
	@zip -r rigcom-v8-release.zip . \
	    --exclude "./build/*" \
	    --exclude "./.rigcache/*" \
	    --exclude "*/.git/*" \
	    --exclude "*.zip"
	@echo "  \033[32m✓ rigcom-v8-release.zip listo\033[0m"

.DEFAULT_GOAL := all
