#!/usr/bin/env bash
# Funções compartilhadas para lidar com o Ollama.
# Carregado com `source` por install_server.sh e run_server.sh.
#
# Tudo aqui é idempotente: cada etapa é pulada quando já está satisfeita, então
# chamar de novo não reinstala nem rebaixa nada.
#
# Nada aqui é fatal. O servidor tem fallback local e continua funcionando sem
# LLM: o objetivo é falhar em voz alta, não derrubar o robô.

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

# O modelo pedido está instalado? Compara o nome EXATO (grep -qx): comparação
# por substring daria falso positivo entre `qwen2.5:1.5b` e
# `qwen2.5:1.5b-instruct`, que são modelos diferentes. Pedir um que não existe
# devolve HTTP 404 e o servidor cai no fallback em silêncio.
ollama_tem_modelo() {
  local alvo
  alvo="$(ollama_modelo)"
  ollama list 2>/dev/null | awk 'NR > 1 { print $1 }' | grep -qx -- "$alvo"
}

# --- etapas, todas puláveis quando já resolvidas ---

ollama_instalar_se_faltar() {
  if command -v ollama >/dev/null 2>&1; then
    return 0
  fi
  if [[ "${ROBO_INSTALL_OLLAMA:-1}" != "1" ]]; then
    echo "LLM: Ollama ausente e instalação desativada (ROBO_INSTALL_OLLAMA=0)."
    return 1
  fi

  echo "LLM: Ollama não encontrado. Instalando (só desta vez)..."
  curl -fsSL https://ollama.com/install.sh | sh
  command -v ollama >/dev/null 2>&1
}

ollama_subir_se_parado() {
  if ollama_responde; then
    return 0
  fi
  if ! command -v ollama >/dev/null 2>&1; then
    return 1
  fi

  echo "LLM: subindo o Ollama..."

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
      return 0
    fi
    sleep 0.5
  done

  echo "LLM: Ollama não subiu em 10 s. Log em /tmp/ollama_robo.log"
  return 1
}

ollama_baixar_modelo_se_faltar() {
  local modelo
  modelo="$(ollama_modelo)"

  if ollama_tem_modelo; then
    return 0
  fi
  if [[ "${ROBO_PULL_MODEL:-1}" != "1" ]]; then
    echo "LLM: modelo '$modelo' ausente e download desativado (ROBO_PULL_MODEL=0)."
    return 1
  fi

  echo "LLM: baixando o modelo '$modelo'. Isso acontece uma vez só e demora."
  ollama pull "$modelo" || return 1
  ollama_tem_modelo
}

# Deixa o LLM pronto: instala, sobe e baixa o modelo, pulando o que já existe.
# Numa máquina já preparada isto custa um curl de milissegundos.
ollama_preparar() {
  ollama_instalar_se_faltar     || return 1
  ollama_subir_se_parado        || return 1
  ollama_baixar_modelo_se_faltar || return 1

  echo "LLM pronto: '$(ollama_modelo)' em $(ollama_api_root)"
  return 0
}
