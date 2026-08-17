# Robo

Projeto experimental que integra um Raspberry Pi 5 com um M5Stack Cardputer por Wi-Fi e WebSocket. O Raspberry funciona como cérebro do sistema. O Cardputer funciona como interface física com tela, teclado, microfone, speaker e SD.

## Status atual

O ciclo principal já está implementado de ponta a ponta:

1. O Cardputer captura áudio do microfone em RAM.
2. O usuário salva e envia o snapshot pelo WebSocket.
3. O servidor salva os arquivos de áudio, converte para Whisper e transcreve.
4. O texto transcrito é enviado para o Ollama local.
5. O servidor analisa o sentimento da fala e manda a emoção para o rosto.
6. O servidor envia a transcrição e a resposta ao Cardputer.
7. Se `auto_tts` estiver ligado, o servidor sintetiza a resposta com Piper e manda o PCM de volta.
8. O Cardputer salva o áudio recebido no SD e toca no speaker, com a boca dirigida pelo áudio.

## Componentes

- `server/robo_server.py`: servidor Python persistente no Raspberry.
- `server/memory_store.py`: memória persistente híbrida em SQLite.
- `server/sentiment.py`: análise de sentimento da fala do usuário.
- `server/lib_ollama.sh`: funções de instalação e verificação do LLM, usadas pelos dois scripts.
- `../tts/cardputer_assistente/cardputer_assistente.ino`: firmware do Cardputer.
- `server/voices/`: voz Piper usada para TTS.
- `server/mic_tests/`: capturas de microfone salvas pelo servidor.
- `server/tts_out/`: textos, WAVs e RAWs gerados pelo Piper.
- `docs/`: notas de arquitetura, workflow, setup, rosto, sentimento, memória, discovery e troubleshooting.
- `scripts/`: utilitários para conversão e testes isolados.

## Servidor

Um comando só, no Raspberry:

```bash
cd /home/ricardo/robo/robo/server
bash run_server.sh
```

Na primeira vez ele prepara tudo sozinho: pacotes de sistema, venv, dependências Python, Ollama e o modelo. Nas vezes seguintes cada etapa se reconhece pronta e é pulada, então o arranque é imediato — nada é reinstalado nem rebaixado.

O `install_server.sh` continua existindo e pode ser chamado direto, mas só é necessário se você quiser rodar a preparação sem subir o servidor.

O servidor escuta em:

```text
ws://0.0.0.0:8765
```

Ele também faz descoberta por UDP na porta `8766`, em duas vias:

```text
ativa:   RDISCOVER <nonce> -> ROBOT ws://<ip_do_raspberry>:8765 <hmac>
passiva: beacon periodico em broadcast, ligado por padrao
```

Mais detalhes: `docs/discovery.md`.

Não é preciso configurar IP no firmware. O Cardputer descobre o servidor sozinho; o `WS_URL` compilado é apenas o último recurso.

O que precisa bater entre os dois lados é o segredo compartilhado: `ROBOT_SECRET` no `.ino` e `ROBO_DISCOVERY_TOKEN` no servidor.

## Console do servidor

Com o Cardputer conectado, o console aceita:

- `msg texto`: manda uma mensagem de texto para a tela do Cardputer.
- `say texto`: gera fala com Piper e toca no Cardputer.
- `auto_tts on`: liga resposta falada automática depois da transcrição.
- `auto_tts off`: deixa apenas texto/transcrição sem fala automática.
- `ping`: manda `PING` ao Cardputer.

No código atual, `AUTO_TTS_REPLY` começa como `True`.

## Cardputer

O firmware é um só:

```text
tts/cardputer_assistente/cardputer_assistente.ino
```

Rosto animado, lip sync por visema, configuração de rede pelo teclado e descoberta do servidor. Note que ele fica em `tts/`, e não em `robo/cardputer/`.

Antes de gravar, o único valor a conferir é o segredo compartilhado:

```cpp
#define ROBOT_SECRET "TROQUE_ESTE_SEGREDO_COMPARTILHADO"
```

SSID, senha e endereço do servidor são configurados no próprio aparelho, pela tecla `W`, e ficam salvos na NVS. No primeiro boot a tela abre sozinha. Detalhes em `docs/cardputer_setup.md`.

Bibliotecas/pacotes no Arduino IDE:

