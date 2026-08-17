#!/usr/bin/env bash
set -euo pipefail

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$BASE_DIR/.venv"
SYSTEM_PACKAGES=(python3-venv python3-pip ffmpeg alsa-utils wget curl)

# shellcheck source=lib_ollama.sh
source "$BASE_DIR/lib_ollama.sh"

# Pule a parte do LLM com: ROBO_INSTALL_OLLAMA=0 bash install_server.sh
INSTALL_OLLAMA="${ROBO_INSTALL_OLLAMA:-1}"

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

if [[ "$INSTALL_OLLAMA" == "1" ]]; then
  echo
  echo "--- LLM (Ollama) ---"

  if command -v ollama >/dev/null 2>&1; then
    echo "Ollama já instalado."
  else
    echo "Ollama não encontrado. Instalando pelo script oficial da Ollama."
    echo "Para pular esta etapa: ROBO_INSTALL_OLLAMA=0 bash install_server.sh"
    curl -fsSL https://ollama.com/install.sh | sh
  fi

  if command -v ollama >/dev/null 2>&1; then
    # Precisa estar no ar para o pull funcionar.
    if ollama_garantir_no_ar; then
      MODELO="$(ollama_modelo)"
      if ollama_tem_modelo; then
        echo "Modelo já presente: $MODELO"
      else
        echo "Baixando o modelo $MODELO (isso leva alguns minutos)..."
        ollama pull "$MODELO"
      fi
    else
      echo "Aviso: não consegui subir o Ollama agora."
      echo "       O servidor roda mesmo assim, com o fallback local."
    fi
  else
    echo "Aviso: Ollama continua ausente. O servidor usará o fallback local."
  fi
else
  echo
  echo "Etapa do Ollama pulada (ROBO_INSTALL_OLLAMA=0)."
fi

echo
echo "Ambiente do servidor pronto em: $VENV_DIR"
echo "Pacotes de sistema tratados: ${SYSTEM_PACKAGES[*]}"
