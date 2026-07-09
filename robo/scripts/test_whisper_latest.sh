#!/usr/bin/env bash
set -euo pipefail

# Ativa o venv do servidor e transcreve o WAV mais recente em server/mic_tests/.

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_DIR="$BASE_DIR/server"
VENV_DIR="$SERVER_DIR/.venv"

if [[ ! -x "$VENV_DIR/bin/python" ]]; then
  echo "Ambiente virtual não encontrado em $VENV_DIR"
  echo "Execute primeiro: bash $SERVER_DIR/install_server.sh"
  exit 1
fi

LATEST_WAV="$(ls -1t "$SERVER_DIR"/mic_tests/*.wav 2>/dev/null | head -n 1 || true)"
if [[ -z "$LATEST_WAV" ]]; then
  echo "Nenhum WAV encontrado em $SERVER_DIR/mic_tests/"
  exit 1
fi

source "$VENV_DIR/bin/activate"
python3 - <<'PY' "$LATEST_WAV"
from pathlib import Path
import sys
from faster_whisper import WhisperModel

wav_path = Path(sys.argv[1])
print(f"Arquivo: {wav_path}")
print("Carregando modelo base em cpu/int8...")
model = WhisperModel("base", device="cpu", compute_type="int8")
segments, info = model.transcribe(
    str(wav_path),
    language="pt",
    beam_size=5,
    vad_filter=False,
    condition_on_previous_text=False,
    temperature=0.0,
)

print(f"Idioma detectado: {info.language} ({info.language_probability:.2f})")
print("Segmentos:")
parts = []
for segment in segments:
    text = segment.text.strip()
    if text:
        parts.append(text)
    print(f"[{segment.start:.2f}s -> {segment.end:.2f}s] {text}")

print("Transcrição final:")
print(" ".join(parts).strip())
PY
