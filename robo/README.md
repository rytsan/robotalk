# Robo

Projeto experimental que integra um Raspberry Pi 5 com um M5Stack Cardputer por Wi-Fi e WebSocket. O Raspberry funciona como cérebro do sistema. O Cardputer funciona como interface física com tela, teclado, microfone, speaker e SD.

## Status atual

O ciclo principal já está implementado de ponta a ponta:

1. O Cardputer captura áudio do microfone em RAM.
2. O usuário salva e envia o snapshot pelo WebSocket.
3. O servidor salva os arquivos de áudio, converte para Whisper e transcreve.
4. O texto transcrito é enviado para o Ollama local.
5. O servidor envia a transcrição e a resposta ao Cardputer.
6. Se `auto_tts` estiver ligado, o servidor sintetiza a resposta com Piper e manda o PCM de volta.
7. O Cardputer salva o áudio recebido no SD e toca no speaker.

## Componentes

- `server/robo_server.py`: servidor Python persistente no Raspberry.
- `cardputer/firmware/cardputer_robot_lab/cardputer_robot_lab.ino`: firmware do Cardputer.
- `server/voices/`: voz Piper usada para TTS.
- `server/mic_tests/`: capturas de microfone salvas pelo servidor.
- `server/tts_out/`: textos, WAVs e RAWs gerados pelo Piper.
- `docs/`: notas de arquitetura, workflow, setup e troubleshooting.
- `scripts/`: utilitários para conversão e testes isolados.

## Servidor

Instale o ambiente no Raspberry:

```bash
cd /home/ricardo/robo/robo/server
bash install_server.sh
```

Rode o servidor:

```bash
cd /home/ricardo/robo/robo/server
bash run_server.sh
```

O servidor escuta em:

```text
ws://0.0.0.0:8765
```

Ele também suporta UDP discovery na porta `8766`, para firmware com descoberta dinâmica:

```text
RDISCOVER <nonce> -> ROBOT ws://<ip_do_raspberry>:8765 <hmac>
```

Mais detalhes: `docs/discovery.md`.

No firmware, configure `WS_URL` com o IP real do Raspberry, por exemplo:

```cpp
const char* WS_URL = "ws://192.168.1.100:8765";
```

## Console do servidor

Com o Cardputer conectado, o console aceita:

- `msg texto`: manda uma mensagem de texto para a tela do Cardputer.
- `say texto`: gera fala com Piper e toca no Cardputer.
- `auto_tts on`: liga resposta falada automática depois da transcrição.
- `auto_tts off`: deixa apenas texto/transcrição sem fala automática.
- `ping`: manda `PING` ao Cardputer.

No código atual, `AUTO_TTS_REPLY` começa como `True`.

## Cardputer

Há dois sketches no workspace:

- `tts/cardputer_assistente.ino`: aparenta ser o firmware mais novo. O cabeçalho marca `v2.0`, cita correção de memória do Cardputer sem PSRAM, usa playback pelo SD em chunks e tem interface animada.
- `robo/cardputer/firmware/cardputer_robot_lab/cardputer_robot_lab.ino`: firmware mais simples, documentado inicialmente dentro do projeto `robo/`.

Para retomar do ponto mais provável do último desenvolvimento, comece por:

```text
/home/ricardo/robo/tts/cardputer_assistente.ino
```

Se quiser uma versão mínima para depurar protocolo e áudio, use:

```text
/home/ricardo/robo/robo/cardputer/firmware/cardputer_robot_lab/cardputer_robot_lab.ino
```

Antes de gravar, ajuste:

- `WIFI_SSID`
- `WIFI_PASS`
- `WS_URL`

Bibliotecas/pacotes no Arduino IDE:

- Boards Manager URL: `https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json`
- Pacote de placas: `M5Stack`, versão `3.2.2`
- Placa: `M5Cardputer`
- Bibliotecas: `M5Cardputer`, `M5Unified`, `M5GFX`, `ArduinoWebsockets`

Controles no teclado:

- `R`: inicia ou para a gravação circular em RAM.
- `S`: quando a gravação está ativa, para, salva `SD:/mic_ring.raw` e envia ao servidor.
- `P`: envia `PING`.

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

## Protocolo WebSocket

Mensagens de texto:

- `PING` / `PONG`: teste de conexão.
- `RECORD_START 17000 1 s16le`: início de envio de áudio capturado.
- `RECORD_END`: fim do envio de áudio capturado.
- `RECORDING`: confirmação do servidor.
- `MSG ...`: texto mostrado na tela do Cardputer.
- `SAY ...`: comando que pode ser enviado pelo Cardputer ao servidor para TTS direto.
- `PLAY_START` / `PLAY_END`: início e fim de áudio TTS enviado pelo servidor.

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

Instalação mínima do modelo:

```bash
ollama pull qwen2.5:1.5b-instruct
```

Se o Ollama falhar, o servidor usa um fallback local simples para manter o fluxo funcionando.

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
