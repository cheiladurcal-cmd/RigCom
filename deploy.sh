#!/usr/bin/env bash
# ============================================================
#  RigCom v4.0 — Deploy / Install Script
#  Intelligent Compiler Platform
#  Author: Richard Felipe Urbina
#  φ = 1.6180339887498948482 · P(A) ∈ {0,1}
# ============================================================

set -euo pipefail

RIGCOM_VERSION="4.0.0"
BOLD="\033[1m"
CYAN="\033[36m"
GREEN="\033[32m"
YELLOW="\033[33m"
RED="\033[31m"
RESET="\033[0m"

banner() {
    echo -e "${CYAN}${BOLD}"
    echo "  ██████╗ ██╗ ██████╗  ██████╗ ██████╗ ███╗   ███╗"
    echo "  ██╔══██╗██║██╔════╝ ██╔════╝██╔═══██╗████╗ ████║"
    echo "  ██████╔╝██║██║  ███╗██║     ██║   ██║██╔████╔██║"
    echo "  ██╔══██╗██║██║   ██║██║     ██║   ██║██║╚██╔╝██║"
    echo "  ██║  ██║██║╚██████╔╝╚██████╗╚██████╔╝██║ ╚═╝ ██║"
    echo "  ╚═╝  ╚═╝╚═╝ ╚═════╝  ╚═════╝ ╚═════╝ ╚═╝     ╚═╝"
    echo -e "${RESET}"
    echo -e "  ${CYAN}RigCom v${RIGCOM_VERSION} — Instalador automático${RESET}"
    echo -e "  ${CYAN}φ = 1.6180339887498948482${RESET}"
    echo ""
}

detect_platform() {
    if [ -n "${TERMUX_VERSION:-}" ] || [ -d "$HOME/../usr/bin" ]; then
        echo "termux"
    elif [ "$(uname)" = "Darwin" ]; then
        echo "macos"
    else
        echo "linux"
    fi
}

install_deps_termux() {
    echo -e "  ${YELLOW}→ Instalando dependencias (Termux)...${RESET}"
    pkg update -y -q
    pkg install -y -q clang make
    echo -e "  ${GREEN}✓ Dependencias OK${RESET}"
}

install_deps_linux() {
    echo -e "  ${YELLOW}→ Verificando dependencias (Linux)...${RESET}"
    if ! command -v clang &>/dev/null; then
        if command -v apt-get &>/dev/null; then
            sudo apt-get install -yq clang
        elif command -v pacman &>/dev/null; then
            sudo pacman -S --noconfirm clang
        elif command -v dnf &>/dev/null; then
            sudo dnf install -yq clang
        else
            echo -e "  ${RED}✗ Instala clang manualmente${RESET}"
            exit 1
        fi
    fi
    echo -e "  ${GREEN}✓ clang $(clang --version | head -1)${RESET}"
}

install_deps_macos() {
    echo -e "  ${YELLOW}→ Verificando dependencias (macOS)...${RESET}"
    if ! command -v clang &>/dev/null; then
        xcode-select --install 2>/dev/null || true
    fi
    echo -e "  ${GREEN}✓ Dependencias OK${RESET}"
}

build() {
    echo ""
    echo -e "  ${CYAN}→ Compilando RigCom v${RIGCOM_VERSION}...${RESET}"
    make clean all 2>&1 | grep -E "CC|LD|error|warning|✓" || make clean all
    echo -e "  ${GREEN}✓ Build completado${RESET}"
}

install_binary() {
    local platform="$1"
    echo ""
    echo -e "  ${CYAN}→ Instalando binario...${RESET}"

    if [ "$platform" = "termux" ]; then
        INSTALL_DIR="${PREFIX}/bin"
        mkdir -p "$INSTALL_DIR"
        cp build/bin/rigcom "$INSTALL_DIR/rigcom"
        chmod +x "$INSTALL_DIR/rigcom"
        # Install dashboard
        mkdir -p "${PREFIX}/share/rigcom"
        cp ui/index.html "${PREFIX}/share/rigcom/dashboard.html"
        echo -e "  ${GREEN}✓ Instalado → ${INSTALL_DIR}/rigcom${RESET}"
        echo -e "  ${GREEN}✓ Dashboard → ${PREFIX}/share/rigcom/dashboard.html${RESET}"
    else
        INSTALL_DIR="/usr/local/bin"
        SHARE_DIR="/usr/local/share/rigcom"
        sudo mkdir -p "$INSTALL_DIR" "$SHARE_DIR"
        sudo cp build/bin/rigcom "$INSTALL_DIR/rigcom"
        sudo chmod +x "$INSTALL_DIR/rigcom"
        sudo cp ui/index.html "$SHARE_DIR/dashboard.html"
        echo -e "  ${GREEN}✓ Instalado → ${INSTALL_DIR}/rigcom${RESET}"
        echo -e "  ${GREEN}✓ Dashboard → ${SHARE_DIR}/dashboard.html${RESET}"
    fi
}

verify() {
    echo ""
    echo -e "  ${CYAN}→ Verificando instalación...${RESET}"
    if command -v rigcom &>/dev/null; then
        rigcom info
        echo -e "  ${GREEN}✓ rigcom funciona correctamente${RESET}"
    else
        echo -e "  ${YELLOW}⚠ Agrega el directorio de instalación a tu PATH${RESET}"
    fi
}

main() {
    banner
    local platform
    platform=$(detect_platform)
    echo -e "  Plataforma detectada: ${BOLD}${platform}${RESET}"

    case "$platform" in
        termux) install_deps_termux ;;
        macos)  install_deps_macos  ;;
        linux)  install_deps_linux  ;;
    esac

    build
    install_binary "$platform"
    verify

    echo ""
    echo -e "  ${YELLOW}╔════════════════════════════════════════╗${RESET}"
    echo -e "  ${YELLOW}║  RigCom v${RIGCOM_VERSION} instalado correctamente  ║${RESET}"
    echo -e "  ${YELLOW}║  φ = 1.6180339887498948482             ║${RESET}"
    echo -e "  ${YELLOW}╚════════════════════════════════════════╝${RESET}"
    echo ""
    echo -e "  Comandos disponibles:"
    echo -e "    ${CYAN}rigcom build${RESET}       — Compilar proyecto"
    echo -e "    ${CYAN}rigcom check${RESET}       — Análisis semántico"
    echo -e "    ${CYAN}rigcom ui${RESET}          — Dashboard WebSocket"
    echo -e "    ${CYAN}rigcom bootstrap${RESET}   — Auto-compilación"
    echo -e "    ${CYAN}rigcom info${RESET}        — Info del sistema"
    echo ""
}

main "$@"
