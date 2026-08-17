# Configuração do Cardputer no Arduino IDE

## Passo a passo

### 1. Instalar o Arduino IDE

Baixe e instale o Arduino IDE na máquina de desenvolvimento.

### 2. Adicionar a URL do gerenciador de placas

No Arduino IDE, abra as preferências e adicione esta URL em `URLs adicionais para Gerenciadores de Placas`:

```text
https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
```

### 3. Instalar o pacote M5Stack

Abra o gerenciador de placas e instale o pacote **M5Stack** na versão **3.2.2**.

### 4. Selecionar a placa correta

Na IDE, selecione a placa:

- `M5Cardputer`

### 5. Instalar bibliotecas

Instale as bibliotecas necessárias:

- `M5Cardputer`
- `M5Unified`
- `M5GFX`
- `ArduinoWebsockets`

Se a IDE pedir dependências adicionais, instale também as sugeridas para manter o projeto compilando.

### 6. Abrir o firmware

O firmware principal do projeto é:

- `tts/cardputer_assistente.ino`

É ele que tem o rosto animado, o lip sync por visema, a tela de configuração de rede e a descoberta do servidor. O firmware simples, útil para depurar protocolo e áudio isoladamente, fica em:

- `robo/cardputer/firmware/cardputer_robot_lab/cardputer_robot_lab.ino`

Abra um desses arquivos diretamente na Arduino IDE.

### 7. Ajustar a rede

**No firmware principal, não é mais necessário editar o código para trocar de rede.** SSID e senha são configurados no próprio Cardputer, pelo teclado, e ficam salvos na NVS.

Se você abriu o firmware mínimo (`cardputer_robot_lab.ino`), ele não tem essa tela: lá o `WIFI_SSID`, o `WIFI_PASS` e o `WS_URL` ainda precisam ser editados no topo do arquivo antes de gravar.

No primeiro boot, se nunca houve configuração salva e o `WIFI_SSID` ainda for o placeholder `SUA_REDE`, o firmware abre a tela de configuração sozinho. Depois disso, a tecla `W` reabre a tela a qualquer momento.

Os `#define WIFI_SSID` / `WIFI_PASS` / `WS_URL` continuam no código, mas passaram a ser apenas padrão de fábrica: o que estiver salvo na NVS sempre vence.

O único valor que ainda precisa ser conferido antes de gravar é o segredo compartilhado:

```cpp
#define ROBOT_SECRET "TROQUE_ESTE_SEGREDO_COMPARTILHADO"
```

Ele precisa ser idêntico ao `ROBO_DISCOVERY_TOKEN` do servidor, senão a descoberta é rejeitada em silêncio e o Cardputer não acha o Raspberry. Ver `discovery.md`.

### 8. Configurar a rede no aparelho

Na tela de configuração:

| Tecla | Ação |
|---|---|
| `;` | sobe na lista |
| `.` | desce na lista |
| `Enter` | confirma |
| `Ctrl` | volta |
| `Del` | apaga; com o campo vazio, volta |

O menu tem seis itens:

- **Escolher rede Wi-Fi**: escaneia e lista as redes ao alcance. `*` marca rede com senha, as barras mostram o sinal.
- **Conectar agora**: reconecta usando a credencial já salva, sem re-escolher a rede nem redigitar a senha. É o item para quando o roteador estava desligado no boot.
- **Rede oculta (digitar SSID)**: para redes que não aparecem no scan.
- **Servidor**: `auto` usa descoberta por UDP; preencher com uma URL (`ws://192.168.0.50:8765`) força um endereço fixo. Deixe vazio para voltar ao automático.
- **Esquecer rede salva**: limpa a credencial da NVS.
- **Sair**: fecha e tenta conectar.

No rodapé do menu aparece o estado da rede:

```text
Salva:  robo-net
Online: 10.42.0.7 (robo-net)      <- verde
Offline                            <- vermelho
```

`Salva` é o que está na NVS; `Online` é a conexão real no momento. Os dois podem divergir, por exemplo logo depois de escolher uma rede nova que ainda não conectou.

Ao escolher uma rede que **já é a salva**, o campo de senha vem preenchido com a senha conhecida e o título mostra `(salva)`. Basta `Enter` para reconectar; para trocar a senha, apague com `Del` e digite a nova.

A senha aparece em texto claro na tela, de propósito: digitar às cegas neste teclado gera mais erro do que o mascaramento evita.

A credencial só é gravada **depois** de a conexão funcionar. Errar a senha não apaga uma configuração que já estava boa.

## Avisos importantes

Não atualize o pacote M5Stack para `3.3.7` enquanto o microfone estiver sendo usado, porque essa versão quebrou o mic neste projeto.

O rosto do robô usa dois canais independentes: a **forma** dos olhos e da boca mostra a emoção detectada no usuário, e a **cor** mostra o estado do robô. Ver `rosto.md`.
