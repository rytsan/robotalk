# Arquitetura

## Divisão de responsabilidades

### Raspberry Pi = cérebro

O Raspberry Pi 5 executa o servidor principal do projeto. Ele recebe áudio do Cardputer, salva os arquivos de captura, roda a transcrição com `faster-whisper`, decide a resposta textual e gera a fala com Piper.

### Cardputer = interface física

O M5Stack Cardputer fornece a interface local do robô: tela, teclado, speaker e, quando viável, microfone. Ele também faz a ponte com o Raspberry via Wi-Fi.

## WebSocket

A comunicação entre os dois lados usa WebSocket porque ela mantém uma conexão persistente, simples e bidirecional. Isso ajuda no envio de texto, comandos de controle e blocos de áudio sem abrir e fechar conexões a cada troca.

## Protocolo

Os comandos são propositalmente simples:

- `PING` / `PONG`: verificação de conectividade e latência.
- `PLAY_START` / `PLAY_END`: delimitam uma sessão de reprodução de áudio no Cardputer.
- `RECORD_START` / `RECORD_END`: delimitam uma sessão de gravação no Cardputer.
- `MSG`: mensagem de texto normal, usada para comandos e respostas curtas.

A implementação final pode evoluir, mas a ideia é manter o protocolo pequeno e fácil de depurar.

## Formato de áudio

### Saída para o Cardputer

- PCM S16LE
- mono
- 16000 Hz

Esse é o formato esperado para reprodução no speaker do Cardputer.

### Entrada do microfone do Cardputer

- PCM S16LE
- mono
- 17000 Hz

Esse foi o formato que funcionou melhor nos testes do microfone do Cardputer neste projeto.
