# Workflow

## Preparação

1. No Raspberry, iniciar o servidor:

```bash
cd /home/ricardo/robo/robo/server
bash run_server.sh
```

Esse é o único comando necessário. Na primeira vez ele prepara tudo: pacotes de sistema, venv, dependências, Ollama e modelo. Depois, cada etapa se reconhece pronta e é pulada, então o arranque é imediato.

Confira as linhas do boot antes de seguir:

```text
LLM pronto: 'qwen2.5:1.5b-instruct' em http://127.0.0.1:11434
LLM OK: ... modelo 'qwen2.5:1.5b-instruct' pronto
Beacon UDP transmitindo para: ...
Servidor em ws://0.0.0.0:8765
```

A primeira linha vem do script; a segunda, do próprio servidor conferindo por conta.

Se a primeira disser `LLM INDISPONÍVEL`, as respostas virão do fallback local até isso ser resolvido.

2. No Cardputer, gravar o firmware. Não é preciso editar SSID, senha nem IP: o único valor a conferir antes de gravar é o `ROBOT_SECRET`, que precisa bater com o `ROBO_DISCOVERY_TOKEN` do servidor.
3. Ligar o Cardputer com SD inserido e chave física em `ON`.
4. No primeiro boot a tela de configuração abre sozinha. Escolher a rede e digitar a senha. Depois disso a credencial fica salva na NVS e o passo não se repete; a tecla `W` reabre a tela quando precisar trocar de rede.
5. O Cardputer descobre o servidor por UDP sozinho. O rodapé mostra o endereço resolvido.
6. Confirmar conexão com `P`, que envia `PING` e deve receber `PONG`.

## Ciclo de voz

1. O usuário aperta `R`.
2. O Cardputer começa uma gravação circular em RAM.
3. O firmware captura blocos de 240 amostras em 17 kHz, S16LE, mono.
4. O buffer circular mantém até 384 blocos, cerca de 5,4 segundos.
5. Durante a captura, o firmware evita polling WebSocket e escrita em SD.
6. O usuário aperta `S`.
7. O Cardputer para a gravação.
8. O snapshot é salvo em `SD:/mic_ring.raw`.
9. O Cardputer envia `RECORD_START 17000 1 s16le`.
10. O Cardputer envia os blocos binários do snapshot em ordem cronológica.
11. O Cardputer envia `RECORD_END`.
12. O servidor salva o RAW original em `server/mic_tests/`.
13. O servidor grava um WAV em 17 kHz para inspeção.
14. O servidor converte o WAV para 16 kHz mono com `ffmpeg`.
15. O servidor transcreve o WAV convertido com `faster-whisper`.
16. O servidor envia `MSG Transcricao: ...` ao Cardputer.
17. O servidor chama o Ollama local com o histórico curto da conversa.
18. Se o Ollama falhar, o servidor usa `basic_reply`.
19. O servidor analisa o sentimento da transcrição e envia `EMO <HUMOR>`.
20. O Cardputer muda a forma dos olhos e da boca conforme o humor recebido.
21. O servidor envia `MSG Resposta: ...` ao Cardputer.
22. Se `auto_tts` estiver ligado, o servidor gera fala com Piper.
23. O servidor converte a fala para RAW PCM S16LE mono 16 kHz.
24. O servidor envia `PLAY_START`.
25. O Cardputer entra em `Pensando`, ainda não em `Falando`: nesse ponto o áudio só está sendo recebido.
26. O servidor envia o áudio em chunks de 1024 bytes.
27. O servidor envia `PLAY_END`.
28. O Cardputer salva o áudio em `SD:/rx_audio.raw`.
29. O Cardputer entra em `Falando` e toca o RAW no speaker, com a boca dirigida pelo áudio em janelas de 20 ms.

## Console manual

O console do servidor permite operar o Cardputer sem capturar áudio:

- `msg texto`: exibe texto na tela.
- `say texto`: gera TTS e toca no speaker.
- `auto_tts on`: habilita fala automática após cada resposta.
- `auto_tts off`: desabilita fala automática.
- `ping`: envia `PING`.

## Teclas do Cardputer

- `R` ou `Espaço`: inicia ou para a gravação circular.
- `S`: para a gravação, salva o snapshot e envia ao servidor.
- `P`: envia `PING`.
- `W`: abre a configuração de rede.

## Estado persistente

O servidor mantém:

- o modelo `faster-whisper` carregado uma única vez no início;
- um histórico curto de conversa em memória;
- a última conexão WebSocket ativa;
- arquivos de captura em `server/mic_tests/`;
- arquivos gerados de TTS em `server/tts_out/`;
- memória de longo prazo em `server/memory/robo_memory.sqlite3`.

O Cardputer mantém:

- buffer circular de microfone em RAM;
- SSID, senha e URL manual do servidor na NVS;
- `SD:/mic_ring.raw` com a última captura enviada;
- `SD:/rx_audio.raw` com o último áudio recebido do servidor.

## Contratos atuais

- Host do servidor: `0.0.0.0`
- Porta WebSocket: `8765`
- Porta UDP de descoberta: `8766`, nos dois lados
- Entrada do microfone: PCM S16LE, mono, 17000 Hz
- Entrada do Whisper: WAV mono, 16000 Hz
- Saída de TTS para Cardputer: PCM S16LE, mono, 16000 Hz
- Chunk de envio de TTS: 1024 bytes
- Chunk de playback no Cardputer: 1920 amostras (120 ms)
- Janela de visema: 320 amostras (20 ms)
- Modelo Whisper: `base`, CPU, `int8`
- Modelo Ollama padrão: `qwen2.5:1.5b-instruct`
- Voz Piper: `server/voices/pt_BR-faber-medium.onnx`

## Meta do fluxo

A prioridade atual é confiabilidade do ciclo completo:

- captar em RAM;
- transmitir em chunks pequenos;
- salvar evidências no servidor;
- transcrever;
- responder;
- sintetizar quando necessário;
- reproduzir no Cardputer sem travar o firmware.
