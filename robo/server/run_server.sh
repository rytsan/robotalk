#!/usr/bin/env bash
set -euo pipefail

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$BASE_DIR/.venv"

if [[ ! -x "$VENV_DIR/bin/python" ]]; then
  echo "Ambiente virtual não encontrado. Execute primeiro: bash install_server.sh"
  exit 1
fi

source "$VENV_DIR/bin/activate"
python "$BASE_DIR/robo_server.py"
