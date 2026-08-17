# Memória do assistente

## Objetivo

A memória serve para deixar a conversa mais fluida e reduzir chamadas desnecessárias ao LLM. A primeira implementação é local, persistente e leve para Raspberry Pi.

## Banco atual

O servidor cria um SQLite em:

```text
server/memory/robo_memory.sqlite3
```

Tabelas principais:

- `conversation_turns`: histórico persistente de falas do usuário e respostas do assistente.
- `memories`: fatos/assertivas em formato de grafo simples.
- `memory_links`: arestas explícitas entre memórias, preparada para uso posterior.
- `response_cache`: perguntas e respostas já resolvidas, usadas para resposta rápida por similaridade.

Se o SQLite local tiver FTS5 habilitado, o servidor também cria índices lexicais:

- `conversation_turns_fts`
- `memories_fts`
- `response_cache_fts`

## Modelo híbrido

Cada memória em `memories` tem duas leituras:

- Grafo: `subject`, `predicate`, `object_value`.
- Árvore: `tree_path`.

Exemplo:

```text
subject: ricardo
predicate: nome
object_value: Ricardo
tree_path: /pessoas/ricardo/identidade/nome
```

Isso permite consultar tanto por relação quanto por hierarquia.

## Fluxo de resposta

1. O Cardputer envia áudio.
2. O servidor transcreve.
3. O servidor grava o turno do usuário no SQLite.
4. O servidor tenta extrair fatos simples da fala.
5. Se a pergunta puder ser respondida por fato exato, ele responde sem chamar Ollama.
6. Se não puder, ele busca pergunta/resposta parecida em `response_cache`.
7. Se a similaridade for alta, ele reutiliza a resposta sem chamar Ollama.
8. Se não houver confiança suficiente, ele monta um contexto curto com memórias relevantes.
9. O contexto é enviado ao Ollama junto com o prompt.
10. A resposta do Ollama é salva no cache semântico.
11. A resposta do assistente também é gravada no SQLite.

## Busca híbrida

A busca combina três sinais:

- Lexical: FTS5/BM25 quando disponível.
- Vetorial: embedding local por hashing de tokens, n-gramas e bigramas.
- Operacional: confiança, recência e número de usos.

## Embedding

O embedding vem do `nomic-embed-text` pelo Ollama, com o hashing local como rede de segurança se o Ollama estiver fora.

O hashing sozinho não media significado. Medido em pares reais:

| par | hashing | nomic |
|---|---|---|
| `carro` / `automóvel` | 0,414 | **0,851** |
| `estou com fome` / `quero comer` | 0,012 | **0,588** |
| `gosto de cachorro` / `adoro cães` | **−0,027** | **0,502** |
| `carro azul` / `carro amarelo` | 0,593 | 0,845 |

Repare na última linha: o hashing pontuava frases que se **contradizem** mais alto do que sinônimos. Era um casador lexical, não semântico.

Repare também que nem o nomic separa contradição: duas frases sobre a cor do mesmo carro são topicamente quase idênticas. Embedding mede assunto, não concordância de fato. Isso importa para o cache semântico, discutido adiante.

```bash
export ROBO_EMBED_MODEL="nomic-embed-text"   # vazio = só hashing
export ROBO_EMBED_URL="http://127.0.0.1:11434/api/embeddings"
export ROBO_EMBED_TIMEOUT_S="20"
```

### Migração automática

Vetores de modelos diferentes não são comparáveis: 128 dimensões contra 768 daria cosseno zero e a busca degradaria em silêncio. Por isso cada linha guarda em `embedding_model` de que modelo veio, e o modelo ativo é decidido **uma vez por sessão**.

No arranque, toda linha cujo modelo diverge do ativo é regenerada:

```text
Memória: 42 embedding(s) regenerado(s) para 'nomic-embed-text'.
```

Trocar `ROBO_EMBED_MODEL` e reiniciar é suficiente; nada precisa ser apagado à mão. As buscas ignoram linhas de outro modelo enquanto a regeneração não acontece.

Custo: cerca de 50 ms por embedding e ~16 KB por vetor gravado como JSON.

Decisão prática:

```text
fato exato forte
  -> responde direto
cache semântico com score alto
  -> responde direto
memórias relevantes, mas score insuficiente
  -> envia contexto para Ollama
sem memória útil
  -> Ollama responde normalmente
```

O limiar inicial do cache semântico é `0.88`. Esse valor é conservador para evitar respostas reaproveitadas no contexto errado.

