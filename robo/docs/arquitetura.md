# Arquitetura

## Divisão de responsabilidades

### Raspberry Pi = cérebro

O Raspberry Pi 5 executa o servidor principal do projeto. Ele recebe áudio do Cardputer, salva os arquivos de captura, roda a transcrição com `faster-whisper`, decide a resposta textual e gera a fala com Piper.

Ele também analisa o sentimento da fala do usuário e informa ao Cardputer qual emoção mostrar. Ver `sentimento.md`.

### Cardputer = interface física

O M5Stack Cardputer fornece a interface local do robô: tela, teclado, speaker e, quando viável, microfone. Ele também faz a ponte com o Raspberry via Wi-Fi.

O Cardputer é autônomo em duas coisas: a configuração de rede (SSID e senha digitados no teclado, salvos na NVS) e o estado visual do robô (`Pronto`, `Ouvindo`, `Pensando`, `Falando`, `ERRO`), que ele deriva sozinho do fluxo. O servidor não precisa comandar nenhum dos dois.

## WebSocket

A comunicação entre os dois lados usa WebSocket porque ela mantém uma conexão persistente, simples e bidirecional. Isso ajuda no envio de texto, comandos de controle e blocos de áudio sem abrir e fechar conexões a cada troca.

## Protocolo

Os comandos são propositalmente simples:

- `PING` / `PONG`: verificação de conectividade e latência.
- `PLAY_START` / `PLAY_END`: delimitam uma sessão de reprodução de áudio no Cardputer.
- `RECORD_START` / `RECORD_END`: delimitam uma sessão de gravação no Cardputer.
- `RECORDING`: confirmação do servidor de que a captura começou.
- `MSG`: mensagem de texto normal, usada para comandos e respostas curtas.
- `EMO <HUMOR>`: emoção detectada na fala do usuário; muda a forma do rosto.
- `SAY <texto>`: pedido de TTS direto, sem passar por captura de áudio.
- `HELLO_CARDPUTER` / `HELLO_ROBO`: handshake opcional com prova do segredo.

A implementação final pode evoluir, mas a ideia é manter o protocolo pequeno e fácil de depurar.

### Sobre `EMO` e `STATE`

`EMO` carrega apenas o rótulo do humor: `NEUTRAL`, `HAPPY`, `SAD`, `CONFUSED`, `EXCITED`, `CONCERNED`. O parser do firmware compara o rótulo por igualdade exata, então acrescentar um argumento (`EMO HAPPY 0.8`) quebraria a leitura em silêncio e cairia em `NEUTRAL`.

Existe também um verbo `STATE <ESTADO>` implementado no firmware, mas o servidor nunca o envia: o estado é derivado localmente. Ele fica disponível para forçar um estado durante depuração.

## Dois canais no rosto

A forma dos olhos e da boca mostra a emoção do **usuário**; a cor do rosto mostra o estado do **robô**. São canais independentes de propósito, para que dê para ler as duas coisas ao mesmo tempo. Detalhes em `rosto.md`.

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
