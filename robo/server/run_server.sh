#!/usr/bin/env bash
set -euo pipefail

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$BASE_DIR/.venv"
DEFAULT_DISCOVERY_TOKEN="TROQUE_ESTE_SEGREDO_COMPARTILHADO"

if [[ ! -x "$VENV_DIR/bin/python" ]]; then
  echo "Ambiente virtual não encontrado. Execute primeiro: bash install_server.sh"
  exit 1
fi

source "$VENV_DIR/bin/activate"

# shellcheck source=lib_ollama.sh
source "$BASE_DIR/lib_ollama.sh"

# Sobe o Ollama antes do servidor, se ainda nao estiver no ar. Falhar aqui nao
# impede o robo de rodar: ele responde com o fallback local, so mais burro.
if ! ollama_garantir_no_ar; then
  echo "Aviso: seguindo sem LLM. As respostas usarão o fallback local."
elif ! ollama_tem_modelo; then
  echo "Aviso: o modelo '$(ollama_modelo)' não está instalado."
  echo "       Rode: ollama pull $(ollama_modelo)"
  echo "       Ou aponte para outro: ROBO_OLLAMA_MODEL=<nome> bash run_server.sh"
fi

# Precisa casar com ROBOT_SECRET no firmware para o RDISCOVER ser aceito.
# Pode ser sobrescrito antes de rodar:
#   ROBO_DISCOVERY_TOKEN="meu_segredo" bash run_server.sh
export ROBO_DISCOVERY_TOKEN="${ROBO_DISCOVERY_TOKEN:-$DEFAULT_DISCOVERY_TOKEN}"

# Beacon UDP ligado por padrão: o servidor se anuncia em broadcast a cada
# intervalo, então o Cardputer o encontra mesmo tendo ligado antes dele.
# O RDISCOVER continua ativo em paralelo; um não substitui o outro.
# Desligue com ROBO_DISCOVERY_BEACON_ENABLED=0 se a rede reclamar de broadcast.
export ROBO_DISCOVERY_BEACON_ENABLED="${ROBO_DISCOVERY_BEACON_ENABLED:-1}"

python "$BASE_DIR/robo_server.py"