- Boards Manager URL: `https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json`
- Pacote de placas: `M5Stack`, versão `3.2.2`
- Placa: `M5Cardputer`
- Bibliotecas: `M5Cardputer`, `M5Unified`, `M5GFX`, `ArduinoWebsockets`

Controles no teclado:

- `R` ou `Espaço`: inicia ou para a gravação circular em RAM.
- `S`: quando a gravação está ativa, para, salva `SD:/mic_ring.raw` e envia ao servidor.
- `P`: envia `PING`.
- `W`: abre a configuração de rede.

## Rosto

O rosto usa dois canais independentes:

- **forma** dos olhos e da boca: emoção detectada no usuário, recebida via `EMO`;
- **cor** do rosto: estado do robô (`Pronto`, `Ouvindo`, `Pensando`, `Falando`, `ERRO`), derivado localmente.

Durante a fala, a boca é dirigida pelo áudio em janelas de 20 ms, usando energia e taxa de cruzamento por zero para escolher entre seis visemas. Mais detalhes: `docs/rosto.md`.

## Análise de sentimento

O servidor classifica a fala do usuário e envia `EMO <HUMOR>` ao Cardputer. A classificação usa léxico português embutido, com valência contínua, arousal separado, negação e intensificadores. Sem dependência nova e sem latência extra.

Para ampliar o vocabulário com um léxico externo:

```bash
export ROBO_SENTIMENT_LEXICON="/caminho/oplexicon_v3.0.txt"
```

Mais detalhes: `docs/sentimento.md`.

## Formatos de áudio

Entrada do Cardputer para o servidor:

- PCM S16LE
- mono
- 17000 Hz
- blocos de 240 amostras, 480 bytes por frame
- buffer circular de 384 blocos, cerca de 5,4 segundos

Saída do servidor para o Cardputer:

- PCM S16LE
- mono
- 16000 Hz
- chunks de 1024 bytes
- delimitado por `PLAY_START` e `PLAY_END`

No Cardputer, a reprodução lê do SD em blocos de 1920 amostras (120 ms), com buffer duplo para não deixar buraco entre blocos.

## Protocolo WebSocket

Mensagens de texto:

- `PING` / `PONG`: teste de conexão.
- `RECORD_START 17000 1 s16le`: início de envio de áudio capturado.
- `RECORD_END`: fim do envio de áudio capturado.
- `RECORDING`: confirmação do servidor.
- `MSG ...`: texto mostrado na tela do Cardputer.
- `EMO <HUMOR>`: emoção detectada na fala do usuário; muda a forma do rosto.
- `SAY ...`: comando que pode ser enviado pelo Cardputer ao servidor para TTS direto.
- `PLAY_START` / `PLAY_END`: início e fim de áudio TTS enviado pelo servidor.
- `HELLO_CARDPUTER` / `HELLO_ROBO`: handshake opcional com prova do segredo.

Humores aceitos em `EMO`: `NEUTRAL`, `HAPPY`, `SAD`, `CONFUSED`, `EXCITED`, `CONCERNED`. O rótulo é comparado por igualdade exata, então acrescentar argumentos quebraria a leitura.

Mensagens binárias:

- Entre `RECORD_START` e `RECORD_END`, o Cardputer envia blocos de áudio do microfone.
- Entre `PLAY_START` e `PLAY_END`, o servidor envia blocos de áudio para reprodução.

## LLM com Ollama

O servidor tenta usar Ollama local por padrão:

- modelo: `qwen2.5:1.5b-instruct`
- endpoint: `http://127.0.0.1:11434/api/chat`
- timeout: `30` segundos

Variáveis de ambiente:

```bash
export ROBO_OLLAMA_BASE_URL="http://127.0.0.1:11434/api/chat"
export ROBO_OLLAMA_MODEL="qwen2.5:1.5b-instruct"
export ROBO_OLLAMA_TIMEOUT_S="30"
```

O `run_server.sh` cuida disso sozinho, a cada arranque: instala o Ollama se não houver, sobe o serviço se estiver parado e baixa o modelo se faltar. Tudo idempotente — numa máquina já preparada custa alguns milissegundos.

Para usar um modelo que já esteja instalado, em vez de baixar outro:

```bash
ROBO_OLLAMA_MODEL="qwen2.5:1.5b" bash run_server.sh
```

Para desativar cada etapa automática:

