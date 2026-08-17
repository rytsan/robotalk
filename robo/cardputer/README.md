# Cardputer

## Firmware

Há **um** firmware:

```text
tts/cardputer_assistente/cardputer_assistente.ino
```

Rosto animado, lip sync por visema, configuração de rede pelo teclado e descoberta do servidor.

Repare na inversão: o firmware mora na pasta `tts/`, que é legada para o resto do projeto. Os `.py` daquela pasta são protótipos já consolidados em `robo/server/`.

Existiu um segundo sketch, `cardputer_robot_lab.ino`, como versão mínima de depuração. Foi removido: ele não tinha rosto nem tela de configuração, e a semelhança de nome levava a gravar o firmware errado e concluir que o projeto estava quebrado. Está no histórico do git se algum dia fizer falta.

## O que ajustar antes de gravar

Só o segredo compartilhado:

```cpp
#define ROBOT_SECRET "TROQUE_ESTE_SEGREDO_COMPARTILHADO"
```

Ele precisa ser idêntico ao `ROBO_DISCOVERY_TOKEN` do servidor. Se divergir, o Cardputer rejeita a resposta da descoberta em silêncio e nunca acha o Raspberry.

SSID, senha e endereço do servidor **não** são editados no código: ficam na NVS e são configurados no próprio aparelho, pela tecla `W`.

## Controles do firmware

- `R` ou `Espaço`: inicia ou para a gravação circular em RAM.
- `S`: para a gravação, salva `SD:/mic_ring.raw` e envia os blocos ao servidor.
- `P`: envia `PING` para testar a conexão.
- `W`: abre a configuração de rede.

Quando o Raspberry responder com áudio, o firmware salva em `SD:/rx_audio.raw` e toca no speaker.

## Configuração de rede (tecla `W`)

| Tecla | Ação |
|---|---|
| `;` | sobe |
| `.` | desce |
| `Enter` | confirma |
| `Ctrl` | volta |
| `Del` | apaga; com o campo vazio, volta |

O menu permite escolher uma rede do scan, reconectar com a credencial salva, digitar um SSID oculto, forçar uma URL de servidor, esquecer a rede salva ou sair. O rodapé mostra a rede salva e o estado real da conexão, com o IP quando online.

No primeiro boot, se não houver nada salvo e o `WIFI_SSID` ainda for o placeholder, a tela abre sozinha.

A senha só é gravada depois de a conexão funcionar, então errar a senha não apaga uma configuração boa. Escolher de novo a rede que já está salva traz o campo de senha preenchido.

## Dependências lembradas

- `M5Cardputer`
- `M5Unified`
- `M5GFX`
- `ArduinoWebsockets`
- `Preferences` e `mbedtls` já vêm no core ESP32

## Observação

Use o pacote de placas M5Stack na versão `3.2.2` enquanto o microfone estiver em uso. Não subir para `3.3.7` neste momento.

## Documentação relacionada

- `robo/docs/cardputer_setup.md` — passo a passo da Arduino IDE e da configuração
- `robo/docs/rosto.md` — canais do rosto e lip sync
- `robo/docs/discovery.md` — descoberta do servidor
- `robo/docs/troubleshooting.md` — problemas já conhecidos
