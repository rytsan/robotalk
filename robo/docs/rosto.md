# Rosto e animação

O rosto vive no firmware `tts/cardputer_assistente/cardputer_assistente.ino`. Tela de 240x135, cabeça desenhada uma vez e atualizada por regiões sujas (olhos, boca, rodapé, bateria).

## Dois canais independentes

Esta é a decisão central do desenho:

| Canal | O que mostra | De onde vem |
|---|---|---|
| **Forma** dos olhos e da boca | emoção do **usuário** | `EMO` enviado pelo servidor |
| **Cor** do rosto | estado do **robô** | `currentState`, derivado localmente |

Separar os dois foi necessário porque antes o humor sequestrava a cor: com `EMO HAPPY` ativo, `IDLE`, `LISTENING` e `SPEAKING` ficavam todos verdes e não dava para ler o que o robô estava fazendo.

Agora dá para ver as duas coisas ao mesmo tempo: rosto amarelo com olhos tristes é "o robô está pensando e percebeu que você está mal".

### Regra de geometria da boca

Tudo que a boca desenha precisa caber dentro de `MOUTH_BOX`, porque essa caixa é a única região que `drawMouth()` limpa. O que passar dela **nunca é apagado** e vira uma segunda boca permanente na tela.

```text
caixa de limpeza : y 72 .. 105
borda da cabeca  : y 106 .. 107   <- a caixa nao pode alcancar
olhos (desenho)  : ate y 69
```

Foi exatamente esse o defeito do sorriso original: arco de raio 24 centrado em `MOUTH_CY - 4` descia até y 107, deixando resíduo de y 102 a 107 e encostando na borda da cabeça. O `STATE_SPEAKING` tem trava de altura por isso — um visema mais alto que a caixa deixaria rastro.

### Estados (cor)

| Estado | Cor | Rodapé |
|---|---|---|
| `STATE_IDLE` | verde | Pronto |
| `STATE_LISTENING` | ciano | Ouvindo |
| `STATE_SENDING` | ciano | Enviando |
| `STATE_THINKING` | amarelo | Pensando |
| `STATE_SPEAKING` | verde | Falando |
| `STATE_ERROR` | vermelho | ERRO |

O estado é derivado no próprio firmware: `LISTENING` ao receber `RECORDING`, `THINKING` após `RECORD_END`, `SPEAKING` quando o playback começa de fato, `ERROR` nas falhas de SD e WebSocket. O servidor não precisa mandar nada para o canal de cor.

`PLAY_START` marca `THINKING`, não `SPEAKING`: naquele momento o áudio só está sendo recebido, e marcar `SPEAKING` fazia o robô gesticular durante o download.

### Humores (forma)

`MOOD_NEUTRAL`, `MOOD_HAPPY`, `MOOD_SAD`, `MOOD_CONFUSED`, `MOOD_EXCITED`, `MOOD_CONCERNED`.

A forma de humor é aplicada em `IDLE` e em `SPEAKING`. Em `SPEAKING` o piscar fica desligado, senão competiria com o lip sync. Nos demais estados a forma do olho é a do estado (pulso em `LISTENING`, spinner em `THINKING`, X em `ERROR`).

O humor não expira sozinho: ele vale até o próximo `EMO`. Como o servidor manda um `EMO` a cada interação, na prática ele é sempre atualizado.

## Spinner do "Pensando"

Em `STATE_THINKING` cada olho vira uma órbita com um ponto girando, mais duas caudas menores atrás dele. As caudas é que fazem o movimento ser lido como rotação; um ponto solto parece só pular de lugar.

```text
THINK_STEPS   8     posicoes na orbita, 45 graus cada
THINK_ORBIT_R 13    raio da orbita
THINK_DOT_R   4     raio do ponto principal (caudas: 3 e 2)
THINK_TAIL    3     ponto + 2 caudas
```

A extensão máxima é `ORBIT_R + DOT_R = 17 px` do centro, dentro dos 22 px de meia-caixa do olho. Uma volta completa leva 8 frames de 90 ms, ou seja 720 ms.

A versão anterior movia os olhos para os lados a cada 90 ms. O gesto estava errado: lia como "desconfiado" em vez de "processando", e a velocidade deixava o rosto nervoso.

Os pontinhos do rodapé avançam a cada 4 frames, não a cada frame. No mesmo ritmo do spinner eles piscavam rápido demais, e redesenhar o texto a 90 ms era tráfego de SPI sem ganho.

## Lip sync por visema

A boca durante `SPEAKING` é dirigida pelo áudio que está tocando, com resolução de 20 ms.

### Por que 20 ms

O chunk de playback tem 1920 amostras a 16 kHz = 120 ms. Se a boca fosse calculada uma vez por chunk, ela ficaria congelada durante toda a reprodução daquele pedaço — e sílaba humana dura 100 a 300 ms. Uma média sobre 120 ms achata exatamente a informação que se quer mostrar.

`PLAY_CHUNK_SAMPLES` é múltiplo exato de `VIS_WINDOW` de propósito. Se sobrar resto por chunk, a boca acumula adiantamento ao longo da fala.

### Os dois sinais

Uma passada por janela extrai ambos:

| Sinal | Custo | Controla |
|---|---|---|
| RMS normalizado por pico móvel | soma de quadrados + `sqrtf` | abertura (altura) |
| Zero-crossing rate | comparação de sinal | forma (largura) |

O pico móvel sobe imediatamente e decai devagar (`peakEnv >> 7` por janela). Isso evita limiares absolutos, que quebravam quando o servidor aplicava filtro de volume no Piper.

### Visemas

| Visema | Condição | Boca |
|---|---|---|
| `VIS_CLOSED` | energia < 8% | fechada, 4 px |
| `VIS_MMM` | energia baixa, grave | comprimida |
| `VIS_SS` | energia baixa, aguda | larga e fina |
| `VIS_EE` | energia média/alta, aguda | larga, média |
| `VIS_OH` | energia média, grave | redonda e estreita |
| `VIS_AH` | energia alta, grave | aberta e alta |

Os visemas de um chunk inteiro são calculados no momento da leitura do SD e empilhados numa FIFO. `tickMouth()` consome a FIFO a cada 20 ms e só redesenha quando a forma muda, o que evita 50 redraws por segundo no SPI.

O suavizador tem attack imediato e release de aproximadamente 3 frames. Sem isso a boca treme; com isso ela ganha peso.

### Constantes que precisam de calibração na placa

```c
#define VIS_ZCR_PCT 15      // % de cruzamentos que conta como sibilante
```

E os cortes de `lvl` dentro de `visemeFromWindow` (8, 20, 55). São ponto de partida e dependem do nível de saída real da voz Piper em uso.

## Playback sem gap

O laço de reprodução usa buffer duplo. Espera a fila do canal ter vaga, preenche o buffer que acabou de liberar e enfileira; a leitura do SD do próximo chunk acontece enquanto o atual ainda toca.

A ordem importa: esperar vaga **antes** de ler. `playRaw()` guarda o ponteiro em vez de copiar as amostras, então ler primeiro sobrescreveria um buffer que ainda está tocando.

Isso depende de `isPlaying(canal)` devolver `2` quando há um som tocando e outro enfileirado, que é o contrato do M5Unified.