```bash
ROBO_INSTALL_OLLAMA=0    # não instala o Ollama
ROBO_PULL_MODEL=0        # não baixa modelo
```

Atenção ao nome exato: `qwen2.5:1.5b` e `qwen2.5:1.5b-instruct` são modelos diferentes. Pedir um que não existe devolve HTTP 404 e o servidor cai no fallback.

### Comparação medida

Testados com o system prompt real do robô, nesta ordem de preferência:

| modelo | tamanho | português | extração JSON | velocidade |
|---|---|---|---|---|
| `cnmoro/gemma3-gaia-ptbr-4b:q4_k_m` | 2,5 GB | natural e conciso | 5/5 | mais rápido |
| `qwen2.5:7b-instruct` | 4,7 GB | correto, prolixo | 4/5 | médio |
| `qwen2.5:1.5b` | 986 MB | erros de concordância | 5/5 | médio |
| `qwen3.5:2b` | 2,7 GB | — | — | inutilizável |

O `qwen3.5` é modelo de raciocínio e devolve conteúdo vazio pela API de chat; não serve aqui sem tratamento à parte.

O gemma afinado para pt-BR responde de forma mais natural e mais curta, que é o que este robô pede, e ainda foi o mais rápido. Ele responde com emoji: o servidor remove antes de mandar para a tela e para o Piper, porque o Cardputer usa fonte ASCII e o Piper não fala emoji.

### Data e hora

Todo modelo testado inventava um horário quando perguntado, sem avisar. O Raspberry tem relógio, então o servidor injeta data e hora em cada turno:

```text
Agora são 15:55 de segunda-feira, 17 de agosto de 2026.
```

O prompt também diz que o robô não tem internet nem sensores externos e deve admitir quando não sabe. Com isso, `Qual a previsão do tempo amanhã?` passa a ser respondido com "não tenho essa informação" em vez de um palpite inventado.

Perguntas sobre data e hora **não entram no cache semântico** e não são respondidas por ele. Sem essa exceção, a hora de agora ficaria gravada e seria devolvida horas depois como se ainda valesse.

Se o Ollama falhar, o servidor usa um fallback local simples para manter o fluxo funcionando. Para saber qual dos dois está em uso, digite `llm` no console ou olhe a linha `Resposta via ...` de cada resposta.

## Memória persistente

O servidor mantém uma memória local em SQLite:

```text
server/memory/robo_memory.sqlite3
```

Ela guarda histórico de conversa e fatos simples em um modelo híbrido:

- grafo: `subject`, `predicate`, `object_value`;
- árvore: `tree_path`.
- lexical: FTS5 quando disponível;
- vetorial: embedding local leve por hashing.

Nesta primeira versão, perguntas como `qual é meu nome?` podem ser respondidas direto pela memória, sem chamar Ollama, depois que o usuário disser algo como `meu nome é Ricardo`.

Também existe um cache semântico de pergunta/resposta: se uma fala nova for muito próxima de uma pergunta já respondida, o servidor pode reutilizar a resposta sem chamar Ollama.

O `speaker_id` padrão pode ser ajustado com:

```bash
export ROBO_DEFAULT_SPEAKER_ID="ricardo"
```

Mais detalhes: `docs/memoria.md`.

## Testes úteis

Converter WAV para RAW:

```bash
cd /home/ricardo/robo/robo/scripts
bash convert_wav_to_raw.sh arquivo.wav saida.raw
```

Testar Piper:

```bash
cd /home/ricardo/robo/robo/scripts
bash test_piper.sh
```

Testar Whisper:

```bash
cd /home/ricardo/robo/robo/scripts
bash test_whisper_latest.sh
```

## Pontos importantes

- Usar Arduino IDE com `M5Cardputer`, `M5Unified`, `M5GFX` e `ArduinoWebsockets`.
- Manter o pacote de placas M5Stack em `3.2.2` enquanto o microfone estiver em uso.
- Não usar `3.3.7` neste fluxo, porque essa versão quebrou o microfone nos testes.
- Durante captura do microfone, o firmware evita Wi-Fi e SD para reduzir perda de amostras.
- O fluxo é tratado como half-duplex: gravar e reproduzir em momentos separados.
- Frames grandes por WebSocket podem reiniciar o Cardputer; o TTS usa chunks de 1024 bytes.
