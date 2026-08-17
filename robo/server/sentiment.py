#!/usr/bin/env python3
"""Análise de sentimento local para o servidor do robô.

Canal de empatia: esta análise descreve a emoção do USUÁRIO, extraída da
transcrição da fala dele. O estado do próprio robô (pensando, erro, falando)
é outro canal, resolvido no firmware pela cor do rosto.

Decisões:
- léxico embutido em português, sem download e sem dependência externa;
- valência contínua em [-1, 1] em vez de baldes discretos, para permitir
  intensidade depois;
- arousal separado da valência: "cansado" e "furioso" são ambos negativos,
  mas pedem rostos diferentes;
- negação com janela de 3 tokens e amortecimento: "não é bom" é mais fraco
  que "ruim", não equivalente;
- confusão é tratada como estado cognitivo, não como valência. Perguntar
  não é estar confuso — esse era o bug do classificador anterior.

Um léxico externo maior (OpLexicon v3.0, SentiLex-PT) pode ser carregado
por ROBO_SENTIMENT_LEXICON; ele complementa o embutido, sem substituí-lo.
"""

from __future__ import annotations

import os
import re
import unicodedata
from dataclasses import dataclass
from pathlib import Path

VALENCE_DEADZONE = 0.15
AROUSAL_HIGH = 0.55
NEGATION_WINDOW = 3
NEGATION_DAMPING = -0.8


def normalize(text: str) -> str:
    decomposed = unicodedata.normalize("NFD", text.lower().strip())
    without_accents = "".join(ch for ch in decomposed if unicodedata.category(ch) != "Mn")
    return re.sub(r"\s+", " ", without_accents)


def tokenize(text: str) -> list[str]:
    # Mantém tokens de 2 letras: "ma", "ok" e "so" carregam sentimento.
    return re.findall(r"[a-z0-9]+", normalize(text))


# ============================================================
# LÉXICO DE VALÊNCIA (chaves já sem acento)
# ============================================================

VALENCE: dict[str, float] = {}


def _add(words: str, score: float) -> None:
    for word in words.split():
        VALENCE[normalize(word)] = score


# --- positivo forte ---
_add("otimo otima excelente maravilhoso maravilhosa incrivel sensacional", 0.9)
_add("fantastico fantastica perfeito perfeita espetacular formidavel", 0.9)
_add("adorei amei apaixonado encantado emocionante", 0.85)
_add("melhor lindo linda genial brilhante impecavel", 0.8)

# --- positivo moderado ---
_add("bom boa legal bacana massa show joia", 0.6)
_add("gostei gosto curti curto agrada agradavel", 0.6)
_add("feliz contente alegre animado satisfeito realizado", 0.7)
_add("obrigado obrigada valeu grato agradeco", 0.55)
_add("tranquilo tranquila calmo calma sereno relaxado confortavel", 0.45)
_add("certo correto exato funcionou consegui resolvido sucesso pronto", 0.55)
_add("sim claro beleza ok positivo confirmado", 0.35)
_add("amigo amiga carinho amor gentil querido divertido", 0.65)

# --- negativo moderado ---
_add("ruim ruins fraco fraca pobre mediocre", -0.6)
_add("triste chateado chateada magoado desanimado deprimido abatido", -0.7)
_add("cansado cansada exausto esgotado sonolento entediado", -0.45)
_add("chato chata irritante aborrecido incomodo tedioso", -0.55)
_add("dificil complicado confuso bagunca desorganizado", -0.4)
_add("problema problemas erro erros falha falhou quebrou bug", -0.55)
_add("perdido travado parado atrasado lento demora", -0.45)
_add("medo receio inseguro preocupado ansioso nervoso tenso aflito", -0.6)

# --- negativo forte ---
_add("pessimo pessima horrivel terrivel horroroso insuportavel", -0.9)
_add("odeio detesto odiei raiva furioso irritado revoltado", -0.85)
_add("desastre catastrofe tragedia fracasso desespero", -0.85)
_add("burro idiota estupido lixo porcaria merda droga", -0.8)
_add("morto morreu destruido arruinado impossivel", -0.7)

