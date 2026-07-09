# Workflow

## Preparação

1. No Raspberry, iniciar o servidor:

```bash
cd /home/ricardo/robo/robo/server
bash run_server.sh
```

2. No Cardputer, gravar o firmware com Wi-Fi e `WS_URL` apontando para o IP do Raspberry.
3. Ligar o Cardputer com SD inserido e chave física em `ON`.
4. Confirmar conexão com `P`, que envia `PING` e deve receber `PONG`.

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
19. O servidor envia `MSG Resposta: ...` ao Cardputer.
20. Se `auto_tts` estiver ligado, o servidor gera fala com Piper.
21. O servidor converte a fala para RAW PCM S16LE mono 16 kHz.
22. O servidor envia `PLAY_START`.
23. O servidor envia o áudio em chunks de 1024 bytes.
24. O servidor envia `PLAY_END`.
25. O Cardputer salva o áudio em `SD:/rx_audio.raw`.
26. O Cardputer toca o RAW no speaker.

## Console manual

O console do servidor permite operar o Cardputer sem capturar áudio:

- `msg texto`: exibe texto na tela.
- `say texto`: gera TTS e toca no speaker.
- `auto_tts on`: habilita fala automática após cada resposta.
- `auto_tts off`: desabilita fala automática.
- `ping`: envia `PING`.

## Estado persistente

O servidor mantém:

- o modelo `faster-whisper` carregado uma única vez no início;
- um histórico curto de conversa em memória;
- a última conexão WebSocket ativa;
- arquivos de captura em `server/mic_tests/`;
- arquivos gerados de TTS em `server/tts_out/`.

O Cardputer mantém:

- buffer circular de microfone em RAM;
- `SD:/mic_ring.raw` com a última captura enviada;
- `SD:/rx_audio.raw` com o último áudio recebido do servidor.

## Contratos atuais

- Host do servidor: `0.0.0.0`
- Porta WebSocket: `8765`
- Entrada do microfone: PCM S16LE, mono, 17000 Hz
- Entrada do Whisper: WAV mono, 16000 Hz
- Saída de TTS para Cardputer: PCM S16LE, mono, 16000 Hz
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
