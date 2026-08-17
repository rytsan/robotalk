#!/usr/bin/env bash
set -euo pipefail

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$BASE_DIR/.venv"
SYSTEM_PACKAGES=(python3-venv python3-pip ffmpeg alsa-utils wget curl)

# shellcheck source=lib_ollama.sh
source "$BASE_DIR/lib_ollama.sh"

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 não encontrado. Instale o Python 3 antes de continuar."
  exit 1
fi

if command -v apt-get >/dev/null 2>&1; then
  if command -v sudo >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y "${SYSTEM_PACKAGES[@]}"
  elif [[ ${EUID:-$(id -u)} -eq 0 ]]; then
    apt-get update
    apt-get install -y "${SYSTEM_PACKAGES[@]}"
  else
    echo "Pacotes de sistema sugeridos: ${SYSTEM_PACKAGES[*]}"
    echo "Instale-os com permissões de administrador antes de continuar."
  fi
else
  echo "apt-get não encontrado; instale manualmente: ${SYSTEM_PACKAGES[*]}"
fi

if [[ ! -d "$VENV_DIR" ]]; then
  python3 -m venv "$VENV_DIR"
fi

"$VENV_DIR/bin/python" -m pip install --upgrade pip
"$VENV_DIR/bin/pip" install -r "$BASE_DIR/requirements.txt"

mkdir -p "$BASE_DIR/mic_tests" "$BASE_DIR/tts_out" "$BASE_DIR/voices"

# ============================================================
# LLM (Ollama)
# ============================================================

echo
echo "--- LLM (Ollama) ---"

# Mesma funcao que o run_server.sh usa, entao os dois caminhos convergem e nao
# ha duas logicas de instalacao para manter em sincronia.
if ! ollama_preparar; then
  echo "Aviso: o servidor roda mesmo assim, com o fallback local."
fi

echo
echo "Ambiente do servidor pronto em: $VENV_DIR"
echo "Pacotes de sistema tratados: ${SYSTEM_PACKAGES[*]}"
echo
echo "Daqui em diante, basta: bash run_server.sh"