# ============================================================
# INTENSIFICADORES E ATENUADORES
# ============================================================

INTENSIFIERS: dict[str, float] = {}


def _intens(words: str, factor: float) -> None:
    for word in words.split():
        INTENSIFIERS[normalize(word)] = factor


_intens("extremamente absurdamente absolutamente totalmente completamente", 1.8)
_intens("muito super mega ultra bastante demais bem", 1.5)
_intens("realmente verdadeiramente sinceramente profundamente", 1.4)
_intens("meio pouco levemente ligeiramente razoavelmente", 0.5)
_intens("quase apenas so somente", 0.7)

NEGATORS = {
    "nao", "nunca", "jamais", "nenhum", "nenhuma", "ninguem",
    "nada", "sem", "nem", "tampouco",
}

# ============================================================
# AROUSAL (ativação, independente de ser bom ou ruim)
# ============================================================

AROUSAL: dict[str, float] = {}


def _arous(words: str, score: float) -> None:
    for word in words.split():
        AROUSAL[normalize(word)] = score


_arous("socorro urgente corre rapido agora emergencia imediatamente", 0.95)
_arous("odeio raiva furioso revoltado desespero panico", 0.9)
_arous("adorei amei incrivel sensacional espetacular uau nossa caramba", 0.85)
_arous("animado empolgado eufórico ansioso nervoso agitado", 0.8)
_arous("vamos bora partiu comeca acelera", 0.75)
_arous("cansado exausto entediado sonolento devagar", 0.15)
_arous("tranquilo calmo sereno relaxado indiferente", 0.2)
_arous("qualquer talvez", 0.25)
# Problema/erro é negativo COM ativação: pede rosto preocupado, não triste.
_arous("problema erro falha falhou travou quebrou bug", 0.6)

# ============================================================
# MARCADORES DE CONFUSÃO (estado cognitivo, não valência)
# ============================================================

CONFUSION_PHRASES = [
    "nao entendi", "nao entendo", "nao sei", "nao compreendi",
    "como assim", "o que voce quis dizer", "que isso", "nao faz sentido",
    "estou confuso", "estou confusa", "fiquei confuso", "me perdi",
    "pode repetir", "repete", "nao ficou claro", "explica de novo",
]

# "oi" fica de fora de propósito: é saudação, e incluí-la faria todo
# "oi robô" virar CONFUSED — exatamente o tipo de falso positivo que
# derrubava o classificador anterior.
CONFUSION_WORDS = {"confuso", "confusa", "duvida", "hein", "ahn"}


@dataclass(frozen=True)
class Sentiment:
    valence: float          # -1 (negativo) .. 1 (positivo)
    arousal: float          # 0 (apático) .. 1 (muito ativado)
    mood: str               # rótulo do protocolo EMO
    confidence: float       # 0 .. 1, baseado na cobertura do léxico
    hits: int               # quantos tokens casaram com o léxico

    def emo_command(self) -> str:
        return f"EMO {self.mood}"


def _is_confused(normalized: str, tokens: list[str]) -> bool:
    for phrase in CONFUSION_PHRASES:
        if phrase in normalized:
            return True
    return any(token in CONFUSION_WORDS for token in tokens)


def _score_tokens(tokens: list[str]) -> tuple[float, int]:
    """Soma a valência aplicando intensificador e negação por janela."""
    total = 0.0
    hits = 0

    for index, token in enumerate(tokens):
        base = VALENCE.get(token)
        if base is None or base == 0.0:
            continue

        score = base
        hits += 1

        # intensificador imediatamente antes
        if index > 0:
            factor = INTENSIFIERS.get(tokens[index - 1])
            if factor is not None:
                score *= factor

        # negador nos N tokens anteriores: inverte e amortece
        window_start = max(0, index - NEGATION_WINDOW)
        if any(tokens[j] in NEGATORS for j in range(window_start, index)):
            score *= NEGATION_DAMPING

        total += score

    return total, hits


