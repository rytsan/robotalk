# Cardputer

## Firmware

Há dois sketches no workspace:

- `/home/ricardo/robo/tts/cardputer_assistente.ino`
- `/home/ricardo/robo/robo/cardputer/firmware/cardputer_robot_lab/cardputer_robot_lab.ino`

O `tts/cardputer_assistente.ino` aparenta ser o mais novo: o cabeçalho marca `v2.0`, menciona correção de memória para Cardputer sem PSRAM e salva/toca áudio pelo SD em chunks. O `cardputer_robot_lab.ino` é a versão mais simples e serve melhor para depuração.

## O que ajustar

Antes de compilar e gravar, troque no firmware:

- SSID do Wi-Fi;
- senha do Wi-Fi;
- IP do Raspberry Pi;
- qualquer porta ou endpoint usado na conexão WebSocket.

## Controles do firmware

- `R`: inicia ou para a gravação circular em RAM.
- `S`: para a gravação, salva `SD:/mic_ring.raw` e envia os blocos ao servidor.
- `P`: envia `PING` para testar a conexão.

Quando o Raspberry responder com áudio, o firmware salva em `SD:/rx_audio.raw` e toca no speaker.

## Dependências lembradas

- `M5Cardputer`
- `M5Unified`
- `M5GFX`
- `ArduinoWebsockets`

## Observação

Use o pacote de placas M5Stack na versão `3.2.2` enquanto o microfone estiver em uso. Não subir para `3.3.7` neste momento.
