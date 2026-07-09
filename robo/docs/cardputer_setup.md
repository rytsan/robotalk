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

Há dois sketches no workspace. O mais provável de ser o último firmware desenvolvido é:

- `/home/ricardo/robo/tts/cardputer_assistente.ino`

Motivos:

- tem cabeçalho `v2.0`;
- menciona correção de memória para Cardputer sem PSRAM;
- toca áudio recebido lendo do SD em chunks;
- tem timestamp local mais recente que o firmware simples em `robo/cardputer/`.

O firmware simples, útil para depuração, fica em:

- `cardputer/firmware/cardputer_robot_lab/cardputer_robot_lab.ino`

Abra um desses arquivos diretamente na Arduino IDE.

### 7. Ajustar a rede

No firmware, troque os valores de:

- SSID do Wi-Fi;
- senha;
- IP do Raspberry Pi.

## Aviso importante

Não atualize o pacote M5Stack para `3.3.7` enquanto o microfone estiver sendo usado, porque essa versão quebrou o mic neste projeto.
