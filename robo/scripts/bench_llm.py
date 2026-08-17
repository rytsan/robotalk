#!/usr/bin/env python3
"""Mede o custo real de cada modelo NA MÁQUINA ONDE ELE VAI RODAR.

Números de LLM não transferem entre máquinas: geração de token é limitada por
banda de memória, e a do Raspberry é bem menor que a de um desktop. Rode isto
no próprio Pi antes de escolher.

Uso:
    python3 bench_llm.py
    python3 bench_llm.py qwen2.5:1.5b cnmoro/gemma3-gaia-ptbr-4b:q4_k_m

Usa as métricas que o próprio Ollama devolve (`eval_count`, `eval_duration`,
`prompt_eval_duration`), então não depende de cronometrar de fora.
"""

from __future__ import annotations

import json
import sys
import time
from urllib import error, request

OLLAMA_URL = "http://127.0.0.1:11434/api/chat"
TAGS_URL = "http://127.0.0.1:11434/api/tags"

SYSTEM_PROMPT = (
    "Você é o cérebro de um robô experimental em português do Brasil. "
    "Responda de forma curta, natural e útil. "
    "Se a entrada for ambígua, faça uma resposta simples e objetiva. "
    "Se o usuário pedir ações do robô, explique em uma frase o que será feito."
)

FALAS = [
    "Oi robo, tudo bem?",
    "Liga a luz da sala",
    "Me explica rapidinho o que e um buraco negro",
    "Estou meio pra baixo hoje",
]


def modelos_instalados() -> list[str]:
    try:
        with request.urlopen(TAGS_URL, timeout=5) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except (error.URLError, OSError) as exc:
        print(f"Ollama não respondeu: {exc}")
        sys.exit(1)
    nomes = [str(m.get("name", "")) for m in data.get("models", [])]
    # embeddings não servem para chat
    return [n for n in nomes if "embed" not in n]


def medir(model: str, fala: str) -> dict | None:
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": fala},
        ],
        "stream": False,
        "options": {"temperature": 0.4, "num_predict": 128},
    }
    req = request.Request(
        OLLAMA_URL,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    inicio = time.time()
    try:
        with request.urlopen(req, timeout=600) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except (error.URLError, OSError) as exc:
        print(f"    ERRO: {exc}")
        return None

    tokens = int(data.get("eval_count", 0) or 0)
    dur_ns = int(data.get("eval_duration", 0) or 0)
    prefill_ns = int(data.get("prompt_eval_duration", 0) or 0)

    return {
        "parede": time.time() - inicio,
        "tokens": tokens,
        "geracao_s": dur_ns / 1e9,
        "prefill_s": prefill_ns / 1e9,
        "tok_s": (tokens / (dur_ns / 1e9)) if dur_ns else 0.0,
        "texto": " ".join(str(data.get("message", {}).get("content", "")).split()),
    }


def main() -> None:
    alvos = sys.argv[1:] or modelos_instalados()
    if not alvos:
        print("Nenhum modelo de chat instalado.")
        sys.exit(1)

    print("Cada modelo é aquecido antes de medir: a primeira chamada inclui")
    print("carregar o modelo do disco e distorceria o resultado.")
    print()

    resumo: list[tuple[str, float, float]] = []

    for model in alvos:
        print("=" * 70)
        print(model)
        print("=" * 70)

        print("  aquecendo...", flush=True)
        if medir(model, "oi") is None:
            print("  pulando.\n")
            continue

        toks = 0.0
        parede = 0.0
        amostras = 0

        for fala in FALAS:
            r = medir(model, fala)
            if r is None:
                continue
            amostras += 1
            toks += r["tok_s"]
            parede += r["parede"]
            print(f"  {r['parede']:6.1f}s parede | {r['tok_s']:5.1f} tok/s | "
                  f"prefill {r['prefill_s']:4.1f}s | {r['tokens']:3d} tokens")
            print(f"          {r['texto'][:150]}")

        if not amostras:
            continue

        tok_s = toks / amostras
        media = parede / amostras
        resumo.append((model, tok_s, media))
        print(f"  --- média: {media:.1f}s por resposta, {tok_s:.1f} tok/s")
        print()

    print("=" * 70)
    print("RESUMO")
    print("=" * 70)
    print(f"{'modelo':40} {'tok/s':>7} {'resposta':>10} {'turno*':>9}")
    for model, tok_s, media in sorted(resumo, key=lambda x: -x[1]):
        # O turno real faz DUAS chamadas: a resposta e a extração de fatos.
        print(f"{model:40} {tok_s:7.1f} {media:9.1f}s {media * 2:8.1f}s")
    print()
    print("* turno = resposta + extração de fatos, que são duas chamadas ao LLM.")
    print("  Ainda faltam Whisper e Piper por cima disso.")
    print()
    print("Se o turno passar de uns 10 s, considere:")
    print("  - modelo menor para a resposta;")
    print("  - ROBO_FACT_EXTRACTION=0 para cortar a segunda chamada;")
    print("  - ROBO_EXTRACTION_MODEL para extrair fatos com um modelo menor.")


if __name__ == "__main__":
    main()
