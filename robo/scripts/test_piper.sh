#!/usr/bin/env bash
set -euo pipefail

# Gera uma frase simples com Piper e converte para RAW 16 kHz mono.
# Depois disso você pode enviar o RAW ao Cardputer pelo servidor.

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_DIR="$BASE_DIR/server"
VENV_DIR="$SERVER_DIR/.venv"
VOICE_DIR="$SERVER_DIR/voices"
TEXT_FILE="$SERVER_DIR/tts_out/piper_test.txt"
WAV_FILE="$SERVER_DIR/tts_out/piper_test.wav"
RAW_FILE="$SERVER_DIR/tts_out/piper_test_16k.raw"
TEXT="Olá, este é um teste do Piper para o robô experimental."

if [[ ! -x "$VENV_DIR/bin/python" ]]; then
  echo "Ambiente virtual não encontrado em $VENV_DIR"
  echo "Execute primeiro: bash $SERVER_DIR/install_server.sh"
  exit 1
fi

if [[ ! -f "$VOICE_DIR/pt_BR-faber-medium.onnx" || ! -f "$VOICE_DIR/pt_BR-faber-medium.onnx.json" ]]; then
  echo "Arquivos de voz ausentes em $VOICE_DIR"
  echo "Veja: $VOICE_DIR/README.md"
  exit 1
fi

source "$VENV_DIR/bin/activate"
mkdir -p "$SERVER_DIR/tts_out"
printf '%s\n' "$TEXT" > "$TEXT_FILE"

python3 -m piper \
  --model "$VOICE_DIR/pt_BR-faber-medium.onnx" \
  --input-file "$TEXT_FILE" \
  --output-file "$WAV_FILE"

ffmpeg -y -i "$WAV_FILE" -ac 1 -ar 16000 -f s16le "$RAW_FILE"

echo "WAV gerado: $WAV_FILE"
echo "RAW gerado: $RAW_FILE"
echo "Se quiser ouvir no Cardputer, use o comando 'say ...' no console do servidor ou envie pelo fluxo WebSocket."