## Extração de fatos

São duas camadas. A regex é rápida e determinística; o LLM cobre o resto.

### Camada 1: regex

Roda antes da resposta, custo zero. Reconhece:

- `meu nome é Ricardo`
- `eu me chamo Ricardo`
- `pode me chamar de Ricardo`
- `eu gosto de ...`
- `eu curto ...`

Confiança 0,95: em conflito, ganha do LLM.

### Camada 2: LLM

Roda **depois** de a resposta e o áudio já terem sido enviados. O Cardputer está tocando a fala nesse momento, então a chamada extra ao LLM não é percebida como espera.

Usa `format: json` do Ollama e temperatura zero. Confiança 0,6.

Sozinha, a extração por LLM é perigosa. Num teste real com `qwen2.5:1.5b`, a fala `"Qual é a capital da França?"` produziu o fato `capital = "Lisboa"`: uma pergunta virou afirmação, o predicado foi inventado, e a resposta ainda estava errada. Três travas impedem isso:

1. **Perguntas são ignoradas.** Texto terminado em `?` ou começando por palavra interrogativa não passa. Pergunta não afirma nada sobre quem fala, e é onde o modelo mais inventa.
2. **Vocabulário fechado.** O predicado precisa existir em `FACT_TREE`; qualquer outro é invenção e é descartado com aviso no console.
3. **Ancoragem no texto.** O valor precisa aparecer na fala do usuário. Se o modelo devolve algo que ninguém disse, ele inventou. Foi esta trava que matou o caso `Lisboa`.

Predicados aceitos: `nome`, `apelido`, `idade`, `cidade`, `estado`, `pais`, `profissao`, `trabalha_em`, `estuda`, `gosta_de`, `nao_gosta_de`, `tem`, `animal_de_estimacao`, `objetivo`.

Os multivalorados (`gosta_de`, `nao_gosta_de`, `tem`, `animal_de_estimacao`, `objetivo`) viram `predicado:valor`, porque `memories` tem `UNIQUE(subject, predicate)` e sem isso o segundo gosto sobrescreveria o primeiro.

Desligar:

```bash
export ROBO_FACT_EXTRACTION="0"
```

### Comportamento verificado

| fala | fatos extraídos |
|---|---|
| `Meu nome e Ricardo e eu moro em Curitiba` | `nome=Ricardo`, `cidade=Curitiba` |
| `Tenho um cachorro chamado Bidu e um gato` | `tem:cachorro`, `tem:gato` |
| `Eu gosto muito de pizza e de futebol` | `gosta_de:pizza`, `gosta_de:futebol` |
| `Qual e a capital da Franca?` | nenhum |
| `Oi robo, tudo bem?` | nenhum |
| `Liga a luz da sala por favor` | nenhum |

Perguntas rápidas já respondidas sem LLM:

- `qual é meu nome?`
- `você sabe meu nome?`
- `como eu me chamo?`
- `quem sou eu?`

## Identidade de voz

Ainda não há biometria de voz real. Nesta versão, todas as falas são atribuídas ao `speaker_id` padrão:

```bash
export ROBO_DEFAULT_SPEAKER_ID="ricardo"
```

Para reconhecimento de voz real, o próximo passo é criar um fluxo de cadastro:

1. gravar algumas amostras de voz por pessoa;
2. gerar embeddings de speaker;
3. salvar os vetores associados ao `speaker_id`;
4. comparar cada nova fala com os vetores cadastrados;
5. usar o `speaker_id` detectado para buscar memórias da pessoa correta.

## Próximos passos recomendados

- Adicionar comandos de console para listar, apagar e corrigir memórias.
- **Revisar o cache semântico.** Com o embedding novo ele passa a disparar de verdade, e aí o limiar de `0.88` fica arriscado: `gosto de pizza` e `gosto de pizzaria` medem 0,943 e são perguntas diferentes. Ou o limiar sobe, ou o cache deixa de valer para pergunta factual.
- Adicionar memória episódica: eventos com data, local e participantes.
- Adicionar confirmação antes de salvar fatos sensíveis.
- Implementar cadastro e reconhecimento de speaker.
- Persistir `valence` e `arousal` por turno em `conversation_turns`, usando o mesmo `ensure_column()` que já faz migração incremental. Isso abriria recuperação enviesada por humor e falas como "você parecia chateado ontem". Ver `sentimento.md`.
