#!/usr/bin/env bash
# Funções compartilhadas para lidar com o Ollama.
# Carregado com `source` por install_server.sh e run_server.sh.
#
# Nada aqui é fatal: o servidor tem fallback local e continua funcionando sem
# LLM. O objetivo é falhar em voz alta, não derrubar o robô.

# http://127.0.0.1:11434/api/chat -> http://127.0.0.1:11434
ollama_api_root() {
  local url="${ROBO_OLLAMA_BASE_URL:-http://127.0.0.1:11434/api/chat}"
  printf '%s' "${url%%/api/*}"
}

ollama_modelo() {
  printf '%s' "${ROBO_OLLAMA_MODEL:-qwen2.5:1.5b-instruct}"
}

ollama_responde() {
  curl -fsS --max-time 2 "$(ollama_api_root)/api/tags" >/dev/null 2>&1
}

# Sobe o Ollama se não estiver respondendo. Devolve 1 se não conseguir.
ollama_garantir_no_ar() {
  if ollama_responde; then
    return 0
  fi

  if ! command -v ollama >/dev/null 2>&1; then
    echo "Ollama não está instalado. Rode: bash install_server.sh"
    return 1
  fi

  echo "Ollama não está respondendo. Tentando subir..."

  if command -v systemctl >/dev/null 2>&1; then
    sudo systemctl start ollama >/dev/null 2>&1 \
      || systemctl --user start ollama >/dev/null 2>&1 \
      || true
  fi

  if ! ollama_responde; then
    nohup ollama serve >/tmp/ollama_robo.log 2>&1 &
  fi

  local tentativa
  for tentativa in $(seq 1 20); do          # até ~10 s
    if ollama_responde; then
      echo "Ollama no ar em $(ollama_api_root)."
      return 0
    fi
    sleep 0.5
  done

  echo "Ollama não subiu em 10 s. Log em /tmp/ollama_robo.log"
  return 1
}

# O modelo pedido está instalado? Compara o nome exato: `qwen2.5:1.5b` e
# `qwen2.5:1.5b-instruct` são modelos diferentes, e pedir o que não existe
# devolve HTTP 404 que o servidor trata como fallback.
ollama_tem_modelo() {
  local alvo
  alvo="$(ollama_modelo)"
  ollama list 2>/dev/null | awk 'NR > 1 { print $1 }' | grep -qx -- "$alvo"
}