def _score_arousal(tokens: list[str]) -> tuple[float, int]:
    values = [AROUSAL[token] for token in tokens if token in AROUSAL]
    if not values:
        return 0.0, 0
    return sum(values) / len(values), len(values)


def _mood_from(valence: float, arousal: float, confused: bool) -> str:
    if confused:
        return "CONFUSED"

    if valence > VALENCE_DEADZONE:
        return "EXCITED" if arousal >= AROUSAL_HIGH else "HAPPY"

    if valence < -VALENCE_DEADZONE:
        # negativo agitado é preocupação/raiva; negativo apático é tristeza
        return "CONCERNED" if arousal >= AROUSAL_HIGH else "SAD"

    if arousal >= AROUSAL_HIGH:
        return "EXCITED"

    return "NEUTRAL"


def analyze(text: str) -> Sentiment:
    """Analisa a fala do usuário. Não recebe a resposta do robô de propósito:
    misturar os dois foi o que tornou o classificador anterior incoerente."""
    normalized = normalize(text)
    tokens = tokenize(text)

    if not tokens:
        return Sentiment(0.0, 0.0, "NEUTRAL", 0.0, 0)

    raw_valence, hits = _score_tokens(tokens)
    arousal_mean, arousal_hits = _score_arousal(tokens)
    confused = _is_confused(normalized, tokens)

    # saturação suave: muitas palavras não devem estourar a escala
    valence = max(-1.0, min(1.0, raw_valence / 2.0))

    # sem pista de arousal, assume ativação média-baixa
    arousal = arousal_mean if arousal_hits else 0.35

    # intensidade da valência também sugere ativação
    arousal = min(1.0, max(arousal, abs(valence) * 0.7))

    coverage = (hits + arousal_hits) / len(tokens)
    confidence = min(1.0, coverage * 2.0)
    if confused:
        confidence = max(confidence, 0.6)

    mood = _mood_from(valence, arousal, confused)

    return Sentiment(
        valence=round(valence, 3),
        arousal=round(arousal, 3),
        mood=mood,
        confidence=round(confidence, 3),
        hits=hits + arousal_hits,
    )


# ============================================================
# LÉXICO EXTERNO OPCIONAL
# ============================================================

def load_external_lexicon(path: Path) -> int:
    """Carrega um léxico maior por cima do embutido.

    Aceita OpLexicon v3.0 (`palavra,pos,polaridade`) e qualquer arquivo de
    duas colunas `palavra<sep>polaridade`. Linhas malformadas são ignoradas
    em silêncio: um léxico parcial é melhor que derrubar o servidor.
    """
    if not path.exists():
        return 0

    loaded = 0
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue

        parts = re.split(r"[,;\t]", line)
        if len(parts) < 2:
            continue

        word = normalize(parts[0])
        if not word or " " in word:
            continue

        try:
            polarity = float(parts[-1])
        except ValueError:
            continue

        # OpLexicon usa -1/0/1; normaliza qualquer escala para [-1, 1]
        if polarity > 1.0 or polarity < -1.0:
            polarity = max(-1.0, min(1.0, polarity / 5.0))

        if polarity == 0.0:
            continue

        # o embutido é curado à mão e tem prioridade
        if word not in VALENCE:
            VALENCE[word] = polarity
            loaded += 1

    return loaded


def init_from_env() -> None:
    raw_path = os.environ.get("ROBO_SENTIMENT_LEXICON", "").strip()
    if not raw_path:
        return

    path = Path(raw_path)
    loaded = load_external_lexicon(path)
    if loaded:
        print(f"Léxico externo carregado de {path}: +{loaded} palavras.")
    else:
        print(f"Léxico externo não adicionou palavras: {path}")
