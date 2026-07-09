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

# Precisa casar com ROBOT_SECRET no firmware para o RDISCOVER ser aceito.
# Pode ser sobrescrito antes de rodar:
#   ROBO_DISCOVERY_TOKEN="meu_segredo" bash run_server.sh
export ROBO_DISCOVERY_TOKEN="${ROBO_DISCOVERY_TOKEN:-$DEFAULT_DISCOVERY_TOKEN}"

python "$BASE_DIR/robo_server.py"
