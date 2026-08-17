# Análise de sentimento

Módulo `server/sentiment.py`. Ele alimenta o canal de empatia do rosto: descreve a emoção do **usuário**, extraída da transcrição da fala dele.

O estado do próprio robô (pensando, falando, erro) é outro canal e não passa por aqui. Ver `rosto.md`.

## O que havia antes

`classify_mood()` fazia matching de palavras num saco só. Três defeitos de fundo:

1. `"?" in user_text` retornava `CONFUSED`, e esse teste vinha **antes** de `HAPPY` e `EXCITED`. Como o Whisper põe `?` em toda pergunta e quase toda fala ao robô é pergunta, o rosto vivia preso em `CONFUSED`.
2. `user_text` e `reply_text` eram concatenados, então o "erro" dito pelo usuário deixava o rosto preocupado mesmo com resposta tranquila.
3. Matching por substring sem fronteira de palavra: `"legal"` casava com `"ilegal"`, `"boa"` com `"Lisboa"`. E nenhum tratamento de negação, então `"não estou triste"` classificava `SAD`.

## Desenho atual

Sem dependência nova e sem latência extra: só stdlib.

### Valência contínua

Léxico pt-BR embutido, com chaves já sem acento, mapeando palavra para valência em `[-1, 1]`. Contínuo em vez de baldes discretos, para permitir intensidade depois.

A soma é saturada suavemente (`raw / 2`, limitado a `[-1, 1]`), senão uma frase com muitas palavras carregadas estouraria a escala.

### Arousal separado da valência

`cansado` e `furioso` são ambos negativos, mas pedem rostos diferentes. O arousal é um léxico próprio, menor, com a média das palavras que casaram.

Sem pista nenhuma de arousal, assume 0.35. A intensidade da valência também sugere ativação, então o valor final é `max(arousal, |valência| * 0.7)`.

### Negação e intensificadores

Negadores (`não`, `nunca`, `nem`, `sem`, ...) agem numa janela de 3 tokens à frente, invertendo e amortecendo com fator `-0.8`. O amortecimento é proposital: `"não é bom"` é mais fraco que `"ruim"`, não equivalente.

Intensificadores agem só no token imediatamente anterior: `muito` 1.5, `extremamente` 1.8, `pouco` 0.5.

### Confusão é estado cognitivo

Não é valência, e não é pontuação. É detectada por marcador explícito: frases como `"não entendi"`, `"como assim"`, `"pode repetir"`, ou palavras como `"confuso"`, `"dúvida"`, `"hein"`.

`"oi"` ficou de fora da lista de propósito: é saudação, e incluí-la faria todo `"oi robô"` virar `CONFUSED` — exatamente o falso positivo que derrubava o classificador anterior.

## Mapa para os humores

Um circumplexo simples. Confusão tem precedência sobre o resto.

```text
confusao explicita          -> CONFUSED

valencia > +0.15
  arousal >= 0.55           -> EXCITED
  arousal <  0.55           -> HAPPY

valencia < -0.15
  arousal >= 0.55           -> CONCERNED    (negativo agitado: raiva, aflicao)
  arousal <  0.55           -> SAD          (negativo apatico)

valencia neutra
  arousal >= 0.55           -> EXCITED
  resto                     -> NEUTRAL
```

Palavras de problema (`erro`, `falha`, `travou`) recebem arousal 0.6 de propósito, para caírem em `CONCERNED` e não em `SAD`. Erro é negativo com ativação.

## Comportamento verificado

| Frase | Resultado |
|---|---|
| `Qual e a capital da Franca?` | `NEUTRAL` |
| `Oi robo, tudo bem?` | `NEUTRAL` |
| `Isso ficou otimo, obrigado!` | `HAPPY` |
| `Adorei, ficou incrivel!` | `EXCITED` |
| `Estou muito triste hoje` | `SAD` |
| `Que dia chato, estou cansado` | `SAD` |
| `Deu erro de novo, nao consigo resolver` | `CONCERNED` |
| `Socorro, o sistema quebrou!` | `CONCERNED` |
| `Nao entendi o que voce disse` | `CONFUSED` |
| `Nao estou triste, estou bem` | `HAPPY` |
| `Isso e pessimo, odeio isso` | `CONCERNED` |
| `Ligue a luz da sala` | `NEUTRAL` |

As três primeiras são o teste de regressão que importa: perguntas comuns não podem mais virar `CONFUSED`.

## Léxico externo opcional

O embutido é curado à mão e cobre o vocabulário comum de conversa. Para ampliar:

```bash
export ROBO_SENTIMENT_LEXICON="/caminho/oplexicon_v3.0.txt"
```

Aceita OpLexicon v3.0 (`palavra,pos,polaridade`) e qualquer arquivo de duas colunas `palavra<sep>polaridade`. Escalas fora de `[-1, 1]` são normalizadas. Linhas malformadas são ignoradas em silêncio: um léxico parcial é melhor que derrubar o servidor.

O externo **complementa** o embutido; palavras que já existem no curado não são sobrescritas.

## Limitações conhecidas

- Léxico não pega ironia nem sarcasmo.
- A análise é sobre a transcrição, então erro de STT vira erro de sentimento.
- Não há prosódia: `"tudo bem"` dito animado e dito apático saem iguais.
- `confidence` é calculado, mas ainda não é usado para nada. O protocolo `EMO` transmite só o rótulo.

## Restrição do protocolo

Se a intensidade for transmitida ao firmware, `EMO HAPPY 0.8` **quebra** o parser atual: `tratarTexto` compara `txt.substring(4)` por igualdade exata, então `"HAPPY 0.8"` não casa com nada e cai em `MOOD_NEUTRAL` silenciosamente.

Ou trata no firmware antes, ou usa um verbo novo (`EMO2`) mantendo `EMO` como está.

## Próximos passos

- Classificação por Ollama, em paralelo com a síntese do Piper, para pegar contexto e ironia. Precisa manter o léxico como fallback, porque os atalhos de memória pulam o Ollama.
- Prosódia a partir do WAV que o servidor já tem: energia, taxa de fala e pitch dão arousal de verdade.
- Persistir `valence` e `arousal` por turno em `conversation_turns`, via o `ensure_column()` que o `MemoryStore` já usa para migração incremental.
