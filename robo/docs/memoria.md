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

O embedding atual é propositalmente leve e offline. Ele não é um embedding neural, mas já permite comparar proximidade entre frases sem instalar pacotes pesados no Raspberry. A função fica em `server/memory_store.py` e pode ser trocada depois por embeddings de um modelo local.

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

## Fatos reconhecidos nesta versão

Atualmente a extração local reconhece frases simples:

- `meu nome é Ricardo`
- `eu me chamo Ricardo`
- `pode me chamar de Ricardo`
- `eu gosto de ...`
- `eu curto ...`

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
- Usar o Ollama para extrair fatos estruturados em JSON depois de cada conversa.
- Trocar o embedding por um modelo neural local quando o Raspberry aguentar.
- Adicionar memória episódica: eventos com data, local e participantes.
- Adicionar confirmação antes de salvar fatos sensíveis.
- Implementar cadastro e reconhecimento de speaker.
- Persistir `valence` e `arousal` por turno em `conversation_turns`, usando o mesmo `ensure_column()` que já faz migração incremental. Isso abriria recuperação enviesada por humor e falas como "você parecia chateado ontem". Ver `sentimento.md`.
