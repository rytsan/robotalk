#!/usr/bin/env bash
set -euo pipefail

# Converte WAV para PCM S16LE mono 16 kHz em RAW.
# Uso:
#   bash convert_wav_to_raw.sh entrada.wav saida.raw

if [[ $# -ne 2 ]]; then
  echo "Uso: $0 entrada.wav saida.raw"
  exit 1
fi

INPUT_WAV="$1"
OUTPUT_RAW="$2"

ffmpeg -y -i "$INPUT_WAV" -ac 1 -ar 16000 -f s16le "$OUTPUT_RAW"
